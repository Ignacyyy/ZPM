#include "main.h"

#include <cerrno>
#include <csignal>
#include <cctype>
#include <cstring>
#include <limits>
#include <set>
#include <sys/file.h>
#include <sys/wait.h>

namespace {

constexpr const char* kLogFile = "/tmp/zupd.log";
constexpr const char* kPatchLogFile = "/tmp/zupd_patchcheck.log";
constexpr std::chrono::milliseconds kDryRunStepDelay{160};
constexpr std::chrono::milliseconds kLiveLogRefreshInterval{140};
constexpr int kLiveLogLines = 3;
constexpr int kLiveLogTopPaddingLines = 1;
constexpr int kLiveLogBottomPaddingLines = 1;
constexpr int kLiveLogRowsBelowBar =
    kLiveLogTopPaddingLines + kLiveLogLines + kLiveLogBottomPaddingLines;
constexpr int kLiveLogPrefixColumns = 7;
constexpr std::size_t kMaxPendingLogLine = 4096;

volatile std::sig_atomic_t g_interrupted = 0;

enum class LogMode {
    Truncate,
    Append
};

enum class NativeUpdateResult {
    Ok,
    Failed,
    RestartRequired
};

struct Options {
    bool reboot = false;
    bool shutdown = false;
    bool help = false;
    bool version = false;
    bool yes = false;
    bool fullUpdate = false;
    bool dryRun = false;
};

struct CommandResult {
    int exitCode = 127;
    std::string output;
};

struct ProcessConfig {
    bool captureStdout = false;
    bool mirrorCapturedStdoutToLog = false;
    bool logStdout = false;
    bool logStderr = false;
    bool discardStdin = true;
    bool discardStdout = true;
    bool discardStderr = true;
    LogMode logMode = LogMode::Append;
    const char* logPath = kLogFile;
    std::string header;
    std::vector<std::pair<std::string, std::string>> environment;
};

struct ReportAccess {
    bool valid = false;
    gid_t gid = 0;
};

struct UpdateStatus {
    bool native = false;
    bool hasFlatpak = false;
    bool hasSnap = false;
    bool dnf5 = false;
    bool checkError = false;
    bool zypperDup = false;

    std::vector<std::string> nativePackages;
    std::vector<std::string> zypperPatches;
    std::vector<std::string> flatpakPackages;
    std::vector<std::string> snapPackages;

    bool hasFlatpakUpdates() const {
        return hasFlatpak && !flatpakPackages.empty();
    }

    bool hasSnapUpdates() const {
        return hasSnap && !snapPackages.empty();
    }

    bool any() const {
        return native || hasFlatpakUpdates() || hasSnapUpdates();
    }
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

    int get() const {
        return fd_;
    }

    bool valid() const {
        return fd_ >= 0;
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
        const char* paths[] = {"/run/zupd.lock", "/tmp/zupd.lock"};
        bool busy = false;
        bool openFailed = false;

        for (const char* path : paths) {
            FileDescriptor fd(open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0644));
            if (!fd.valid()) {
                openFailed = true;
                continue;
            }

            if (!isSafeOwnedRegularFile(fd.get())) {
                openFailed = true;
                continue;
            }

            if (flock(fd.get(), LOCK_EX | LOCK_NB) == 0) {
                fd_ = std::move(fd);
                return true;
            }

            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                busy = true;
            } else {
                openFailed = true;
            }
        }

        if (busy) {
            std::cerr << RED << "Error: Another zupd instance is already running.\n" << RESET;
        } else if (openFailed) {
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
        sa.sa_flags = 0;
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

FileDescriptor openLogFile(const char* path, LogMode mode) {
    const int flags = O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW |
                      (mode == LogMode::Append ? O_APPEND : 0);

    FileDescriptor fd(open(path, flags, 0600));
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

void writeLogLine(const char* path, const std::string& line, LogMode mode = LogMode::Append) {
    FileDescriptor fd = openLogFile(path, mode);
    if (!fd.valid()) {
        return;
    }

    writeAll(fd.get(), line + "\n");
}

void writeLogHeader(const std::string& header, LogMode mode = LogMode::Append) {
    writeLogLine(kLogFile, header, mode);
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

    void moveCursorBelow() const {
        if (!started_) {
            return;
        }

        std::lock_guard<std::mutex> outputLock(zpm::progressbar_detail::outputMutex());
        std::cout << "\033[" << kLiveLogRowsBelowBar << "B\r" << std::flush;
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
        const std::vector<std::string> lines = displayLines();
        const int textColumns = std::max(0,
                                         zpm::progressbar_detail::terminalWidth() -
                                             kLiveLogPrefixColumns);

        std::lock_guard<std::mutex> outputLock(zpm::progressbar_detail::outputMutex());

        if (!rowsReserved_) {
            std::cout << "\033[s";
            for (int i = 0; i < kLiveLogRowsBelowBar; ++i) {
                std::cout << "\n\033[K";
            }
            std::cout << "\033[u";
            rowsReserved_ = true;
        }

        std::cout << "\033[s";

        for (int row = 1; row <= kLiveLogRowsBelowBar; ++row) {
            std::cout << "\033[u\033[" << row << "B\r\033[K";

            const int logIndex = row - kLiveLogTopPaddingLines - 1;
            if (logIndex >= 0 &&
                logIndex < kLiveLogLines &&
                logIndex < static_cast<int>(lines.size())) {
                std::cout << CYAN << "  log> " << RESET
                          << zpm::progressbar_detail::sanitizeTask(lines[static_cast<std::size_t>(logIndex)],
                                                                   textColumns);
            }
        }

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
                kill(-pid, SIGINT);
                kill(pid, SIGINT);
                signalSent = true;
            }
            continue;
        }

        return 127;
    }
}

void redirectToDevNull(int targetFd) {
    const int flags = (targetFd == STDIN_FILENO) ? O_RDONLY : O_WRONLY;
    const int nullFd = open("/dev/null", flags | O_CLOEXEC);
    if (nullFd >= 0) {
        dup2(nullFd, targetFd);
        close(nullFd);
    }
}

CommandResult runProcess(const std::vector<std::string>& args, const ProcessConfig& config = {}) {
    if (args.empty() || args.front().empty()) {
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
                          config.mirrorCapturedStdoutToLog ||
                          !config.header.empty();
    FileDescriptor log;

    if (needsLog) {
        log = openLogFile(config.logPath, config.logMode);
        if (!log.valid()) {
            return {};
        }
        if (!config.header.empty()) {
            writeAll(log.get(), config.header + "\n");
        }
    }

    const pid_t pid = fork();
    if (pid < 0) {
        return {};
    }

    if (pid == 0) {
        setpgid(0, 0);

        if (config.discardStdin) {
            redirectToDevNull(STDIN_FILENO);
        }

        if (config.captureStdout) {
            readEnd.reset();
            dup2(writeEnd.get(), STDOUT_FILENO);
        } else if (config.logStdout && log.valid()) {
            dup2(log.get(), STDOUT_FILENO);
        } else if (config.discardStdout) {
            redirectToDevNull(STDOUT_FILENO);
        }

        if (config.logStderr && log.valid()) {
            dup2(log.get(), STDERR_FILENO);
        } else if (config.discardStderr) {
            redirectToDevNull(STDERR_FILENO);
        }

        for (const auto& [key, value] : config.environment) {
            setenv(key.c_str(), value.c_str(), 1);
        }

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const std::string& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execvp(argv[0], argv.data());
        _exit(127);
    }

    setpgid(pid, pid);

    if (config.captureStdout) {
        writeEnd.reset();
    }

    CommandResult result;
    if (config.captureStdout) {
        std::array<char, 4096> buffer {};

        for (;;) {
            const ssize_t count = read(readEnd.get(), buffer.data(), buffer.size());
            if (count > 0) {
                result.output.append(buffer.data(), static_cast<size_t>(count));
                if (config.mirrorCapturedStdoutToLog && log.valid()) {
                    writeAll(log.get(), buffer.data(), static_cast<size_t>(count));
                }
                continue;
            }

            if (count == 0) {
                break;
            }

            if (errno == EINTR) {
                if (g_interrupted) {
                    kill(-pid, SIGINT);
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

int runCommandLogged(const std::vector<std::string>& args,
                     const std::string& header,
                     LogMode mode = LogMode::Append,
                     const char* logPath = kLogFile,
                     const std::vector<std::pair<std::string, std::string>>& environment = {}) {
    ProcessConfig config;
    config.logStdout = true;
    config.logStderr = true;
    config.header = header;
    config.logMode = mode;
    config.logPath = logPath;
    config.environment = environment;
    return runProcess(args, config).exitCode;
}

CommandResult captureCommand(const std::vector<std::string>& args,
                             const char* logPath = nullptr,
                             const std::string& header = {},
                             LogMode mode = LogMode::Append,
                             bool mirrorStdoutToLog = false,
                             bool stderrToLog = false,
                             const std::vector<std::pair<std::string, std::string>>& environment = {}) {
    ProcessConfig config;
    config.captureStdout = true;
    config.mirrorCapturedStdoutToLog = mirrorStdoutToLog;
    config.logStderr = stderrToLog;
    config.header = header;
    config.logMode = mode;
    config.environment = environment;
    if (logPath != nullptr) {
        config.logPath = logPath;
    }
    return runProcess(args, config);
}

int runCommandSplitLogs(const std::vector<std::string>& args,
                        const char* stdoutLogPath,
                        const std::string& stdoutHeader,
                        LogMode stdoutMode,
                        const char* stderrLogPath,
                        const std::vector<std::pair<std::string, std::string>>& environment = {}) {
    if (args.empty() || args.front().empty()) {
        return 127;
    }

    FileDescriptor stdoutLog = openLogFile(stdoutLogPath, stdoutMode);
    FileDescriptor stderrLog = openLogFile(stderrLogPath, LogMode::Append);

    if (!stdoutLog.valid() || !stderrLog.valid()) {
        return 127;
    }

    if (!stdoutHeader.empty()) {
        writeAll(stdoutLog.get(), stdoutHeader + "\n");
        writeAll(stderrLog.get(), stdoutHeader + "\n");
    }

    const pid_t pid = fork();
    if (pid < 0) {
        return 127;
    }

    if (pid == 0) {
        setpgid(0, 0);
        redirectToDevNull(STDIN_FILENO);
        dup2(stdoutLog.get(), STDOUT_FILENO);
        dup2(stderrLog.get(), STDERR_FILENO);

        for (const auto& [key, value] : environment) {
            setenv(key.c_str(), value.c_str(), 1);
        }

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const std::string& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execvp(argv[0], argv.data());
        _exit(127);
    }

    setpgid(pid, pid);
    return waitForChild(pid);
}

bool runCommandOk(const std::vector<std::string>& args,
                  const std::string& header,
                  LogMode mode = LogMode::Append,
                  const std::vector<std::pair<std::string, std::string>>& environment = {}) {
    return runCommandLogged(args, header, mode, kLogFile, environment) == 0;
}

std::vector<std::pair<std::string, std::string>> cLocaleEnv() {
    return {{"LC_ALL", "C"}};
}

std::vector<std::pair<std::string, std::string>> aptEnv() {
    return {
        {"LC_ALL", "C"},
        {"DEBIAN_FRONTEND", "noninteractive"},
        {"DEBCONF_NONINTERACTIVE_SEEN", "true"},
        {"APT_LISTCHANGES_FRONTEND", "none"},
        {"NEEDRESTART_MODE", "a"},
        {"UCF_FORCE_CONFOLD", "1"}
    };
}

std::vector<std::string> aptGet(std::initializer_list<std::string> args) {
    std::vector<std::string> command = {
        "apt-get",
        "-o", "Dpkg::Lock::Timeout=120",
        "-o", "APT::Get::Assume-Yes=true",
        "-o", "Dpkg::Use-Pty=0"
    };
    command.insert(command.end(), args.begin(), args.end());
    return command;
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

std::map<std::string, std::string> readOsRelease() {
    std::map<std::string, std::string> values;
    std::ifstream file("/etc/os-release");
    std::string line;

    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }

        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }

        values[key] = value;
    }

    return values;
}

bool isTumbleweedLike() {
    const auto os = readOsRelease();
    std::string combined;

    for (const char* key : {"ID", "ID_LIKE", "NAME", "PRETTY_NAME", "VERSION"}) {
        const auto it = os.find(key);
        if (it != os.end()) {
            combined += " " + it->second;
        }
    }

    combined = toLower(combined);
    return combined.find("tumbleweed") != std::string::npos ||
           combined.find("slowroll") != std::string::npos;
}

void printCommandLines(const std::vector<std::string>& command,
                       const std::string& prefix,
                       const std::vector<std::pair<std::string, std::string>>& environment = {}) {
    const CommandResult result = captureCommand(command, nullptr, {}, LogMode::Append, false, false, environment);
    for (const std::string& rawLine : splitLines(result.output)) {
        const std::string line = trim(rawLine);
        if (!line.empty()) {
            std::cout << prefix << line << "\n";
        }
    }
}

void collectAptRepositoriesFromFile(const std::filesystem::path& path,
                                    std::set<std::string>& repositories) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }

        if (startsWith(line, "deb ")) {
            repositories.insert(line);
        } else if (startsWith(line, "URIs:")) {
            const std::string uri = trim(line.substr(5));
            if (!uri.empty()) {
                repositories.insert("deb " + uri);
            }
        }
    }
}

void printAptRepositories(const std::string& prefix) {
    std::set<std::string> repositories;
    collectAptRepositoriesFromFile("/etc/apt/sources.list", repositories);

    std::error_code ec;
    if (std::filesystem::is_directory("/etc/apt/sources.list.d", ec)) {
        for (const auto& entry : std::filesystem::directory_iterator("/etc/apt/sources.list.d", ec)) {
            if (ec) {
                break;
            }
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            if (entry.path().extension() == ".list" || entry.path().extension() == ".sources") {
                collectAptRepositoriesFromFile(entry.path(), repositories);
            }
        }
    }

    for (const std::string& repository : repositories) {
        std::cout << prefix << repository << "\n";
    }
}

void printZypperRepositories(const std::string& prefix) {
    const CommandResult result = captureCommand({"zypper", "lr"});
    for (const std::string& rawLine : splitLines(result.output)) {
        const std::string line = trim(rawLine);
        if (line.empty() || !std::isdigit(static_cast<unsigned char>(line.front()))) {
            continue;
        }

        const std::vector<std::string> columns = split(line, '|');
        if (columns.size() >= 2) {
            const std::string name = trim(columns[1]);
            if (!name.empty()) {
                std::cout << prefix << name << "\n";
            }
        }
    }
}

void printDnfRepositories(const std::string& prefix) {
    const std::vector<std::string> command = commandExists("dnf5")
        ? std::vector<std::string>{"dnf5", "repolist", "-q"}
        : std::vector<std::string>{"dnf", "repolist", "-q"};
    const CommandResult result = captureCommand(command);
    bool first = true;

    for (const std::string& rawLine : splitLines(result.output)) {
        const std::string line = trim(rawLine);
        if (line.empty()) {
            continue;
        }
        if (first) {
            first = false;
            const std::string lower = toLower(line);
            if (lower.find("repo") != std::string::npos) {
                continue;
            }
        }

        std::istringstream stream(line);
        std::string repo;
        if (stream >> repo) {
            std::cout << prefix << repo << "\n";
        }
    }
}

std::vector<std::string> nonEmptyLines(const std::string& text) {
    std::vector<std::string> lines;
    for (const std::string& line : splitLines(text)) {
        addUnique(lines, line);
    }
    return lines;
}

std::vector<std::string> parseAptSimulation(const std::string& output) {
    std::vector<std::string> packages;

    for (const std::string& line : splitLines(output)) {
        if (!startsWith(line, "Inst ")) {
            continue;
        }

        std::istringstream ss(line);
        std::string marker;
        std::string package;
        if (ss >> marker >> package) {
            addUnique(packages, package);
        }
    }

    return packages;
}

std::vector<std::string> parseZypperListUpdates(const std::string& output) {
    std::vector<std::string> packages;

    for (const std::string& line : splitLines(output)) {
        const auto columns = split(line, '|');
        if (columns.size() < 3) {
            continue;
        }

        if (trim(columns[0]) == "v") {
            addUnique(packages, columns[2]);
        }
    }

    return packages;
}

std::vector<std::string> parseZypperPatches(const std::string& output) {
    static const std::set<std::string> wanted = {
        "needed", "security", "recommended", "optional"
    };

    std::vector<std::string> patches;

    for (const std::string& line : splitLines(output)) {
        const auto columns = split(line, '|');
        if (columns.size() < 3) {
            continue;
        }

        if (wanted.count(toLower(trim(columns[0]))) != 0) {
            addUnique(patches, columns[2]);
        }
    }

    return patches;
}

std::vector<std::string> parseDnfUpdates(const std::string& output) {
    std::vector<std::string> packages;

    for (const std::string& rawLine : splitLines(output)) {
        const std::string line = trim(rawLine);
        if (line.empty() ||
            startsWith(line, "Last metadata") ||
            startsWith(line, "Available") ||
            startsWith(line, "Package ")) {
            continue;
        }

        std::istringstream ss(line);
        std::string packageWithArch;
        std::string version;
        std::string repo;
        if (!(ss >> packageWithArch >> version >> repo)) {
            continue;
        }

        const auto dot = packageWithArch.rfind('.');
        if (dot == std::string::npos) {
            continue;
        }

        addUnique(packages, packageWithArch.substr(0, dot));
    }

    return packages;
}

std::vector<std::string> parseSnapRefreshList(const std::string& output) {
    std::vector<std::string> snaps;

    for (const std::string& rawLine : splitLines(output)) {
        const std::string line = trim(rawLine);
        const std::string lower = toLower(line);

        if (line.empty() ||
            startsWith(lower, "name ") ||
            startsWith(lower, "all snaps")) {
            continue;
        }

        std::istringstream ss(line);
        std::string name;
        if (ss >> name) {
            addUnique(snaps, name);
        }
    }

    return snaps;
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

std::vector<std::pair<std::string, std::string>> parseDisabledSnapRevisions(const std::string& output) {
    std::vector<std::pair<std::string, std::string>> revisions;

    for (const std::string& rawLine : splitLines(output)) {
        const std::string line = trim(rawLine);
        const std::string lower = toLower(line);

        if (line.empty() || startsWith(lower, "name ") || lower.find("disabled") == std::string::npos) {
            continue;
        }

        std::istringstream ss(line);
        std::string name;
        std::string version;
        std::string revision;

        if ((ss >> name >> version >> revision) &&
            isSafeToken(name) &&
            isDigits(revision)) {
            revisions.emplace_back(name, revision);
        }
    }

    return revisions;
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

    const std::filesystem::directory_iterator entries(directory, ec);
    if (ec) {
        writeLogLine(kLogFile, "cleanup: cannot iterate " + directory.string() + ": " + ec.message());
        return false;
    }

    bool ok = true;
    for (const auto& entry : entries) {
        if (!isRootOwnedRemovableEntry(entry.path())) {
            writeLogLine(kLogFile, "cleanup: skipped unsafe entry " + entry.path().string());
            ok = false;
            continue;
        }

        std::error_code removeError;
        std::filesystem::remove_all(entry.path(), removeError);
        if (removeError) {
            writeLogLine(kLogFile, "cleanup: cannot remove " + entry.path().string() + ": " + removeError.message());
            ok = false;
        }
    }

    return ok;
}

bool removeEntriesWithPrefix(const std::filesystem::path& directory, const std::string& prefix) {
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec)) {
        return true;
    }

    const std::filesystem::directory_iterator entries(directory, ec);
    if (ec) {
        writeLogLine(kLogFile, "cleanup: cannot iterate " + directory.string() + ": " + ec.message());
        return false;
    }

    bool ok = true;
    for (const auto& entry : entries) {
        const std::string filename = entry.path().filename().string();
        if (!startsWith(filename, prefix)) {
            continue;
        }

        if (!isRootOwnedRemovableEntry(entry.path())) {
            writeLogLine(kLogFile, "cleanup: skipped unsafe entry " + entry.path().string());
            ok = false;
            continue;
        }

        std::error_code removeError;
        std::filesystem::remove_all(entry.path(), removeError);
        if (removeError) {
            writeLogLine(kLogFile, "cleanup: cannot remove " + entry.path().string() + ": " + removeError.message());
            ok = false;
        }
    }

    return ok;
}

int countSteps(const UpdateStatus& status) {
    int steps = 1; // cleanup

    if (status.native) {
        steps += 2; // consistency check + native update
    }
    if (status.hasFlatpakUpdates()) {
        ++steps;
    }
    if (status.hasSnapUpdates()) {
        ++steps;
    }

    return steps;
}

std::string nativeUpdateTask(UiState state) {
    switch (state) {
        case UiState::APT:
            return "updating APT packages";
        case UiState::ZYPPER:
            return "updating Zypper packages";
        case UiState::DNF:
            return "updating DNF packages";
        default:
            return "updating native packages";
    }
}

void beginProgressStep(UiState state,
                       int& step,
                       const std::string& text) {
    std::lock_guard<std::mutex> outputLock(zpm::progressbar_detail::outputMutex());

    if (step > 0) {
        std::cout << "\r\033[K\033[1A\r\033[K";
    } else {
        std::cout << "\r\033[K";
    }

    std::cout << CYAN << "[>]" << RESET << " " << text << "\n\n";

    progressbar_set_state(state, ++step);
    std::cout << std::flush;
}

void printInfoHeader() {
    std::cout << "\n" << CYAN << "[ZPM-INFO]" << RESET << "\n";
}

UiState nativeStateForPackageManager(const std::string& packageManager) {
    if (packageManager == "zypper") {
        return UiState::ZYPPER;
    }
    if (packageManager == "dnf") {
        return UiState::DNF;
    }
    return UiState::APT;
}

std::string nativeTitleForPackageManager(const std::string& packageManager, bool dnf5 = false) {
    if (packageManager == "zypper") {
        return "Zypper";
    }
    if (packageManager == "dnf") {
        return dnf5 ? "DNF5" : "DNF";
    }
    return "APT";
}

std::string dryRunPackageManager() {
    const std::string packageManager = get_package_manager();
    return packageManager == "unknown" ? "apt" : packageManager;
}

std::vector<std::string> dryRunNativePackages(const std::string& packageManager,
                                              const Options& options) {
    if (packageManager == "zypper") {
        return options.fullUpdate
            ? std::vector<std::string>{"fake-distribution-release", "fake-kernel-default"}
            : std::vector<std::string>{"fake-zypper-lib", "fake-desktop-patch"};
    }
    if (packageManager == "dnf") {
        return options.fullUpdate
            ? std::vector<std::string>{"fake-system-release", "fake-kernel-core"}
            : std::vector<std::string>{"fake-dnf-lib", "fake-security-update"};
    }

    return options.fullUpdate
        ? std::vector<std::string>{"fake-linux-image", "fake-linux-image-meta"}
        : std::vector<std::string>{"fake-openssl", "fake-desktop-library"};
}

UpdateStatus buildDryRunStatus(const std::string& packageManager, const Options& options) {
    UpdateStatus status;
    status.native = true;
    status.dnf5 = packageManager == "dnf" && commandExists("dnf5");
    status.zypperDup = packageManager == "zypper" && (options.fullUpdate || isTumbleweedLike());
    status.nativePackages = dryRunNativePackages(packageManager, options);

    status.hasFlatpak = commandExists("flatpak");
    if (status.hasFlatpak) {
        status.flatpakPackages = {"fake.flatpak.App"};
    }

    status.hasSnap = commandExists("snap");
    if (status.hasSnap) {
        status.snapPackages = {"fake-snap-app"};
    }

    return status;
}

bool dryRunStep() {
    std::this_thread::sleep_for(kDryRunStepDelay);
    return !g_interrupted;
}

NativeUpdateResult dryRunNativeUpdate() {
    return dryRunStep() ? NativeUpdateResult::Ok : NativeUpdateResult::Failed;
}

bool checkInterrupted(bool& ok) {
    if (!g_interrupted) {
        return false;
    }

    ok = false;
    return true;
}

bool askConfirm(const Options& options) {
    if (options.yes) {
        return true;
    }

    std::cout << "\n" << YELLOW << "Proceed with update?" << RESET << " [y/n]: ";

    std::string answer;
    if (!std::getline(std::cin >> std::ws, answer)) {
        return false;
    }

    answer = toLower(trim(answer));
    return answer == "y" || answer == "yes";
}

void printPackageList(const std::string& title,
                      const std::vector<std::string>& packages,
                      bool showWhenEmpty = false) {
    if (packages.empty() && !showWhenEmpty) {
        return;
    }

    const std::string prefix = std::string(YELLOW) + "[+] " + RESET;
    std::cout << RED << "\nPackages to update (" << title << "):\n" << RESET;

    if (packages.empty()) {
        std::cout << YELLOW << "(no packages listed)" << RESET << "\n";
        return;
    }

    for (const std::string& package : packages) {
        std::cout << prefix << package << "\n";
    }
}

void printUniversalUpdateLists(const UpdateStatus& status) {
    printPackageList("Flatpak", status.flatpakPackages);
    printPackageList("Snap", status.snapPackages);
}

void checkUniversalManagers(UpdateStatus& status) {
    status.hasFlatpak = commandExists("flatpak") ||
                        commandExists("/usr/bin/flatpak") ||
                        commandExists("/usr/local/bin/flatpak");
    status.hasSnap = commandExists("snap") || commandExists("/usr/bin/snap");

    if (status.hasFlatpak) {
        const CommandResult result = captureCommand({"flatpak", "remote-ls", "--updates", "--columns=name"});
        status.flatpakPackages = nonEmptyLines(result.output);
    }

    if (status.hasSnap) {
        const CommandResult result = captureCommand({"snap", "refresh", "--list"});
        status.snapPackages = parseSnapRefreshList(result.output);
    }
}

void logOptionalCleanupFailure(const std::string& action) {
    writeLogLine(kLogFile, "optional cleanup: " + action + " failed - skipped");
}

bool cleanupUniversal(const UpdateStatus& status) {
    if (status.hasFlatpak) {
        if (!runCommandOk({"flatpak", "uninstall", "--unused", "-y"}, "----cleaning_flatpak----")) {
            if (g_interrupted) {
                return false;
            }
            logOptionalCleanupFailure("flatpak uninstall --unused");
        }

        writeLogHeader("----cleaning_flatpak_cache----");
        if (!removeEntriesWithPrefix("/var/tmp", "flatpak-cache-")) {
            logOptionalCleanupFailure("flatpak cache cleanup");
        }
    }

    if (status.hasSnap) {
        const CommandResult list = captureCommand({"snap", "list", "--all"},
                                                  kLogFile,
                                                  "----checking_disabled_snap_revisions----",
                                                  LogMode::Append,
                                                  true,
                                                  true);

        if (list.exitCode != 0) {
            if (g_interrupted) {
                return false;
            }
            logOptionalCleanupFailure("snap list --all");
        } else {
            for (const auto& [name, revision] : parseDisabledSnapRevisions(list.output)) {
                if (!runCommandOk({"snap", "remove", name, "--revision=" + revision},
                                  "----removing_snap_revision_" + name + "_" + revision + "----")) {
                    if (g_interrupted) {
                        return false;
                    }
                    logOptionalCleanupFailure("snap remove " + name + " revision " + revision);
                }
            }
        }

        writeLogHeader("----cleaning_snap_cache----");
        if (!removeDirectoryEntries("/var/lib/snapd/cache")) {
            logOptionalCleanupFailure("snap cache cleanup");
        }
    }

    return !g_interrupted;
}

bool finishAndReport(const Options& options,
                     bool ok,
                     int total,
                     int step,
                     LiveLogView* liveLog = nullptr) {
    const bool interrupted = g_interrupted;
    if (g_interrupted) {
        ok = false;
    }

    if (liveLog != nullptr) {
        liveLog->stop();
    }

    if (ok) {
        progressbar_set_state(UiState::DONE, total);
        progressbar_finish(options.dryRun ? "Dry run done!" : "DONE!");
    } else {
        progressbar_set_state(UiState::ERROR, step);
        progressbar_finish("ERROR!");
    }

    if (liveLog != nullptr) {
        liveLog->moveCursorBelow();
    }

    if (interrupted) {
        std::cout << YELLOW << "Cancelled by user (Ctrl+C).\n" << RESET;
    }

    if (!ok) {
        if (options.dryRun) {
            std::cout << RED << "Dry run failed or was interrupted.\n" << RESET;
        } else {
            std::cout << RED << "ERROR," << RESET
                      << " check " << kLogFile << " for details.\n";
        }
        return false;
    }

    if (options.dryRun) {
        return true;
    }

    std::cout << YELLOW << "[RAPORT]" << RESET << " " << kLogFile << "\n";

    if (options.reboot) {
        std::cout << YELLOW << "[*] Rebooting in 3s..." << RESET << "\n";
        std::this_thread::sleep_for(std::chrono::seconds(3));
        runCommandLogged({"reboot"}, "-----reboot-----");
    } else if (options.shutdown) {
        std::cout << YELLOW << "[*] Shutting down in 3s..." << RESET << "\n";
        std::this_thread::sleep_for(std::chrono::seconds(3));
        runCommandLogged({"shutdown", "-h", "now"}, "-----shutdown-----");
    }

    return true;
}

void printHelp(const char* progName) {
    std::cout << RED << "Usage: " << RESET << progName << " [options]"
              << " or zpm upd/update [options]\n";
    std::cout << RED << "Options:" << RESET << "\n"
              << "  --full, -f       Perform a full system upgrade\n"
              << "  --reboot, -r     Reboot the system after update\n"
              << "  --shutdown, -s   Shutdown the system after update\n"
              << "  --yes, -y        Automatic system update\n"
              << "  --dry-run        Simulate update flow; no packages are changed\n"
              << "  --help, -h       Show this help message\n"
              << "  --version, -v    Show version information\n";
}

void printVersion() {
    std::cout << RED << "zupd component version: v" << zpm_version::version()
              << " of ZPM\n" << RESET
              << "https://github.com/Zielina-Konrad-productions/ZPM\n"
              << "Copyright (c) 2026 Ignacyyy & Ry3ball\n"
              << "License: MIT\n";
}

bool parseOptions(int argc, char* argv[], Options& options) {
    std::vector<std::string> errors;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--full" || arg == "-f") {
            options.fullUpdate = true;
        } else if (arg == "--reboot" || arg == "-r") {
            options.reboot = true;
        } else if (arg == "--shutdown" || arg == "-s") {
            options.shutdown = true;
        } else if (arg == "--help" || arg == "-h") {
            options.help = true;
        } else if (arg == "--version" || arg == "-v") {
            options.version = true;
        } else if (arg == "--yes" || arg == "-y") {
            options.yes = true;
        } else if (arg == "--dry-run") {
            options.dryRun = true;
        } else {
            errors.push_back("Unknown option: " + arg);
        }
    }

    if (options.reboot && options.shutdown) {
        errors.push_back("-r/--reboot and -s/--shutdown are mutually exclusive.");
    }

    if (options.dryRun && (options.reboot || options.shutdown)) {
        errors.push_back("--dry-run cannot be combined with --reboot or --shutdown.");
    }

    if ((options.help || options.version) &&
        (options.reboot || options.shutdown || options.yes || options.fullUpdate || options.dryRun)) {
        errors.push_back("--help and --version can only be combined with each other.");
    }

    for (const std::string& error : errors) {
        std::cerr << RED << "Error: " << error << RESET << "\n";
    }

    return errors.empty();
}

void printRepositorySummary(const std::string& packageManager) {
    const auto os = readOsRelease();
    const auto pretty = os.find("PRETTY_NAME");

    std::cout << YELLOW << "[SYS] " << RESET;
    if (pretty != os.end() && !pretty->second.empty()) {
        std::cout << pretty->second;
    } else {
        std::cout << "Unknown Linux distribution";
    }
    std::cout << "\n";

    const std::string prefix = std::string(YELLOW) + "- " + RESET;

    if (packageManager == "apt") {
        std::cout << "\n" << YELLOW << "[D]" << RESET
                  << GREEN << " APT Repositories:\n" << RESET;

        printAptRepositories(prefix);
    } else if (packageManager == "zypper") {
        std::cout << "\n" << YELLOW << "[Z]" << RESET
                  << GREEN << " Zypper Repositories:\n" << RESET;

        printZypperRepositories(prefix);
    } else if (packageManager == "dnf") {
        std::cout << "\n" << YELLOW << "[R]" << RESET
                  << GREEN << " DNF Repositories:\n" << RESET;

        printDnfRepositories(prefix);
    }

    if (commandExists("flatpak") ||
        commandExists("/usr/bin/flatpak") ||
        commandExists("/usr/local/bin/flatpak")) {
        std::cout << "\n" << YELLOW << "[F]" << RESET
                  << GREEN << " Flatpak Remotes:\n" << RESET;

        printCommandLines({"flatpak", "remotes", "--columns=name"}, prefix);
    }

    if (commandExists("snap") || commandExists("/usr/bin/snap")) {
        std::cout << "\n" << YELLOW << "[S]" << RESET
                  << GREEN << " Snap is available.\n" << RESET;
    }
}

UpdateStatus aptCheckUpdates(const Options& options) {
    UpdateStatus status;

    std::cout << "\n" << YELLOW << "[*] Refreshing package cache..." << RESET << "\n";

    if (!runCommandOk(aptGet({"update", "-qq"}),
                      "-----apt_update-----",
                      LogMode::Truncate,
                      aptEnv())) {
        status.checkError = true;
        checkUniversalManagers(status);
        return status;
    }

    const std::vector<std::string> simulateCommand = options.fullUpdate
        ? aptGet({"dist-upgrade", "-s"})
        : aptGet({"upgrade", "-s"});
    const CommandResult simulation = captureCommand(
        simulateCommand,
        kLogFile,
        options.fullUpdate ? "-----apt_simulate_dist_upgrade-----" : "-----apt_simulate_upgrade-----",
        LogMode::Append,
        true,
        true,
        aptEnv()
    );

    if (simulation.exitCode != 0) {
        status.checkError = true;
        checkUniversalManagers(status);
        return status;
    }

    status.nativePackages = parseAptSimulation(simulation.output);
    status.native = !status.nativePackages.empty();

    checkUniversalManagers(status);
    return status;
}

UpdateStatus zypperCheckUpdates(const Options& options) {
    UpdateStatus status;

    std::cout << "\n" << YELLOW << "[*] Refreshing package cache..." << RESET << "\n";

    status.zypperDup = options.fullUpdate || isTumbleweedLike();

    if (!runCommandOk({"zypper", "--non-interactive", "refresh"},
                      "-----zypper_refresh-----",
                      LogMode::Truncate,
                      cLocaleEnv())) {
        status.checkError = true;
        checkUniversalManagers(status);
        return status;
    }

    std::vector<std::string> updateCommand = {"zypper", "--no-refresh", "list-updates"};
    if (status.zypperDup) {
        updateCommand.push_back("--all");
    }
    updateCommand.push_back("-t");
    updateCommand.push_back("package");

    const CommandResult updates = captureCommand(
        updateCommand,
        kLogFile,
        "-----zypper_list_updates-----",
        LogMode::Append,
        true,
        true,
        cLocaleEnv()
    );

    if (updates.exitCode != 0) {
        status.checkError = true;
        checkUniversalManagers(status);
        return status;
    }

    status.nativePackages = parseZypperListUpdates(updates.output);

    const int patchExit = runCommandSplitLogs(
        {"zypper", "--no-refresh", "patch-check"},
        kPatchLogFile,
        "-----zypper_patch_check-----",
        LogMode::Truncate,
        kLogFile,
        cLocaleEnv()
    );

    const bool hasPatches = (patchExit == 100 || patchExit == 101);
    if (patchExit != 0 && !hasPatches) {
        status.checkError = true;
    }

    if (!status.zypperDup) {
        const CommandResult patches = captureCommand(
            {"zypper", "--no-refresh", "list-patches"},
            kLogFile,
            "-----zypper_list_patches-----",
            LogMode::Append,
            true,
            true,
            cLocaleEnv()
        );

        if (patches.exitCode == 0) {
            status.zypperPatches = parseZypperPatches(patches.output);
        }
    }

    status.native = !status.nativePackages.empty() || hasPatches;

    checkUniversalManagers(status);
    return status;
}

UpdateStatus dnfCheckUpdates() {
    UpdateStatus status;

    std::cout << "\n" << YELLOW << "[*] Refreshing package cache..." << RESET << "\n";

    status.dnf5 = commandExists("dnf5") || commandExists("/usr/bin/dnf5");

    const std::vector<std::string> command = status.dnf5
        ? std::vector<std::string>{"dnf5", "check-upgrade", "-q"}
        : std::vector<std::string>{"dnf", "check-update", "-q", "--refresh"};

    const CommandResult result = captureCommand(
        command,
        kLogFile,
        status.dnf5 ? "-----dnf5_check_upgrade-----" : "-----dnf_check_update-----",
        LogMode::Truncate,
        true,
        true,
        cLocaleEnv()
    );

    if (result.exitCode == 100) {
        status.native = true;
        status.nativePackages = parseDnfUpdates(result.output);
    } else if (result.exitCode != 0) {
        status.checkError = true;
    }

    checkUniversalManagers(status);
    return status;
}

bool runUpdateFlow(const Options& options,
                   const UpdateStatus& status,
                   UiState nativeState,
                   const std::function<bool()>& checkConsistency,
                   const std::function<NativeUpdateResult()>& updateNative,
                   const std::function<bool()>& cleanupNative) {
    const int total = countSteps(status);
    int step = 0;
    bool ok = true;

    printInfoHeader();
    progressbar_start(total);
    LiveLogView liveLog(kLogFile, !options.dryRun);
    liveLog.start();

    if (status.native) {
        beginProgressStep(UiState::CHECKING, step, "checking system consistency");
        if (!checkConsistency()) {
            ok = false;
        }
        if (checkInterrupted(ok)) {
            return finishAndReport(options, ok, total, step, &liveLog);
        }
    }

    if (status.native && ok) {
        beginProgressStep(nativeState, step, nativeUpdateTask(nativeState));
        const NativeUpdateResult result = updateNative();

        if (result == NativeUpdateResult::RestartRequired) {
            liveLog.stop();
            progressbar_finish("RESTART NEEDED");
            liveLog.moveCursorBelow();

            std::cout << "\n" << YELLOW
                      << "[*] Zypper is adjusting its stack manager and has aborted the download.\n"
                      << "[*] The remaining system packages are NOT updated.\n"
                      << "[*] Restart command to update system.\n"
                      << RESET << "\n";

            cleanupNative();
            cleanupUniversal(status);
            return false;
        }

        if (result == NativeUpdateResult::Failed) {
            ok = false;
        }
    }
    if (checkInterrupted(ok)) {
        return finishAndReport(options, ok, total, step, &liveLog);
    }

    if (status.hasFlatpakUpdates() && ok) {
        beginProgressStep(UiState::FLATPAK, step, "updating Flatpak packages");
        if (options.dryRun) {
            ok = dryRunStep() && ok;
        } else if (!runCommandOk({"flatpak", "update", "-y"}, "----updating_flatpak----")) {
            ok = false;
        }
    }
    if (checkInterrupted(ok)) {
        return finishAndReport(options, ok, total, step, &liveLog);
    }

    if (status.hasSnapUpdates() && ok) {
        beginProgressStep(UiState::SNAP, step, "updating Snap packages");
        if (options.dryRun) {
            ok = dryRunStep() && ok;
        } else if (!runCommandOk({"snap", "refresh"}, "----updating_snap----")) {
            ok = false;
        }
    }
    if (checkInterrupted(ok)) {
        return finishAndReport(options, ok, total, step, &liveLog);
    }

    beginProgressStep(UiState::CLEANUP, step, "cleaning");
    if (!cleanupNative()) {
        ok = false;
    }
    if (!options.dryRun && !cleanupUniversal(status)) {
        ok = false;
    }

    checkInterrupted(ok);
    return finishAndReport(options, ok, total, step, &liveLog);
}

bool aptUpdate(const Options& options, const UpdateStatus& status) {
    printPackageList("APT", status.nativePackages, status.native);
    printUniversalUpdateLists(status);

    if (!askConfirm(options)) {
        std::cout << YELLOW << "[*] Update cancelled by user." << RESET << "\n";
        return true;
    }

    return runUpdateFlow(
        options,
        status,
        UiState::APT,
        [] {
            return runCommandOk({"dpkg", "--configure", "-a", "--force-confdef", "--force-confold"},
                                "-----checking_system_consistency-----",
                                LogMode::Append,
                                aptEnv());
        },
        [&options] {
            std::vector<std::string> command = aptGet({
                options.fullUpdate ? "dist-upgrade" : "upgrade",
                "-y",
                "-o", "Dpkg::Options::=--force-confdef",
                "-o", "Dpkg::Options::=--force-confold"
            });
            return runCommandOk(command,
                                options.fullUpdate ? "-----updating_APT_dist_upgrade-----" : "-----updating_APT_upgrade-----",
                                LogMode::Append,
                                aptEnv())
                ? NativeUpdateResult::Ok
                : NativeUpdateResult::Failed;
        },
        [] {
            const bool autoremove = runCommandOk(aptGet({"autoremove", "-y"}),
                                                 "----cleaning_APT_autoremove----",
                                                 LogMode::Append,
                                                 aptEnv());
            const bool autoclean = runCommandOk(aptGet({"autoclean"}),
                                                "----cleaning_APT_autoclean----",
                                                LogMode::Append,
                                                aptEnv());
            return autoremove && autoclean;
        }
    );
}

bool zypperUpdate(const Options& options, const UpdateStatus& status) {
    printPackageList("Zypper", status.nativePackages, status.native);
    if (!status.zypperDup) {
        printPackageList("Zypper patches", status.zypperPatches);
    }
    printUniversalUpdateLists(status);

    if (!askConfirm(options)) {
        std::cout << YELLOW << "[*] Update cancelled by user." << RESET << "\n";
        return true;
    }

    return runUpdateFlow(
        options,
        status,
        UiState::ZYPPER,
        [] {
            return runCommandOk({"rpm", "--rebuilddb"}, "-----checking_system_consistency-----");
        },
        [&status] {
            const std::vector<std::string> command = status.zypperDup
                ? std::vector<std::string>{"zypper", "--non-interactive", "dup", "-y", "--auto-agree-with-licenses"}
                : std::vector<std::string>{"zypper", "--non-interactive", "patch", "--with-update", "-y", "--auto-agree-with-licenses"};

            const int exitCode = runCommandLogged(
                command,
                status.zypperDup
                    ? "-----updating_zypper_DUP-----"
                    : "-----updating_zypper_PATCH_WITH_UPDATE-----"
            );

            if (exitCode == 103 || exitCode == 8) {
                return NativeUpdateResult::RestartRequired;
            }

            return exitCode == 0 ? NativeUpdateResult::Ok : NativeUpdateResult::Failed;
        },
        [] {
            return runCommandOk({"zypper", "clean", "-a"}, "----cleaning_zypper----");
        }
    );
}

bool dnfUpdate(const Options& options, const UpdateStatus& status) {
    printPackageList(status.dnf5 ? "DNF5" : "DNF", status.nativePackages, status.native);
    printUniversalUpdateLists(status);

    if (!askConfirm(options)) {
        std::cout << YELLOW << "[*] Update cancelled by user." << RESET << "\n";
        return true;
    }

    return runUpdateFlow(
        options,
        status,
        UiState::DNF,
        [] {
            return runCommandOk({"rpm", "--rebuilddb"}, "-----checking_system_consistency-----");
        },
        [&options, &status] {
            std::vector<std::string> command;
            std::string header;

            if (status.dnf5) {
                command = options.fullUpdate
                    ? std::vector<std::string>{"dnf5", "distro-sync", "-y"}
                    : std::vector<std::string>{"dnf5", "upgrade", "-y"};
                header = options.fullUpdate ? "-----updating_DNF5_distro-sync-----" : "-----updating_DNF5-----";
            } else {
                command = options.fullUpdate
                    ? std::vector<std::string>{"dnf", "distro-sync", "-y"}
                    : std::vector<std::string>{"dnf", "upgrade", "-y"};
                header = options.fullUpdate ? "-----updating_DNF_distro-sync-----" : "-----updating_DNF-----";
            }

            return runCommandOk(command, header) ? NativeUpdateResult::Ok : NativeUpdateResult::Failed;
        },
        [&status] {
            if (status.dnf5) {
                const bool autoremove = runCommandOk({"dnf5", "autoremove", "-y"}, "----cleaning_DNF5_autoremove----");
                const bool clean = runCommandOk({"dnf5", "clean", "packages"}, "----cleaning_DNF5_packages----");
                return autoremove && clean;
            }

            const bool autoremove = runCommandOk({"dnf", "autoremove", "-y"}, "----cleaning_DNF_autoremove----");
            const bool clean = runCommandOk({"dnf", "clean", "packages"}, "----cleaning_DNF_packages----");
            return autoremove && clean;
        }
    );
}

int handleDryRun(const Options& options) {
    const std::string packageManager = dryRunPackageManager();
    const UiState nativeState = nativeStateForPackageManager(packageManager);
    const UpdateStatus status = buildDryRunStatus(packageManager, options);

    std::cout << YELLOW << "[SYS] " << RESET
              << "Simulating " << nativeTitleForPackageManager(packageManager, status.dnf5)
              << " update flow\n";

    if (options.fullUpdate || status.zypperDup) {
        if (status.zypperDup) {
            std::cout << YELLOW << "[!] FULL UPDATE MODE (dup)" << RESET << "\n";
        } else if (packageManager == "dnf") {
            std::cout << YELLOW << "[!] FULL UPDATE MODE (distro-sync)" << RESET << "\n";
        } else {
            std::cout << YELLOW << "[!] FULL UPDATE MODE" << RESET << "\n";
        }
    }

    printPackageList(nativeTitleForPackageManager(packageManager, status.dnf5),
                     status.nativePackages,
                     status.native);
    printUniversalUpdateLists(status);

    return runUpdateFlow(
        options,
        status,
        nativeState,
        [] {
            return dryRunStep();
        },
        [] {
            return dryRunNativeUpdate();
        },
        [] {
            return dryRunStep();
        }
    ) ? 0 : 1;
}

int handleApt(const Options& options) {
    UpdateStatus status = aptCheckUpdates(options);

    if (status.checkError) {
        std::cerr << RED
                  << "Error: Could not check APT updates. Check " << kLogFile
                  << RESET << "\n";
        return 1;
    }

    if (!status.any()) {
        std::cout << "\n" << GREEN << "System is up to date!" << RESET << "\n";
        return 0;
    }

    if (options.fullUpdate) {
        std::cout << YELLOW << "[!] FULL UPDATE MODE" << RESET << "\n";
    }

    return aptUpdate(options, status) ? 0 : 1;
}

int handleZypper(const Options& options) {
    UpdateStatus status = zypperCheckUpdates(options);

    if (status.checkError) {
        std::cerr << RED
                  << "Error: Could not check Zypper updates. Check "
                  << kLogFile << " and " << kPatchLogFile
                  << RESET << "\n";
        return 1;
    }

    if (!status.any()) {
        std::cout << "\n" << GREEN << "System is up to date!" << RESET << "\n";
        return 0;
    }

    if (status.zypperDup) {
        std::cout << YELLOW << "[!] FULL UPDATE MODE (dup)" << RESET << "\n";
    }

    return zypperUpdate(options, status) ? 0 : 1;
}

int handleDnf(const Options& options) {
    UpdateStatus status = dnfCheckUpdates();

    if (status.checkError) {
        std::cerr << RED
                  << "Error: Could not check DNF updates. Check " << kLogFile
                  << RESET << "\n";
        return 1;
    }

    if (!status.any()) {
        std::cout << "\n" << GREEN << "System is up to date!" << RESET << "\n";
        return 0;
    }

    if (options.fullUpdate) {
        std::cout << YELLOW << "[!] FULL UPDATE MODE (distro-sync)" << RESET << "\n";
    }

    return dnfUpdate(options, status) ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[]) {
    Options options;

    if (!parseOptions(argc, argv, options)) {
        std::cerr << YELLOW << "Use --help to show available options." << RESET << "\n";
        return 1;
    }

    if (options.version && options.help) {
        std::cout << YELLOW << "--version" << RESET << "\n";
        printVersion();
        std::cout << "\n" << YELLOW << "--help" << RESET << "\n";
        printHelp(argv[0]);
        return 0;
    }

    if (options.version) {
        printVersion();
        return 0;
    }

    if (options.help) {
        printHelp(argv[0]);
        return 0;
    }

    if (options.dryRun) {
        SigintGuard sigintGuard;
        return handleDryRun(options);
    }

    if (geteuid() != 0) {
        std::cerr << RED << "Run with sudo!\n" << RESET;
        return 1;
    }

    zpm_update::checkForUpdates();

    const std::string packageManager = get_package_manager();
    if (packageManager == "unknown") {
        std::cerr << RED
                  << "Error: Could not detect a supported package manager "
                  << "(apt / zypper / dnf).\n"
                  << RESET;
        return 1;
    }

    FileLock lock;
    if (!lock.acquire()) {
        return 1;
    }

    SigintGuard sigintGuard;
    printRepositorySummary(packageManager);

    int result = 1;
    if (packageManager == "apt") {
        result = handleApt(options);
    } else if (packageManager == "zypper") {
        result = handleZypper(options);
    } else if (packageManager == "dnf") {
        result = handleDnf(options);
    }

    if (g_interrupted) {
        return 130;
    }
    return result;
}
