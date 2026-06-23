#include "main.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <limits>
#include <spawn.h>
#include <thread>
#include <sys/file.h>
#include <sys/wait.h>

extern char** environ;

namespace {

constexpr const char* kLogPath = "/tmp/zclean.log";
constexpr std::chrono::milliseconds kDryRunStepDelay{160};
constexpr std::chrono::milliseconds kLiveLogRefreshInterval{140};
constexpr int kLiveLogLines = 3;
constexpr int kLiveLogRowsAboveBar = 1 + 1 + kLiveLogLines + 1;
constexpr int kLiveLogPrefixColumns = 2;
constexpr std::size_t kMaxPendingLogLine = 4096;

volatile std::sig_atomic_t g_interrupted = 0;

enum class LogMode {
    Truncate,
    Append
};

struct Options {
    bool showHelp = false;
    bool showVersion = false;
    bool dryRun = false;
};

struct ProcessResult {
    int exitCode = 127;
    std::string output;
};

struct ReportAccess {
    bool valid = false;
    gid_t gid = 0;
};

struct ProcessConfig {
    bool captureStdout = false;
    bool logStdout = false;
    bool logStderr = false;
    bool discardStdout = true;
    bool discardStderr = true;
    LogMode logMode = LogMode::Append;
    std::string header;
    std::vector<std::pair<std::string, std::string>> environment;
};

struct SnapRevision {
    std::string name;
    std::string revision;
};

struct AppContext {
    std::string packageManager = "apt";
    std::string dnfCommand = "dnf";
    bool hasFlatpak = false;
    bool hasSnap = false;
};

struct CleanStatus {
    bool native = false;
    bool flatpak = false;
    bool snap = false;
    bool checkError = false;

    bool aptAutoremove = false;
    bool aptAutoclean = false;

    std::vector<std::string> zypperUnneededPackages;
    bool zypperPackageCache = false;

    std::vector<std::string> dnfUnneededPackages;
    bool dnfPackageCache = false;

    std::vector<std::string> flatpakUnusedApplications;
    bool flatpakCache = false;

    std::vector<SnapRevision> disabledSnapRevisions;
    bool snapCache = false;

    bool any() const {
        return native || flatpak || snap;
    }
};

struct Step {
    std::string label;
    std::function<bool()> action;
};

class FileDescriptor {
public:
    FileDescriptor() = default;
    explicit FileDescriptor(int fd) : fd_(fd) {}

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    ~FileDescriptor() {
        reset();
    }

    bool valid() const {
        return fd_ >= 0;
    }

    int get() const {
        return fd_;
    }

    void reset(int fd = -1) {
        if (fd_ >= 0) {
            close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

bool isSafeOwnedRegularFile(int fd) {
    struct stat st {};
    if (fstat(fd, &st) != 0) {
        return false;
    }

    return S_ISREG(st.st_mode) &&
           st.st_nlink == 1 &&
           st.st_uid == geteuid();
}

bool parseGid(const char* text, gid_t& gid) {
    if (text == nullptr || *text == '\0') {
        return false;
    }

    const std::string value(text);
    if (!std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isdigit(c);
        })) {
        return false;
    }

    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0') {
        return false;
    }

    if (parsed > static_cast<unsigned long>(std::numeric_limits<gid_t>::max())) {
        return false;
    }

    gid = static_cast<gid_t>(parsed);
    return true;
}

ReportAccess reportAccess() {
    ReportAccess access;

    if (geteuid() != 0) {
        return access;
    }

    gid_t sudoGid = 0;
    if (parseGid(getenv("SUDO_GID"), sudoGid)) {
        access.valid = true;
        access.gid = sudoGid;
    }

    return access;
}

class FileLock {
public:
    bool acquire() {
        const char* paths[] = {"/run/zclean.lock", "/tmp/zclean.lock"};
        bool busy = false;
        bool failed = false;

        for (const char* path : paths) {
            FileDescriptor fd(open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0644));
            if (!fd.valid()) {
                failed = true;
                continue;
            }

            if (!isSafeOwnedRegularFile(fd.get())) {
                failed = true;
                continue;
            }

            if (flock(fd.get(), LOCK_EX | LOCK_NB) == 0) {
                fd_ = std::move(fd);
                return true;
            }

            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                busy = true;
            } else {
                failed = true;
            }
        }

        if (busy) {
            std::cerr << RED << "Error: Another zclean instance is already running.\n" << RESET;
        } else if (failed) {
            std::cerr << RED << "Error: Cannot create lock file.\n" << RESET;
        }
        return false;
    }

    ~FileLock() {
        if (fd_.valid()) {
            flock(fd_.get(), LOCK_UN);
        }
    }

private:
    FileDescriptor fd_;
};

class SigintGuard {
public:
    SigintGuard() {
        struct sigaction sa {};
        sa.sa_handler = handleSigint;
        sigemptyset(&sa.sa_mask);
        installed_ = (sigaction(SIGINT, &sa, &previous_) == 0);
    }

    SigintGuard(const SigintGuard&) = delete;
    SigintGuard& operator=(const SigintGuard&) = delete;

    ~SigintGuard() {
        if (installed_) {
            sigaction(SIGINT, &previous_, nullptr);
        }
    }

private:
    static void handleSigint(int) {
        g_interrupted = 1;
    }

    bool installed_ = false;
    struct sigaction previous_ {};
};

class SpawnFileActions {
public:
    SpawnFileActions() {
        valid_ = (posix_spawn_file_actions_init(&actions_) == 0);
    }

    SpawnFileActions(const SpawnFileActions&) = delete;
    SpawnFileActions& operator=(const SpawnFileActions&) = delete;

    ~SpawnFileActions() {
        if (valid_) {
            posix_spawn_file_actions_destroy(&actions_);
        }
    }

    bool valid() const {
        return valid_;
    }

    posix_spawn_file_actions_t* get() {
        return &actions_;
    }

private:
    bool valid_ = false;
    posix_spawn_file_actions_t actions_ {};
};

std::string trim(const std::string& input) {
    const auto begin = input.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }

    const auto end = input.find_last_not_of(" \t\r\n");
    return input.substr(begin, end - begin + 1);
}

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

void printInfoHeader() {
    std::cout << CYAN << "[ZPM-INFO]" << RESET << "\n";
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::stringstream ss(text);
    std::string line;

    while (std::getline(ss, line)) {
        lines.push_back(line);
    }

    return lines;
}

std::vector<std::string> split(const std::string& text, char delimiter) {
    std::vector<std::string> parts;
    std::stringstream ss(text);
    std::string part;

    while (std::getline(ss, part, delimiter)) {
        parts.push_back(part);
    }

    return parts;
}

void addUnique(std::vector<std::string>& values, const std::string& value) {
    const std::string cleaned = trim(value);
    if (cleaned.empty()) {
        return;
    }

    if (std::find(values.begin(), values.end(), cleaned) == values.end()) {
        values.push_back(cleaned);
    }
}

std::vector<std::string> nonEmptyLines(const std::string& text) {
    std::vector<std::string> lines;
    for (const std::string& line : splitLines(text)) {
        addUnique(lines, line);
    }
    return lines;
}

bool isSafeToken(const std::string& value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](unsigned char c) {
               return std::isalnum(c) || c == '_' || c == '-' || c == '.';
           });
}

bool isDigits(const std::string& value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](unsigned char c) {
               return std::isdigit(c);
           });
}

bool writeAll(int fd, const char* data, size_t size) {
    while (size > 0) {
        const ssize_t written = write(fd, data, size);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        data += written;
        size -= static_cast<size_t>(written);
    }
    return true;
}

bool writeAll(int fd, const std::string& text) {
    return writeAll(fd, text.data(), text.size());
}

FileDescriptor openLog(LogMode mode) {
    const int flags = O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW |
                      (mode == LogMode::Append ? O_APPEND : 0);

    FileDescriptor fd(open(kLogPath, flags, 0600));
    if (!fd.valid()) {
        return {};
    }

    if (!isSafeOwnedRegularFile(fd.get())) {
        return {};
    }

    if (mode == LogMode::Truncate && ftruncate(fd.get(), 0) != 0) {
        return {};
    }

    const ReportAccess access = reportAccess();
    if (access.valid && fchown(fd.get(), geteuid(), access.gid) != 0) {
        return {};
    }

    const mode_t modeBits = access.valid ? 0640 : 0600;
    if (fchmod(fd.get(), modeBits) != 0) {
        return {};
    }

    return fd;
}

bool writeLogLine(const std::string& line, LogMode mode = LogMode::Append) {
    FileDescriptor fd = openLog(mode);
    if (!fd.valid()) {
        return false;
    }
    return writeAll(fd.get(), line + "\n");
}

bool writeLogHeader(const std::string& header, LogMode mode = LogMode::Append) {
    return writeLogLine(header, mode);
}

class LiveLogView {
public:
    explicit LiveLogView(const char* path, bool enabled)
        : path_(path), enabled_(enabled) {}

    LiveLogView(const LiveLogView&) = delete;
    LiveLogView& operator=(const LiveLogView&) = delete;

    ~LiveLogView() {
        stop();
    }

    void start() {
        if (!enabled_ || started_) {
            return;
        }

        offset_ = currentFileSize();
        running_ = true;
        started_ = true;
        stopped_ = false;

        try {
            worker_ = std::thread(&LiveLogView::run, this);
        } catch (...) {
            running_ = false;
            started_ = false;
            stopped_ = true;
        }
    }

    void stop() {
        if (!started_ || stopped_) {
            return;
        }

        stopped_ = true;
        running_ = false;
        if (worker_.joinable()) {
            worker_.join();
        }

        readNewLogData();
        draw();
    }

    void prepareForInfoAppendLocked() {
        if (!rowsReserved_) {
            std::cout << "\r\033[K";
            return;
        }

        std::cout << "\r\033[K"
                  << "\033[" << kLiveLogRowsAboveBar << "A\r\033[K";
        rowsReserved_ = false;
    }

    void drawAtCursorLocked() {
        const std::vector<std::string> lines = displayLines();
        const int terminalColumns = zpm::progressbar_detail::terminalWidth();
        const std::string header =
            zpm::progressbar_detail::sanitizeTask("[ZPM-LOG]", terminalColumns);
        const std::string rowPrefix =
            terminalColumns >= kLiveLogPrefixColumns ? "> "
                                                     : (terminalColumns > 0 ? ">" : "");
        const int textColumns =
            std::max(0, terminalColumns - static_cast<int>(rowPrefix.size()));

        std::cout << "\r\033[K\n"
                  << "\r\033[K";
        if (!header.empty()) {
            std::cout << CYAN << header << RESET;
        }
        std::cout << "\n";

        for (int row = 0; row < kLiveLogLines; ++row) {
            std::cout << "\r\033[K";
            if (row < static_cast<int>(lines.size())) {
                if (!rowPrefix.empty()) {
                    std::cout << CYAN << rowPrefix << RESET;
                }
                std::cout << zpm::progressbar_detail::sanitizeTask(lines[static_cast<std::size_t>(row)],
                                                                   textColumns);
            }
            std::cout << "\n";
        }

        std::cout << "\r\033[K\n" << std::flush;
        rowsReserved_ = true;
    }

private:
    off_t currentFileSize() const {
        FileDescriptor fd(open(path_, O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        if (!fd.valid() || !isSafeOwnedRegularFile(fd.get())) {
            return 0;
        }

        struct stat st {};
        if (fstat(fd.get(), &st) != 0) {
            return 0;
        }

        return st.st_size;
    }

    void addLine(const std::string& rawLine) {
        std::string line = trim(rawLine);
        if (line.empty()) {
            return;
        }

        recentLines_.push_back(std::move(line));
        if (recentLines_.size() > static_cast<std::size_t>(kLiveLogLines)) {
            recentLines_.erase(recentLines_.begin());
        }
    }

    void flushPendingLine() {
        addLine(pendingLine_);
        pendingLine_.clear();
    }

    bool readNewLogData() {
        FileDescriptor fd(open(path_, O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        if (!fd.valid() || !isSafeOwnedRegularFile(fd.get())) {
            return false;
        }

        struct stat st {};
        if (fstat(fd.get(), &st) != 0) {
            return false;
        }

        if (st.st_size < offset_) {
            offset_ = 0;
            pendingLine_.clear();
            recentLines_.clear();
        }

        if (lseek(fd.get(), offset_, SEEK_SET) < 0) {
            return false;
        }

        bool changed = false;
        std::array<char, 4096> buffer {};

        for (;;) {
            const ssize_t count = read(fd.get(), buffer.data(), buffer.size());
            if (count > 0) {
                offset_ += count;
                changed = true;

                for (ssize_t i = 0; i < count; ++i) {
                    const unsigned char ch = static_cast<unsigned char>(buffer[static_cast<std::size_t>(i)]);
                    if (ch == '\n' || ch == '\r') {
                        flushPendingLine();
                    } else if (ch == '\t') {
                        pendingLine_ += ' ';
                    } else if (ch >= 32 && ch != 127 && pendingLine_.size() < kMaxPendingLogLine) {
                        pendingLine_ += static_cast<char>(ch);
                    }
                }
                continue;
            }

            if (count == 0) {
                break;
            }

            if (errno == EINTR) {
                continue;
            }

            break;
        }

        return changed;
    }

    std::vector<std::string> displayLines() const {
        std::vector<std::string> lines = recentLines_;
        const std::string partial = trim(pendingLine_);
        if (!partial.empty()) {
            if (lines.size() == static_cast<std::size_t>(kLiveLogLines)) {
                lines.erase(lines.begin());
            }
            lines.push_back(partial);
        }
        return lines;
    }

    void draw() {
        std::lock_guard<std::mutex> outputLock(zpm::progressbar_detail::outputMutex());
        if (!rowsReserved_) {
            return;
        }

        std::cout << "\033[s"
                  << "\033[" << kLiveLogRowsAboveBar << "A\r";
        drawAtCursorLocked();
        std::cout << "\033[u" << std::flush;
    }

    void run() {
        while (running_) {
            if (readNewLogData() || !pendingLine_.empty()) {
                draw();
            }

            std::this_thread::sleep_for(kLiveLogRefreshInterval);
        }
    }

    const char* path_;
    bool enabled_ = false;
    bool started_ = false;
    bool stopped_ = true;
    bool rowsReserved_ = false;
    std::atomic<bool> running_{false};
    std::thread worker_;
    off_t offset_ = 0;
    std::string pendingLine_;
    std::vector<std::string> recentLines_;
};

void beginCleanStep(float progress,
                    const std::string& progressText,
                    int& step,
                    const std::string& infoText,
                    LiveLogView* liveLog = nullptr,
                    bool showProgress = true) {
    const bool startProgressbar = step == 0;

    {
        std::lock_guard<std::mutex> outputLock(zpm::progressbar_detail::outputMutex());

        if (liveLog != nullptr) {
            liveLog->prepareForInfoAppendLocked();
        } else {
            std::cout << "\r\033[K";
        }

        const int terminalColumns = zpm::progressbar_detail::terminalWidth();
        const std::string prefix =
            zpm::progressbar_detail::sanitizeTask("[>] ", terminalColumns);
        const int textColumns =
            std::max(0, terminalColumns - static_cast<int>(prefix.size()));

        if (!prefix.empty()) {
            std::cout << CYAN << prefix << RESET;
        }
        std::cout << zpm::progressbar_detail::sanitizeTask(infoText, textColumns) << "\n";

        if (liveLog != nullptr) {
            liveLog->drawAtCursorLocked();
        }

        std::cout << std::flush;
    }

    ++step;
    if (!showProgress) {
        return;
    }

    if (startProgressbar) {
        progressbar_start(progress, progressText);
    } else {
        progressbar_update(progress, progressText);
    }
}

int decodeExitStatus(int status) {
    if (status == -1) {
        return 127;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 127;
}

int waitForChild(pid_t pid) {
    int status = 0;
    bool signalSent = false;

    for (;;) {
        const pid_t result = waitpid(pid, &status, 0);
        if (result == pid) {
            return decodeExitStatus(status);
        }

        if (result < 0 && errno == EINTR) {
            if (g_interrupted && !signalSent) {
                kill(pid, SIGINT);
                signalSent = true;
            }
            continue;
        }

        return 127;
    }
}

std::vector<std::string> buildEnvironmentStorage(
    const std::vector<std::pair<std::string, std::string>>& overrides) {
    std::vector<std::string> storage;

    for (char** current = environ; current != nullptr && *current != nullptr; ++current) {
        const std::string entry(*current);
        const auto separator = entry.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        const std::string key = entry.substr(0, separator);
        const auto overridden = std::find_if(overrides.begin(), overrides.end(),
                                             [&](const auto& item) {
                                                 return item.first == key;
                                             });
        if (overridden == overrides.end()) {
            storage.push_back(entry);
        }
    }

    for (const auto& [key, value] : overrides) {
        storage.push_back(key + "=" + value);
    }

    return storage;
}

std::vector<char*> buildPointerArray(std::vector<std::string>& values) {
    std::vector<char*> pointers;
    pointers.reserve(values.size() + 1);

    for (std::string& value : values) {
        pointers.push_back(value.data());
    }

    pointers.push_back(nullptr);
    return pointers;
}

bool addDevNullRedirect(SpawnFileActions& actions, int targetFd) {
    return posix_spawn_file_actions_addopen(actions.get(),
                                            targetFd,
                                            "/dev/null",
                                            O_WRONLY,
                                            0) == 0;
}

bool addDupRedirect(SpawnFileActions& actions, int sourceFd, int targetFd) {
    return posix_spawn_file_actions_adddup2(actions.get(), sourceFd, targetFd) == 0;
}

void logSpawnFailure(const std::vector<std::string>& args, int errorCode) {
    const std::string command = args.empty() ? "<empty>" : args.front();
    writeLogLine("process: cannot start " + command + ": " + std::strerror(errorCode));
}

ProcessResult runProcess(const std::vector<std::string>& args, const ProcessConfig& config = {}) {
    if (args.empty()) {
        return {};
    }

    int pipeFd[2] = {-1, -1};
    FileDescriptor readEnd;
    FileDescriptor writeEnd;

    if (config.captureStdout) {
        if (pipe(pipeFd) != 0) {
            return {};
        }
        fcntl(pipeFd[0], F_SETFD, FD_CLOEXEC);
        fcntl(pipeFd[1], F_SETFD, FD_CLOEXEC);
        readEnd.reset(pipeFd[0]);
        writeEnd.reset(pipeFd[1]);
    }

    const bool needsLog = config.logStdout ||
                          config.logStderr ||
                          !config.header.empty();

    FileDescriptor log;
    if (needsLog) {
        log = openLog(config.logMode);
        if (!log.valid()) {
            return {};
        }
        if (!config.header.empty()) {
            writeAll(log.get(), config.header + "\n");
        }
    }

    SpawnFileActions actions;
    if (!actions.valid()) {
        return {};
    }

    bool actionsOk = true;

    if (config.captureStdout) {
        actionsOk = actionsOk &&
                    addDupRedirect(actions, writeEnd.get(), STDOUT_FILENO) &&
                    posix_spawn_file_actions_addclose(actions.get(), readEnd.get()) == 0 &&
                    posix_spawn_file_actions_addclose(actions.get(), writeEnd.get()) == 0;
    } else if (config.logStdout && log.valid()) {
        actionsOk = actionsOk && addDupRedirect(actions, log.get(), STDOUT_FILENO);
    } else if (config.discardStdout) {
        actionsOk = actionsOk && addDevNullRedirect(actions, STDOUT_FILENO);
    }

    if (config.logStderr && log.valid()) {
        actionsOk = actionsOk && addDupRedirect(actions, log.get(), STDERR_FILENO);
    } else if (config.discardStderr) {
        actionsOk = actionsOk && addDevNullRedirect(actions, STDERR_FILENO);
    }

    if (log.valid()) {
        actionsOk = actionsOk &&
                    posix_spawn_file_actions_addclose(actions.get(), log.get()) == 0;
    }

    if (!actionsOk) {
        return {};
    }

    std::vector<std::string> argvStorage = args;
    std::vector<char*> argv = buildPointerArray(argvStorage);

    std::vector<std::string> envStorage;
    std::vector<char*> envp;
    char* const* environment = environ;
    if (!config.environment.empty()) {
        envStorage = buildEnvironmentStorage(config.environment);
        envp = buildPointerArray(envStorage);
        environment = envp.data();
    }

    pid_t pid = -1;
    const int spawnError = posix_spawnp(&pid,
                                        args.front().c_str(),
                                        actions.get(),
                                        nullptr,
                                        argv.data(),
                                        environment);
    if (spawnError != 0) {
        logSpawnFailure(args, spawnError);
        return {};
    }

    if (config.captureStdout) {
        writeEnd.reset();
    }

    ProcessResult result;
    if (config.captureStdout) {
        std::array<char, 4096> buffer {};

        for (;;) {
            const ssize_t count = read(readEnd.get(), buffer.data(), buffer.size());
            if (count > 0) {
                result.output.append(buffer.data(), static_cast<size_t>(count));
                continue;
            }

            if (count == 0) {
                break;
            }

            if (errno == EINTR) {
                if (g_interrupted) {
                    kill(pid, SIGINT);
                }
                continue;
            }

            break;
        }
    }

    result.exitCode = waitForChild(pid);
    result.output = trim(result.output);
    return result;
}

std::vector<std::pair<std::string, std::string>> cLocaleEnv() {
    return {{"LC_ALL", "C"}};
}

std::vector<std::pair<std::string, std::string>> aptEnv() {
    return {{"LC_ALL", "C"}, {"DEBIAN_FRONTEND", "noninteractive"}};
}

ProcessResult capture(const std::vector<std::string>& args,
                      const std::vector<std::pair<std::string, std::string>>& environment = cLocaleEnv()) {
    ProcessConfig config;
    config.captureStdout = true;
    config.environment = environment;
    return runProcess(args, config);
}

int runLogged(const std::vector<std::string>& args,
              const std::string& header,
              const std::vector<std::pair<std::string, std::string>>& environment = cLocaleEnv()) {
    ProcessConfig config;
    config.logStdout = true;
    config.logStderr = true;
    config.header = header;
    config.environment = environment;
    return runProcess(args, config).exitCode;
}

bool executableAt(const std::string& path) {
    return access(path.c_str(), X_OK) == 0;
}

bool commandExists(const std::string& command) {
    if (command.find('/') != std::string::npos) {
        return executableAt(command);
    }

    const char* pathEnv = getenv("PATH");
    const std::string path = pathEnv != nullptr
        ? pathEnv
        : "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";

    std::stringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty()) {
            dir = ".";
        }
        if (executableAt(dir + "/" + command)) {
            return true;
        }
    }

    return false;
}

bool outputHasLinePrefix(const std::string& output, const std::string& prefix) {
    for (const std::string& line : splitLines(output)) {
        if (startsWith(line, prefix)) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> parseZypperUnneededPackages(const std::string& output) {
    std::vector<std::string> packages;

    for (const std::string& line : splitLines(output)) {
        const auto columns = split(line, '|');
        if (columns.size() < 3) {
            continue;
        }

        if (trim(columns[0]).find('i') != std::string::npos) {
            addUnique(packages, columns[2]);
        }
    }

    return packages;
}

std::vector<std::string> parseFlatpakApplications(const std::string& output) {
    std::vector<std::string> apps;

    for (const std::string& rawLine : splitLines(output)) {
        const std::string line = trim(rawLine);
        if (line.empty() || line == "Application") {
            continue;
        }
        addUnique(apps, line);
    }

    return apps;
}

std::vector<SnapRevision> parseDisabledSnapRevisions(const std::string& output) {
    std::vector<SnapRevision> revisions;

    for (const std::string& rawLine : splitLines(output)) {
        const std::string line = trim(rawLine);
        const std::string lower = toLower(line);

        if (line.empty() ||
            startsWith(lower, "name ") ||
            lower.find("disabled") == std::string::npos) {
            continue;
        }

        std::istringstream ss(line);
        std::string name;
        std::string version;
        std::string revision;

        if ((ss >> name >> version >> revision) &&
            isSafeToken(name) &&
            isDigits(revision)) {
            revisions.push_back({name, revision});
        }
    }

    return revisions;
}

bool directoryHasEntries(const std::filesystem::path& directory) {
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec)) {
        return false;
    }

    std::filesystem::directory_iterator iterator(directory, ec);
    if (ec) {
        writeLogLine("check: cannot iterate " + directory.string() + ": " + ec.message());
        return false;
    }

    return iterator != std::filesystem::directory_iterator {};
}

bool directoryHasEntryWithPrefix(const std::filesystem::path& directory,
                                 const std::string& prefix) {
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec)) {
        return false;
    }

    std::filesystem::directory_iterator iterator(directory, ec);
    if (ec) {
        writeLogLine("check: cannot iterate " + directory.string() + ": " + ec.message());
        return false;
    }

    for (const auto& entry : iterator) {
        if (startsWith(entry.path().filename().string(), prefix)) {
            return true;
        }
    }

    return false;
}

bool directoryHasRegularFile(const std::filesystem::path& directory,
                             const std::string& extension = {}) {
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec)) {
        return false;
    }

    std::filesystem::recursive_directory_iterator iterator(
        directory,
        std::filesystem::directory_options::skip_permission_denied,
        ec
    );
    if (ec) {
        writeLogLine("check: cannot iterate " + directory.string() + ": " + ec.message());
        return false;
    }

    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end) {
        std::error_code statusError;
        const auto status = iterator->symlink_status(statusError);
        if (!statusError && std::filesystem::is_regular_file(status)) {
            if (extension.empty() || iterator->path().extension() == extension) {
                return true;
            }
        }

        iterator.increment(ec);
        if (ec) {
            ec.clear();
        }
    }

    return false;
}

bool isRootOwnedRemovableEntry(const std::filesystem::path& path) {
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0) {
        return false;
    }

    return st.st_uid == 0 &&
           (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode));
}

bool removeDirectoryEntries(const std::filesystem::path& directory) {
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec)) {
        return true;
    }

    std::filesystem::directory_iterator iterator(directory, ec);
    if (ec) {
        writeLogLine("cleanup: cannot iterate " + directory.string() + ": " + ec.message());
        return false;
    }

    bool ok = true;
    for (const auto& entry : iterator) {
        if (!isRootOwnedRemovableEntry(entry.path())) {
            writeLogLine("cleanup: skipped unsafe entry " + entry.path().string());
            ok = false;
            continue;
        }

        std::error_code removeError;
        std::filesystem::remove_all(entry.path(), removeError);
        if (removeError) {
            writeLogLine("cleanup: cannot remove " + entry.path().string() + ": " +
                         removeError.message());
            ok = false;
        } else {
            writeLogLine("cleanup: removed " + entry.path().string());
        }
    }

    return ok;
}

bool removeEntriesWithPrefix(const std::filesystem::path& directory,
                             const std::string& prefix) {
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec)) {
        return true;
    }

    std::filesystem::directory_iterator iterator(directory, ec);
    if (ec) {
        writeLogLine("cleanup: cannot iterate " + directory.string() + ": " + ec.message());
        return false;
    }

    bool ok = true;
    for (const auto& entry : iterator) {
        const std::string filename = entry.path().filename().string();
        if (!startsWith(filename, prefix)) {
            continue;
        }

        if (!isRootOwnedRemovableEntry(entry.path())) {
            writeLogLine("cleanup: skipped unsafe entry " + entry.path().string());
            ok = false;
            continue;
        }

        std::error_code removeError;
        std::filesystem::remove_all(entry.path(), removeError);
        if (removeError) {
            writeLogLine("cleanup: cannot remove " + entry.path().string() + ": " +
                         removeError.message());
            ok = false;
        } else {
            writeLogLine("cleanup: removed " + entry.path().string());
        }
    }

    return ok;
}

void recordCheckFailure(CleanStatus& status,
                        const std::string& command,
                        int exitCode) {
    status.checkError = true;
    writeLogLine("check: " + command + " failed with exit code " + std::to_string(exitCode));
}

void recordOptionalCheckFailure(const std::string& command, int exitCode) {
    writeLogLine("optional check: " + command +
                 " failed with exit code " + std::to_string(exitCode) +
                 " - skipped");
}

void checkAptClean(CleanStatus& status) {
    const ProcessResult autoremove = capture({"apt-get", "-s", "autoremove"}, aptEnv());
    if (autoremove.exitCode == 0) {
        status.aptAutoremove = outputHasLinePrefix(autoremove.output, "Remv ");
    } else {
        recordCheckFailure(status, "apt-get -s autoremove", autoremove.exitCode);
    }

    const ProcessResult autoclean = capture({"apt-get", "-s", "autoclean"}, aptEnv());
    if (autoclean.exitCode == 0) {
        status.aptAutoclean = outputHasLinePrefix(autoclean.output, "Del ");
    } else {
        recordCheckFailure(status, "apt-get -s autoclean", autoclean.exitCode);
    }

    status.native = status.aptAutoremove || status.aptAutoclean;
}

void checkZypperClean(CleanStatus& status) {
    const ProcessResult unneeded = capture({"zypper", "packages", "--unneeded"});
    if (unneeded.exitCode == 0) {
        status.zypperUnneededPackages = parseZypperUnneededPackages(unneeded.output);
    } else {
        recordCheckFailure(status, "zypper packages --unneeded", unneeded.exitCode);
    }

    status.zypperPackageCache = directoryHasRegularFile("/var/cache/zypp/packages");
    status.native = !status.zypperUnneededPackages.empty() || status.zypperPackageCache;
}

void checkDnfClean(CleanStatus& status, const AppContext& context) {
    const ProcessResult unneeded = capture({context.dnfCommand, "repoquery", "--unneeded", "-q"});
    if (unneeded.exitCode == 0) {
        status.dnfUnneededPackages = nonEmptyLines(unneeded.output);
    } else {
        recordCheckFailure(status,
                           context.dnfCommand + " repoquery --unneeded -q",
                           unneeded.exitCode);
    }

    status.dnfPackageCache =
        directoryHasRegularFile("/var/cache/dnf", ".rpm") ||
        directoryHasRegularFile("/var/cache/libdnf5", ".rpm");
    status.native = !status.dnfUnneededPackages.empty() || status.dnfPackageCache;
}

void checkUniversalClean(CleanStatus& status, const AppContext& context) {
    if (context.hasFlatpak) {
        const ProcessResult unused = capture({"flatpak", "list", "--unused", "--columns=application"});
        if (unused.exitCode == 0) {
            status.flatpakUnusedApplications = parseFlatpakApplications(unused.output);
        } else {
            recordOptionalCheckFailure("flatpak list --unused --columns=application",
                                       unused.exitCode);
        }

        status.flatpakCache = directoryHasEntryWithPrefix("/var/tmp", "flatpak-cache-");
        status.flatpak = !status.flatpakUnusedApplications.empty() || status.flatpakCache;
    }

    if (context.hasSnap) {
        const ProcessResult allSnaps = capture({"snap", "list", "--all"});
        if (allSnaps.exitCode == 0) {
            status.disabledSnapRevisions = parseDisabledSnapRevisions(allSnaps.output);
        } else {
            recordOptionalCheckFailure("snap list --all", allSnaps.exitCode);
        }

        status.snapCache = directoryHasEntries("/var/lib/snapd/cache");
        status.snap = !status.disabledSnapRevisions.empty() || status.snapCache;
    }
}

CleanStatus checkCleanStatus(const AppContext& context) {
    CleanStatus status;

    if (context.packageManager == "apt") {
        checkAptClean(status);
    } else if (context.packageManager == "zypper") {
        checkZypperClean(status);
    } else if (context.packageManager == "dnf") {
        checkDnfClean(status, context);
    }

    checkUniversalClean(status, context);
    return status;
}

std::string nativeShortLabel(const AppContext& context) {
    if (context.packageManager == "zypper") {
        return "Zypper";
    }
    if (context.packageManager == "dnf") {
        return context.dnfCommand == "dnf5" ? "DNF5" : "DNF";
    }
    return "APT";
}

bool aptPrepare() {
    return runLogged({"dpkg", "--configure", "-a"},
                     "-----checking_system_consistency-----",
                     aptEnv()) == 0;
}

bool cleanAptNative(const CleanStatus& status) {
    bool ok = true;

    if (status.aptAutoremove) {
        ok = runLogged({"apt-get", "autoremove", "-y"},
                       "-----apt_autoremove-----",
                       aptEnv()) == 0 && ok;
    }

    if (status.aptAutoclean) {
        ok = runLogged({"apt-get", "autoclean"},
                       "-----apt_autoclean-----",
                       aptEnv()) == 0 && ok;
    }

    return ok;
}

bool rpmPrepare() {
    return runLogged({"rpm", "--rebuilddb"}, "-----checking_system_consistency-----") == 0;
}

bool cleanZypperCache() {
    return runLogged({"zypper", "clean", "-a"}, "-----zypper_clean_all-----") == 0;
}

bool removeZypperUnneeded(const std::vector<std::string>& packages) {
    if (packages.empty()) {
        return true;
    }

    constexpr size_t kBatchSize = 64;
    bool ok = true;

    for (size_t offset = 0; offset < packages.size(); offset += kBatchSize) {
        std::vector<std::string> args = {"zypper", "--non-interactive", "remove", "-y"};
        const size_t end = std::min(packages.size(), offset + kBatchSize);
        for (size_t i = offset; i < end; ++i) {
            args.push_back(packages[i]);
        }

        ok = runLogged(args,
                       "-----zypper_remove_unneeded_" + std::to_string(offset / kBatchSize + 1) +
                           "-----") == 0 && ok;
    }

    return ok;
}

bool cleanDnfNative(const AppContext& context, const CleanStatus& status) {
    bool ok = true;

    if (!status.dnfUnneededPackages.empty()) {
        ok = runLogged({context.dnfCommand, "autoremove", "-y"},
                       "-----" + context.dnfCommand + "_autoremove-----") == 0 && ok;
    }

    if (context.dnfCommand == "dnf5") {
        ok = runLogged({context.dnfCommand, "clean", "all"},
                       "-----dnf5_clean_all-----") == 0 && ok;
        return ok;
    }

    ok = runLogged({context.dnfCommand, "clean", "packages"},
                   "-----dnf_clean_packages-----") == 0 && ok;
    ok = runLogged({context.dnfCommand, "clean", "metadata"},
                   "-----dnf_clean_metadata-----") == 0 && ok;
    ok = runLogged({context.dnfCommand, "clean", "dbcache"},
                   "-----dnf_clean_dbcache-----") == 0 && ok;
    return ok;
}

bool cleanFlatpak(const CleanStatus& status) {
    bool ok = true;

    if (!status.flatpakUnusedApplications.empty()) {
        ok = runLogged({"flatpak", "uninstall", "--unused", "-y"},
                       "-----flatpak_remove_unused-----") == 0 && ok;
    }

    if (status.flatpakCache) {
        ok = writeLogHeader("-----flatpak_cache_cleanup-----") && ok;
        ok = removeEntriesWithPrefix("/var/tmp", "flatpak-cache-") && ok;
    }

    return ok;
}

bool cleanSnap(const CleanStatus& status) {
    bool ok = true;

    for (const SnapRevision& revision : status.disabledSnapRevisions) {
        ok = runLogged({"snap", "remove", revision.name, "--revision=" + revision.revision},
                       "-----snap_remove_" + revision.name + "_" + revision.revision + "-----") == 0 && ok;
        if (g_interrupted) {
            return false;
        }
    }

    if (status.snapCache) {
        ok = writeLogHeader("-----snap_cache_cleanup-----") && ok;
        ok = removeDirectoryEntries("/var/lib/snapd/cache") && ok;
    }

    return ok;
}

std::vector<Step> buildSteps(const AppContext& context, const CleanStatus& status) {
    std::vector<Step> steps;

    if (status.native && context.packageManager == "apt") {
        steps.push_back({"APT: prepare package database", aptPrepare});
        steps.push_back({"APT: autoremove/autoclean", [&status] {
            return cleanAptNative(status);
        }});
    } else if (status.native && context.packageManager == "zypper") {
        steps.push_back({"RPM: rebuild package database", rpmPrepare});

        if (status.zypperPackageCache) {
            steps.push_back({"Zypper: clean package cache", cleanZypperCache});
        }

        if (!status.zypperUnneededPackages.empty()) {
            steps.push_back({"Zypper: remove unneeded packages", [&status] {
                return removeZypperUnneeded(status.zypperUnneededPackages);
            }});
        }
    } else if (status.native && context.packageManager == "dnf") {
        steps.push_back({"RPM: rebuild package database", rpmPrepare});
        steps.push_back({nativeShortLabel(context) + ": autoremove/clean", [&context, &status] {
            return cleanDnfNative(context, status);
        }});
    }

    if (status.flatpak) {
        steps.push_back({"Flatpak: remove unused/cache", [&status] {
            return cleanFlatpak(status);
        }});
    }

    if (status.snap) {
        steps.push_back({"Snap: remove disabled/cache", [&status] {
            return cleanSnap(status);
        }});
    }

    return steps;
}

int runSteps(const std::vector<Step>& steps) {
    const int total = static_cast<int>(steps.size());
    bool anyFailed = false;

    if (!writeLogHeader("-----zclean_start-----")) {
        std::cout << RED << "Error: Cannot write to log file at "
                  << kLogPath << ".\n" << RESET;
        return 1;
    }

    int infoStep = 0;
    printInfoHeader();
    LiveLogView liveLog(kLogPath, true);
    liveLog.start();

    for (int i = 0; i < total; ++i) {
        if (g_interrupted) {
            liveLog.stop();
            progressbar_finish("Cancelled!");
            std::cout << YELLOW << "Cancelled.\n" << RESET;
            return 130;
        }

        const Step& step = steps[static_cast<size_t>(i)];
        const float startPct = (100.0f * static_cast<float>(i)) / static_cast<float>(total);
        const float endPct = (100.0f * static_cast<float>(i + 1)) / static_cast<float>(total);
        const std::string prefix = std::to_string(i + 1) + "/" + std::to_string(total) +
                                   " | " + step.label;

        beginCleanStep(startPct, prefix, infoStep, step.label, &liveLog);
        const bool ok = step.action();

        if (g_interrupted) {
            liveLog.stop();
            progressbar_finish("Cancelled!");
            std::cout << YELLOW << "Cancelled.\n" << RESET;
            return 130;
        }

        progressbar_update(endPct, prefix + (ok ? " - done" : " - failed"));
        anyFailed = !ok || anyFailed;
    }

    if (anyFailed) {
        liveLog.stop();
        progressbar_finish("Done with errors!");
        std::cout << RED << "Cleaning finished with errors!\n" << RESET;
        std::cout << YELLOW << "[RAPORT] " << RESET << kLogPath << "\n";
        return 1;
    }

    liveLog.stop();
    progressbar_finish("Done!");
    std::cout << GREEN << "Cleaning complete!\n" << RESET;
    std::cout << YELLOW << "[RAPORT] " << RESET << kLogPath << "\n";
    return 0;
}

void printHelp(const char* progName) {
    std::cout << RED << "Usage: " << RESET << progName
              << " [options] or zpm clean [options]\n"
              << RED << "Options:\n" << RESET
              << "  --dry-run      Simulate cleanup flow; no files are changed\n"
              << "  --version, -v  Show version information\n"
              << "  --help,    -h  Show this help message\n";
}

void printVersion() {
    std::cout << RED << "zclean component version: v" << zpm_version::version()
              << " of ZPM\n" << RESET
              << "https://github.com/Zielina-Konrad-productions/ZPM\n"
              << "Copyright (c) 2026 Ignacyyy & Ry3ball\nLicense: MIT\n";
}

bool parseOptions(int argc, char* argv[], Options& options) {
    std::vector<std::string> errors;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            options.showHelp = true;
        } else if (arg == "--version" || arg == "-v") {
            options.showVersion = true;
        } else if (arg == "--dry-run") {
            options.dryRun = true;
        } else if (startsWith(arg, "-")) {
            errors.push_back("Unknown option: " + arg);
        } else {
            errors.push_back("zclean does not accept package arguments: " + arg);
        }
    }

    if ((options.showHelp || options.showVersion) && options.dryRun) {
        errors.push_back("--help and --version can only be combined with each other.");
    }

    for (const std::string& error : errors) {
        std::cerr << RED << "Error: " << error << RESET << "\n";
    }

    return errors.empty();
}

AppContext buildContext() {
    AppContext context;
    context.packageManager = get_package_manager();
    context.dnfCommand = commandExists("dnf5") ? "dnf5" : "dnf";
    context.hasFlatpak = commandExists("flatpak");
    context.hasSnap = commandExists("snap");
    return context;
}

void printCleanSummary(const AppContext& context, const CleanStatus& status) {
    std::cout << RED << "cleaning system...." << RESET << "\n\n";

    if (status.native) {
        std::cout << YELLOW << "[N] " << RESET << nativeShortLabel(context) << " cleanup needed";
        if (context.packageManager == "apt") {
            std::cout << " (";
            bool separator = false;
            if (status.aptAutoremove) {
                std::cout << "autoremove";
                separator = true;
            }
            if (status.aptAutoclean) {
                std::cout << (separator ? ", " : "") << "autoclean";
            }
            std::cout << ")";
        } else if (context.packageManager == "zypper") {
            std::cout << " (" << status.zypperUnneededPackages.size()
                      << " unneeded package"
                      << (status.zypperUnneededPackages.size() == 1 ? "" : "s");
            if (status.zypperPackageCache) {
                std::cout << ", cache";
            }
            std::cout << ")";
        } else if (context.packageManager == "dnf") {
            std::cout << " (" << status.dnfUnneededPackages.size()
                      << " unneeded package"
                      << (status.dnfUnneededPackages.size() == 1 ? "" : "s");
            if (status.dnfPackageCache) {
                std::cout << ", cache";
            }
            std::cout << ")";
        }
        std::cout << "\n";
    }

    if (status.flatpak) {
        std::cout << YELLOW << "[F] " << RESET << "Flatpak cleanup needed";
        std::cout << " (" << status.flatpakUnusedApplications.size()
                  << " unused app"
                  << (status.flatpakUnusedApplications.size() == 1 ? "" : "s");
        if (status.flatpakCache) {
            std::cout << ", cache";
        }
        std::cout << ")\n";
    }

    if (status.snap) {
        std::cout << YELLOW << "[S] " << RESET << "Snap cleanup needed";
        std::cout << " (" << status.disabledSnapRevisions.size()
                  << " disabled revision"
                  << (status.disabledSnapRevisions.size() == 1 ? "" : "s");
        if (status.snapCache) {
            std::cout << ", cache";
        }
        std::cout << ")\n";
    }

    if (status.checkError) {
        std::cout << YELLOW << "Warning: Some checks failed. Details: "
                  << kLogPath << RESET << "\n";
    }

    std::cout << "\n";
}

bool dryRunStep() {
    std::this_thread::sleep_for(kDryRunStepDelay);
    return !g_interrupted;
}

AppContext buildDryRunContext() {
    AppContext context = buildContext();
    if (context.packageManager == "unknown") {
        context.packageManager = "apt";
    }
    return context;
}

std::vector<std::string> dryRunLabels(const AppContext& context) {
    std::vector<std::string> labels;

    labels.push_back("checking cleanup status");

    if (context.packageManager == "zypper") {
        labels.push_back("RPM: rebuild package database");
        labels.push_back("Zypper: clean package cache");
        labels.push_back("Zypper: remove unneeded packages");
    } else if (context.packageManager == "dnf") {
        labels.push_back("RPM: rebuild package database");
        labels.push_back(nativeShortLabel(context) + ": autoremove/clean");
    } else {
        labels.push_back("APT: prepare package database");
        labels.push_back("APT: autoremove/autoclean");
    }

    if (context.hasFlatpak) {
        labels.push_back("Flatpak: remove unused/cache");
    }

    if (context.hasSnap) {
        labels.push_back("Snap: remove disabled/cache");
    }

    labels.push_back("cleaning report");
    return labels;
}

int runDryRunSteps(const std::vector<std::string>& labels) {
    const int total = static_cast<int>(labels.size());
    int infoStep = 0;

    printInfoHeader();

    for (int i = 0; i < total; ++i) {
        if (g_interrupted) {
            progressbar_finish("Dry run interrupted!");
            std::cout << "\n" << RED
                      << "Dry run failed or was interrupted.\n" << RESET;
            return 130;
        }

        const float startPct = (100.0f * static_cast<float>(i)) / static_cast<float>(total);
        const float endPct = (100.0f * static_cast<float>(i + 1)) / static_cast<float>(total);
        const std::string prefix = std::to_string(i + 1) + "/" + std::to_string(total) +
                                   " | " + labels[static_cast<size_t>(i)];

        beginCleanStep(startPct,
                       prefix,
                       infoStep,
                       labels[static_cast<size_t>(i)],
                       nullptr,
                       false);

        if (!dryRunStep()) {
            progressbar_finish("Dry run interrupted!");
            std::cout << "\n" << RED
                      << "Dry run failed or was interrupted.\n" << RESET;
            return 130;
        }

        progressbar_update(endPct, prefix + " - done");
    }

    std::cout << "\n";
    progressbar_finish("Dry run done!");
    return 0;
}

int handleDryRun() {
    const AppContext context = buildDryRunContext();

    std::cout << YELLOW << "[SYS] " << RESET
              << "Simulating " << nativeShortLabel(context) << " cleanup flow\n\n";

    return runDryRunSteps(dryRunLabels(context));
}

} // namespace

int main(int argc, char* argv[]) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    Options options;
    if (!parseOptions(argc, argv, options)) {
        std::cerr << YELLOW << "Use --help to show available options." << RESET << "\n";
        return 1;
    }

    if (options.showVersion && options.showHelp) {
        std::cout << YELLOW << "--version" << RESET << "\n";
        printVersion();
        std::cout << "\n" << YELLOW << "--help" << RESET << "\n";
        printHelp(argv[0]);
        return 0;
    }

    if (options.showVersion) {
        printVersion();
        return 0;
    }

    if (options.showHelp) {
        printHelp(argv[0]);
        return 0;
    }

    if (options.dryRun) {
        SigintGuard sigintGuard;
        return handleDryRun();
    }

    if (geteuid() != 0) {
        std::cout << RED << "Run with sudo!\n" << RESET;
        return 1;
    }

    zpm_update::checkForUpdates();

    FileLock lock;
    if (!lock.acquire()) {
        return 1;
    }

    SigintGuard sigintGuard;
    AppContext context = buildContext();

    if (context.packageManager == "unknown") {
        std::cout << RED << "Error: Could not detect a supported package manager "
                  << "(apt / zypper / dnf).\n" << RESET;
        return 1;
    }

    if (!writeLogHeader("-----zclean_check-----", LogMode::Truncate)) {
        std::cout << RED << "Error: Cannot create secure log file at "
                  << kLogPath << ".\n" << RESET;
        return 1;
    }

    CleanStatus status = checkCleanStatus(context);

    if (!status.any()) {
        if (status.checkError) {
            std::cout << RED << "Error: Could not reliably check cleanup status.\n" << RESET;
            std::cout << YELLOW << "[RAPORT] " << RESET << kLogPath << "\n";
            return 1;
        }

        std::cout << "\n" << GREEN << "System is already cleaned!" << RESET << "\n";
        return 0;
    }

    printCleanSummary(context, status);

    const std::vector<Step> steps = buildSteps(context, status);
    if (steps.empty()) {
        std::cout << RED << "Error: Cleanup plan is empty despite detected work.\n" << RESET;
        std::cout << YELLOW << "[RAPORT] " << RESET << kLogPath << "\n";
        return 1;
    }

    return runSteps(steps);
}
