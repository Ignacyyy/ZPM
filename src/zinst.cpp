#include "main.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <thread>
#include <sys/file.h>
#include <sys/wait.h>

namespace {

constexpr const char* kLogPath = "/tmp/zinst.log";
constexpr std::size_t kMaxNativeSearchResults = 30;
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

enum class InstallSource {
    Native,
    Flatpak,
    Snap
};

enum class InstallStatus {
    Installed,
    AlreadyInstalled,
    WouldInstall,
    Failed,
    Interrupted
};

struct Options {
    bool showHelp = false;
    bool showVersion = false;
    bool dryRun = false;
    std::vector<std::string> packages;
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
    bool mirrorCapturedStdoutToLog = false;
    bool logStdout = false;
    bool logStderr = false;
    bool discardStdout = true;
    bool discardStderr = true;
    LogMode logMode = LogMode::Append;
    std::string header;
    std::vector<std::pair<std::string, std::string>> environment;
};

struct AppContext {
    std::string packageManager = "apt";
    std::string dnfCommand = "dnf";
    std::string flatpakFlag;
    bool hasFlatpak = false;
    bool hasSnap = false;
    bool nativeConsistencyChecked = false;
    bool aptCacheRefreshed = false;
};

struct PackageResult {
    std::string name;
    std::string message;
    bool success = false;
};

struct InstallTarget {
    std::string name;
    InstallSource source = InstallSource::Native;
};

struct InstallProgress {
    float startPct = 0.0f;
    float endPct = 100.0f;
    int totalSteps = 1;
    std::string label;
};

struct PackageCandidate {
    std::string name;
    std::string summary;
};

struct ResolveResult {
    std::string name;
    bool exists = false;
    bool truncated = false;
    std::vector<PackageCandidate> candidates;
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
        const char* paths[] = {"/run/zinst.lock", "/tmp/zinst.lock"};
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
            std::cerr << RED << "Error: Another zinst instance is already running.\n" << RESET;
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

bool isValidPackageArgument(const std::string& value) {
    return !value.empty() &&
           std::none_of(value.begin(), value.end(), [](unsigned char c) {
               return std::iscntrl(c) || std::isspace(c);
           });
}

std::vector<std::string> splitPackageArgument(const std::string& value) {
    std::vector<std::string> packages;
    std::string current;

    auto flush = [&] {
        const std::string package = trim(current);
        if (!package.empty()) {
            packages.push_back(package);
        }
        current.clear();
    };

    for (const unsigned char c : value) {
        if (std::isspace(c) || c == ',') {
            flush();
        } else {
            current.push_back(static_cast<char>(c));
        }
    }
    flush();

    return packages;
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

void addCandidate(std::vector<PackageCandidate>& candidates,
                  const std::string& name,
                  const std::string& summary,
                  bool&) {
    const std::string cleanedName = trim(name);
    if (cleanedName.empty()) {
        return;
    }

    const std::string cleanedSummary = trim(summary);
    for (PackageCandidate& candidate : candidates) {
        if (candidate.name == cleanedName) {
            if (candidate.summary.empty()) {
                candidate.summary = cleanedSummary;
            }
            return;
        }
    }

    candidates.push_back({cleanedName, cleanedSummary});
}

std::string regexEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() * 2);

    for (char c : value) {
        switch (c) {
            case '\\':
            case '.':
            case '^':
            case '$':
            case '|':
            case '(':
            case ')':
            case '[':
            case ']':
            case '{':
            case '}':
            case '*':
            case '+':
            case '?':
                escaped += '\\';
                [[fallthrough]];
            default:
                escaped += c;
        }
    }

    return escaped;
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
        fd.reset();

        if (mode != LogMode::Truncate) {
            return {};
        }

        struct stat st {};
        if (lstat(kLogPath, &st) != 0 || !S_ISREG(st.st_mode)) {
            return {};
        }

        if (unlink(kLogPath) != 0) {
            return {};
        }

        fd.reset(open(kLogPath, flags, 0600));
        if (!fd.valid() || !isSafeOwnedRegularFile(fd.get())) {
            return {};
        }
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

void writeLogLine(const std::string& line, LogMode mode = LogMode::Append) {
    FileDescriptor fd = openLog(mode);
    if (!fd.valid()) {
        return;
    }
    writeAll(fd.get(), line + "\n");
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
        const int textColumns = std::max(0,
                                         zpm::progressbar_detail::terminalWidth() -
                                             kLiveLogPrefixColumns);

        std::cout << "\r\033[K\n"
                  << "\r\033[K" << CYAN << "[ZPM-LOG]" << RESET << "\n";

        for (int row = 0; row < kLiveLogLines; ++row) {
            std::cout << "\r\033[K";
            if (row < static_cast<int>(lines.size())) {
                std::cout << CYAN << "> " << RESET
                          << zpm::progressbar_detail::sanitizeTask(lines[static_cast<std::size_t>(row)],
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

void beginInstallStep(float progress,
                      const std::string& progressText,
                      int& step,
                      const std::string& infoText,
                      LiveLogView* liveLog = nullptr) {
    const bool startProgressbar = step == 0;

    {
        std::lock_guard<std::mutex> outputLock(zpm::progressbar_detail::outputMutex());

        if (liveLog != nullptr) {
            liveLog->prepareForInfoAppendLocked();
        } else {
            std::cout << "\r\033[K";
        }

        const int textColumns = std::max(0,
                                         zpm::progressbar_detail::terminalWidth() - 4);

        std::cout << CYAN << "[>]" << RESET << " "
                  << zpm::progressbar_detail::sanitizeTask(infoText, textColumns)
                  << "\n";

        if (liveLog != nullptr) {
            liveLog->drawAtCursorLocked();
        }

        std::cout << std::flush;
    }

    ++step;
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

void redirectToDevNull(int targetFd) {
    const int nullFd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (nullFd >= 0) {
        dup2(nullFd, targetFd);
        close(nullFd);
    }
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
                          config.mirrorCapturedStdoutToLog ||
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

    const pid_t pid = fork();
    if (pid < 0) {
        return {};
    }

    if (pid == 0) {
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

ProcessResult capture(const std::vector<std::string>& args) {
    ProcessConfig config;
    config.captureStdout = true;
    return runProcess(args, config);
}

bool runQuiet(const std::vector<std::string>& args) {
    return runProcess(args).exitCode == 0;
}

int runLogged(const std::vector<std::string>& args,
              const std::string& header,
              const std::vector<std::pair<std::string, std::string>>& environment = {}) {
    ProcessConfig config;
    config.logStdout = true;
    config.logStderr = true;
    config.header = header;
    config.environment = environment;
    return runProcess(args, config).exitCode;
}

float progressBetween(float startPct, float endPct, float fraction) {
    const float safeFraction = std::clamp(fraction, 0.0f, 1.0f);
    return startPct + ((endPct - startPct) * safeFraction);
}

int installStepCount(InstallSource source) {
    switch (source) {
        case InstallSource::Native:
            return 8;
        case InstallSource::Flatpak:
        case InstallSource::Snap:
            return 7;
    }

    return 7;
}

float stepFraction(const InstallProgress& progress, int step) {
    const int safeTotal = std::max(progress.totalSteps, 1);
    const int safeStep = std::clamp(step, 0, safeTotal);
    return static_cast<float>(safeStep) / static_cast<float>(safeTotal);
}

std::string installStepLabel(const InstallProgress& progress,
                             int step,
                             const std::string& activity) {
    const int safeTotal = std::max(progress.totalSteps, 1);
    const int safeStep = std::clamp(step, 0, safeTotal);
    return std::to_string(safeStep) + "/" + std::to_string(safeTotal) +
           " | " + progress.label + " - " + activity;
}

void showInstallStep(const InstallProgress& progress,
                     int step,
                     const std::string& activity) {
    progressbar_update(progressBetween(progress.startPct,
                                       progress.endPct,
                                       stepFraction(progress, step)),
                       installStepLabel(progress, step, activity));
}

void finishInstallStep(const InstallProgress& progress, const std::string& activity) {
    progressbar_update(progress.endPct,
                       installStepLabel(progress, progress.totalSteps, activity));
}

int runLoggedStep(const std::vector<std::string>& args,
                  const std::string& header,
                  const InstallProgress& progress,
                  int step,
                  const std::string& activity,
                  const std::vector<std::pair<std::string, std::string>>& environment = {}) {
    showInstallStep(progress, step, activity + "...");
    return runLogged(args, header, environment);
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

std::string nativeLabel(const AppContext& context) {
    if (context.packageManager == "zypper") {
        return "Zypper: ";
    }
    if (context.packageManager == "dnf") {
        return context.dnfCommand == "dnf5" ? "DNF5:   " : "DNF:    ";
    }
    if (context.packageManager == "unknown") {
        return "Native: ";
    }
    return "APT:    ";
}

std::string nativeShortLabel(const AppContext& context) {
    if (context.packageManager == "zypper") {
        return "Zypper";
    }
    if (context.packageManager == "dnf") {
        return context.dnfCommand == "dnf5" ? "DNF5" : "DNF";
    }
    if (context.packageManager == "unknown") {
        return "Native";
    }
    return "APT";
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

bool outputContainsExactLine(const std::string& output, const std::string& wanted) {
    for (const std::string& rawLine : splitLines(output)) {
        if (trim(rawLine) == wanted) {
            return true;
        }
    }
    return false;
}

std::string getFlatpakRemoteFlag() {
    const ProcessResult systemRemotes = capture({"flatpak", "remotes", "--system", "--columns=name"});
    if (systemRemotes.exitCode == 0 && outputContainsExactLine(systemRemotes.output, "flathub")) {
        return "--system";
    }

    const ProcessResult userRemotes = capture({"flatpak", "remotes", "--user", "--columns=name"});
    if (userRemotes.exitCode == 0 && outputContainsExactLine(userRemotes.output, "flathub")) {
        return "--user";
    }

    return {};
}

std::vector<std::string> searchFlatpak(const AppContext& context, const std::string& query) {
    std::vector<std::string> args = {"flatpak", "search"};
    if (!context.flatpakFlag.empty()) {
        args.push_back(context.flatpakFlag);
    }
    args.push_back("--columns=application");
    args.push_back(query);

    std::vector<std::string> results = parseFlatpakApplications(capture(args).output);
    const std::string lowerQuery = toLower(query);

    std::vector<std::string> filtered;
    for (const std::string& app : results) {
        if (toLower(app).find(lowerQuery) != std::string::npos) {
            addUnique(filtered, app);
        }
    }

    std::sort(filtered.begin(), filtered.end());
    return filtered;
}

bool isInstalledNative(const AppContext& context, const std::string& package) {
    if (context.packageManager == "apt") {
        const ProcessResult result = capture({"dpkg-query", "-W", "-f=${Status}", package});
        return result.exitCode == 0 &&
               result.output.find("install ok installed") != std::string::npos;
    }

    return runQuiet({"rpm", "-q", package});
}

bool isInstalledFlatpak(const AppContext& context, const std::string& package) {
    if (!context.hasFlatpak) {
        return false;
    }

    std::vector<std::string> args = {"flatpak", "info"};
    if (!context.flatpakFlag.empty()) {
        args.push_back(context.flatpakFlag);
    }
    args.push_back(package);

    return runQuiet(args);
}

bool isInstalledSnap(const AppContext& context, const std::string& package) {
    return context.hasSnap && runQuiet({"snap", "list", package});
}

bool snapPackageExists(const AppContext& context, const std::string& package) {
    return context.hasSnap && runQuiet({"snap", "info", package});
}

void parseAptNativeResults(const std::string& output,
                           std::vector<PackageCandidate>& candidates,
                           bool& truncated) {
    for (const std::string& rawLine : splitLines(output)) {
        const std::string line = trim(rawLine);
        const std::size_t dash = line.find(" - ");
        if (dash == std::string::npos) {
            continue;
        }

        addCandidate(candidates,
                     line.substr(0, dash),
                     line.substr(dash + 3),
                     truncated);
    }
}

std::string aptPackageSummary(const std::string& output) {
    for (const std::string& rawLine : splitLines(output)) {
        const std::string line = trim(rawLine);
        const std::size_t separator = line.find(':');
        if (separator == std::string::npos) {
            continue;
        }

        const std::string key = line.substr(0, separator);
        if (key == "Description-en" || key == "Description") {
            return trim(line.substr(separator + 1));
        }
    }

    return {};
}

bool isTransitionalPackage(const PackageCandidate& candidate) {
    const std::string summary = toLower(candidate.summary);
    return summary.find("transitional package") != std::string::npos ||
           summary.find("dummy transitional") != std::string::npos ||
           summary.find("pakiet przej") != std::string::npos;
}

int nativeCandidateRank(const std::string& query, const PackageCandidate& candidate) {
    const std::string queryLower = toLower(query);
    const std::string nameLower = toLower(candidate.name);
    const std::string summaryLower = toLower(candidate.summary);
    const bool exact = nameLower == queryLower;
    const bool prefix = startsWith(nameLower, queryLower);
    const bool nameContains =
        queryLower.size() >= 3 && nameLower.find(queryLower) != std::string::npos;
    const bool summaryContains =
        queryLower.size() >= 4 && summaryLower.find(queryLower) != std::string::npos;
    const bool transitional = isTransitionalPackage(candidate);

    if (queryLower.empty()) {
        return 10;
    }
    if (exact && !transitional) {
        return 0;
    }
    if (!transitional && nameLower == queryLower + "-installer") {
        return 1;
    }
    if (!transitional && nameLower == queryLower + "-launcher") {
        return 2;
    }
    if (prefix && !transitional) {
        return 3;
    }
    if (nameContains && !transitional) {
        return 4;
    }
    if (summaryContains && !transitional) {
        return 5;
    }
    if (!transitional) {
        return 10;
    }
    if (exact) {
        return 6;
    }
    if (prefix) {
        return 7;
    }
    if (nameContains) {
        return 8;
    }
    if (summaryContains) {
        return 9;
    }
    return 10;
}

bool candidateMatchesNativeQuery(const std::string& query, const PackageCandidate& candidate) {
    return nativeCandidateRank(query, candidate) < 10;
}

void sortNativeResults(const std::string& query, std::vector<PackageCandidate>& candidates) {
    std::stable_sort(candidates.begin(),
                     candidates.end(),
                     [&](const PackageCandidate& left, const PackageCandidate& right) {
                         const int leftRank = nativeCandidateRank(query, left);
                         const int rightRank = nativeCandidateRank(query, right);
                         if (leftRank != rightRank) {
                             return leftRank < rightRank;
                         }
                         return toLower(left.name) < toLower(right.name);
                     });
}

std::vector<PackageCandidate>::const_iterator exactNativeCandidate(
    const std::vector<PackageCandidate>& candidates,
    const std::string& query) {
    const std::string queryLower = toLower(query);
    return std::find_if(candidates.begin(),
                        candidates.end(),
                        [&](const PackageCandidate& candidate) {
                            return toLower(candidate.name) == queryLower &&
                                   !isTransitionalPackage(candidate);
                        });
}

void finalizeNativeCandidates(const std::string& query, ResolveResult& result) {
    std::vector<PackageCandidate> filtered;
    filtered.reserve(result.candidates.size());

    for (const PackageCandidate& candidate : result.candidates) {
        if (candidateMatchesNativeQuery(query, candidate)) {
            filtered.push_back(candidate);
        }
    }

    result.candidates = std::move(filtered);
    sortNativeResults(query, result.candidates);

    if (result.candidates.size() > kMaxNativeSearchResults) {
        result.candidates.resize(kMaxNativeSearchResults);
        result.truncated = true;
    }
}

void parseZypperNativeResults(const std::string& output,
                              std::vector<PackageCandidate>& candidates,
                              bool& truncated) {
    bool pastHeader = false;

    for (const std::string& rawLine : splitLines(output)) {
        const std::string line = trim(rawLine);
        if (line.find("-+-") != std::string::npos) {
            pastHeader = true;
            continue;
        }
        if (!pastHeader || line.empty()) {
            continue;
        }

        const std::vector<std::string> columns = split(line, '|');
        if (columns.size() < 3) {
            continue;
        }

        const std::string name = trim(columns[1]);
        const std::string type = columns.size() >= 4 ? toLower(trim(columns[3])) : std::string {};
        if (name.empty() || name == "Name" || (!type.empty() && type != "package")) {
            continue;
        }

        addCandidate(candidates, name, columns[2], truncated);
    }
}

bool isDnfMetadataLine(const std::string& line) {
    const std::string lower = toLower(trim(line));
    if (lower.empty() || lower.find("====") != std::string::npos) {
        return true;
    }

    return startsWith(lower, "last metadata expiration check") ||
           startsWith(lower, "updating and loading repositories") ||
           startsWith(lower, "repositories loaded") ||
           startsWith(lower, "loading repositories") ||
           startsWith(lower, "matched fields") ||
           startsWith(lower, "name ") ||
           startsWith(lower, "package ") ||
           startsWith(lower, "summary ") ||
           lower == "no matches found.";
}

bool isKnownRpmArch(const std::string& value) {
    static constexpr std::array<const char*, 12> kKnownArch {
        "x86_64", "aarch64", "noarch", "i686", "i586", "i386",
        "ppc64le", "s390x", "armv7hl", "armhfp", "riscv64", "src"
    };

    return std::any_of(kKnownArch.begin(), kKnownArch.end(), [&](const char* arch) {
        return value == arch;
    });
}

std::string stripRpmArch(const std::string& package) {
    const std::size_t dot = package.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= package.size()) {
        return package;
    }

    const std::string suffix = package.substr(dot + 1);
    return isKnownRpmArch(suffix) ? package.substr(0, dot) : package;
}

bool looksLikePackageToken(const std::string& value) {
    bool hasVisibleCharacter = false;

    for (unsigned char c : value) {
        if (std::iscntrl(c) || std::isspace(c)) {
            return false;
        }
        if (!std::ispunct(c)) {
            hasVisibleCharacter = true;
        }
    }

    return hasVisibleCharacter;
}

void parseDnfNativeResults(const std::string& output,
                           std::vector<PackageCandidate>& candidates,
                           bool& truncated) {
    for (const std::string& rawLine : splitLines(output)) {
        const std::string line = trim(rawLine);
        if (isDnfMetadataLine(line)) {
            continue;
        }

        const std::size_t nameStart = line.find_first_not_of(" \t");
        if (nameStart == std::string::npos) {
            continue;
        }

        const std::size_t separator = line.find_first_of(":|", nameStart);
        std::string package;
        std::string summary;

        if (separator != std::string::npos) {
            package = trim(line.substr(nameStart, separator - nameStart));
            summary = trim(line.substr(separator + 1));
        } else {
            std::istringstream stream(line.substr(nameStart));
            stream >> package;
            std::getline(stream, summary);
            summary = trim(summary);
        }

        if (!looksLikePackageToken(package)) {
            continue;
        }

        addCandidate(candidates, stripRpmArch(package), summary, truncated);
    }
}

ResolveResult finishResolveResult(const std::string& query, ResolveResult result) {
    finalizeNativeCandidates(query, result);
    result.exists = !result.candidates.empty();
    result.name = result.exists ? result.candidates.front().name : query;
    return result;
}

ResolveResult resolveNative(const AppContext& context, const std::string& package) {
    ResolveResult result;
    result.name = package;

    if (context.packageManager == "apt") {
        const std::string pattern = "^" + regexEscape(package);
        parseAptNativeResults(capture({"apt-cache", "search", "--names-only", pattern}).output,
                              result.candidates,
                              result.truncated);

        const ProcessResult exactInfo =
            capture({"apt-cache", "show", "--no-all-versions", package});
        if (exactInfo.exitCode == 0 && !exactInfo.output.empty()) {
            addCandidate(result.candidates,
                         package,
                         aptPackageSummary(exactInfo.output),
                         result.truncated);
        }

        return finishResolveResult(package, std::move(result));
    }

    if (context.packageManager == "zypper") {
        if (runQuiet({"zypper", "--no-refresh", "info", package})) {
            addCandidate(result.candidates, package, {}, result.truncated);
        }

        parseZypperNativeResults(capture({"zypper", "--no-refresh", "search", package}).output,
                                 result.candidates,
                                 result.truncated);

        return finishResolveResult(package, std::move(result));
    }

    if (!context.dnfCommand.empty() && runQuiet({context.dnfCommand, "info", package})) {
        addCandidate(result.candidates, package, {}, result.truncated);
    }

    if (!context.dnfCommand.empty()) {
        parseDnfNativeResults(capture({context.dnfCommand, "search", package}).output,
                              result.candidates,
                              result.truncated);
    }

    return finishResolveResult(package, std::move(result));
}

void printHelp(const char* progName) {
    std::cout << RED << "Usage: " << RESET << progName << " [options] [packages...]"
              << " or zpm inst/install [options] [packages...]\n"
              << RED << "Options:\n" << RESET
	              << "  (auto)         Picks native PM / Flatpak / Snap per package\n"
	              << "  packages       Accepts many names: zinst git curl htop\n"
	              << "                 Also accepts quoted/comma lists: zinst \"git curl\" or zinst git,curl\n"
	              << "  --dry-run      Simulate program flow; fake packages are allowed\n"
              << "  --version, -v  Show version information\n"
              << "  --help,    -h  Show this help message\n";
}

void printVersion() {
    std::cout << RED << "zinst component version: v" << zpm_version::version()
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
            const std::vector<std::string> packages = splitPackageArgument(arg);
            if (packages.empty()) {
                errors.push_back("Invalid package name: " + arg);
                continue;
            }

            for (const std::string& package : packages) {
                if (!isValidPackageArgument(package)) {
                    errors.push_back("Invalid package name: " + package);
                    continue;
                }
                options.packages.push_back(package);
            }
        }
    }

    if ((options.showHelp || options.showVersion) &&
        (options.dryRun || !options.packages.empty())) {
        errors.push_back("--help and --version can only be combined with each other.");
    }

    for (const std::string& error : errors) {
        std::cerr << RED << "Error: " << error << RESET << "\n";
    }

    return errors.empty();
}

bool readChoice(std::string& input) {
    if (g_interrupted) {
        return false;
    }

    return static_cast<bool>(std::getline(std::cin, input));
}

std::string chooseSourceMenu(const AppContext& context,
                             const std::string& package,
                             const ResolveResult& native,
                             bool snapAvailable,
                             const std::vector<std::string>& flatpakResults) {
    const bool nativeAvailable = native.exists;
    const bool flatpakAvailable = !flatpakResults.empty();

    struct Option {
        int number = 0;
        std::string key;
    };

    std::vector<Option> options;
    int index = 1;

    std::cout << "\n" << BOLD << "Package: " << CYAN << package << RESET << "\n";

    std::cout << "  " << BOLD << index << ". " << nativeLabel(context) << RESET;
    if (nativeAvailable) {
        const auto exact = exactNativeCandidate(native.candidates, package);
        if (exact != native.candidates.end() && native.candidates.size() == 1) {
            std::cout << GREEN << "exist (" << exact->name << ")" << RESET << "\n";
        } else if (native.candidates.size() > 1) {
            std::cout << GREEN << "exist (" << native.candidates.size() << " results";
            if (native.truncated) {
                std::cout << ", showing first " << kMaxNativeSearchResults;
            }
            if (exact != native.candidates.end()) {
                std::cout << ", first result: " << exact->name;
            }
            std::cout << ")" << RESET << "\n";
        } else {
            std::cout << GREEN << "exist (" << native.name << ")" << RESET << "\n";
        }
        options.push_back({index, "native"});
    } else {
        std::cout << RED << "none" << RESET << "\n";
    }
    ++index;

    if (context.hasSnap) {
        std::cout << "  " << BOLD << index << ". Snap:    " << RESET;
        if (snapAvailable) {
            std::cout << GREEN << "exist (" << package << ")" << RESET << "\n";
            options.push_back({index, "snap"});
        } else {
            std::cout << RED << "none" << RESET << "\n";
        }
        ++index;
    }

    if (context.hasFlatpak) {
        std::cout << "  " << BOLD << index << ". Flatpak: " << RESET;
        if (flatpakAvailable) {
            std::cout << GREEN << "exist (" << flatpakResults.size() << " result"
                      << (flatpakResults.size() != 1 ? "s" : "") << ")" << RESET << "\n";
            options.push_back({index, "flatpak"});
        } else {
            std::cout << RED << "none" << RESET << "\n";
        }
        ++index;
    }

    std::cout << "  " << BOLD << "0. Skip" << RESET << "\n";

    if (options.empty()) {
        std::cout << YELLOW << "No source available for '" << package << "'." << RESET << "\n";
        return {};
    }

    for (;;) {
        std::cout << BOLD << "Choose: " << RESET;

        std::string input;
        if (!readChoice(input)) {
            return {};
        }

        int choice = -1;
        try {
            choice = std::stoi(trim(input));
        } catch (...) {
            choice = -1;
        }

        if (choice == 0) {
            return {};
        }

        for (const Option& option : options) {
            if (option.number == choice) {
                return option.key;
            }
        }

        std::cout << RED << "Invalid choice, try again.\n" << RESET;
    }
}

std::string chooseNativePackage(const AppContext& context,
                                const ResolveResult& native,
                                const std::string& query) {
    if (native.candidates.empty()) {
        std::cout << YELLOW << "No " << nativeShortLabel(context)
                  << " packages found for '" << query << "'.\n" << RESET;
        return {};
    }

    if (native.candidates.size() == 1) {
        return native.candidates.front().name;
    }

    std::cout << GREEN << "\n" << nativeShortLabel(context)
              << " results for '" << query << "':\n" << RESET;
    for (size_t i = 0; i < native.candidates.size(); ++i) {
        const PackageCandidate& candidate = native.candidates[i];
        std::cout << "  " << (i + 1) << ". " << candidate.name;
        if (!candidate.summary.empty()) {
            std::cout << " - " << candidate.summary;
        }
        std::cout << "\n";
    }
    if (native.truncated) {
        std::cout << "  ... showing first " << kMaxNativeSearchResults << " results\n";
    }
    std::cout << "  0. Cancel\n";

    for (;;) {
        std::cout << BOLD << "Choose: " << RESET;

        std::string input;
        if (!readChoice(input)) {
            return {};
        }

        int choice = -1;
        try {
            choice = std::stoi(trim(input));
        } catch (...) {
            choice = -1;
        }

        if (choice == 0) {
            return {};
        }

        if (choice >= 1 && choice <= static_cast<int>(native.candidates.size())) {
            return native.candidates[static_cast<size_t>(choice - 1)].name;
        }

        std::cout << RED << "Invalid choice, try again.\n" << RESET;
    }
}

std::string chooseFlatpakPackage(const std::vector<std::string>& packages,
                                 const std::string& query) {
    if (packages.empty()) {
        std::cout << YELLOW << "No Flatpak packages found for '" << query << "'.\n" << RESET;
        return {};
    }

    std::cout << GREEN << "\nFlatpak results for '" << query << "':\n" << RESET;
    for (size_t i = 0; i < packages.size(); ++i) {
        std::cout << "  " << (i + 1) << ". " << packages[i] << "\n";
    }
    std::cout << "  0. Cancel\n";

    for (;;) {
        std::cout << BOLD << "Choose: " << RESET;

        std::string input;
        if (!readChoice(input)) {
            return {};
        }

        int choice = -1;
        try {
            choice = std::stoi(trim(input));
        } catch (...) {
            choice = -1;
        }

        if (choice == 0) {
            return {};
        }

        if (choice >= 1 && choice <= static_cast<int>(packages.size())) {
            return packages[static_cast<size_t>(choice - 1)];
        }

        std::cout << RED << "Invalid choice, try again.\n" << RESET;
    }
}

std::vector<InstallTarget> resolveTargets(const AppContext& context,
                                          const std::vector<std::string>& packages) {
    std::vector<InstallTarget> targets;

    for (const std::string& package : packages) {
        if (g_interrupted) {
            break;
        }

        const ResolveResult native = resolveNative(context, package);
        const bool snapAvailable = snapPackageExists(context, package);
        const std::vector<std::string> flatpakResults =
            context.hasFlatpak ? searchFlatpak(context, package) : std::vector<std::string>{};

        const std::string source = chooseSourceMenu(context,
                                                    package,
                                                    native,
                                                    snapAvailable,
                                                    flatpakResults);

        if (source == "native") {
            const std::string selected = chooseNativePackage(context, native, package);
            if (!selected.empty()) {
                targets.push_back({selected, InstallSource::Native});
            }
        } else if (source == "snap") {
            targets.push_back({package, InstallSource::Snap});
        } else if (source == "flatpak") {
            const bool exactMatch =
                std::find(flatpakResults.begin(), flatpakResults.end(), package) != flatpakResults.end();
            const std::string selected = exactMatch
                ? package
                : chooseFlatpakPackage(flatpakResults, package);

            if (!selected.empty()) {
                targets.push_back({selected, InstallSource::Flatpak});
            }
        }
    }

    return targets;
}

std::vector<std::string> dryRunPackages(const std::vector<std::string>& packages) {
    if (!packages.empty()) {
        return packages;
    }

    return {
        "fake-editor",
        "fake-browser",
        "fake-toolkit"
    };
}

std::vector<InstallTarget> buildDryRunTargets(const AppContext& context,
                                              const std::vector<std::string>& packages) {
    std::vector<InstallSource> sources = {InstallSource::Native};
    if (context.hasFlatpak) {
        sources.push_back(InstallSource::Flatpak);
    }
    if (context.hasSnap) {
        sources.push_back(InstallSource::Snap);
    }

    std::vector<InstallTarget> targets;
    const std::vector<std::string> names = dryRunPackages(packages);
    targets.reserve(names.size());

    for (size_t i = 0; i < names.size(); ++i) {
        targets.push_back({names[i], sources[i % sources.size()]});
    }

    return targets;
}

std::string sourceName(const AppContext& context, InstallSource source) {
    switch (source) {
        case InstallSource::Native:
            return nativeShortLabel(context);
        case InstallSource::Flatpak:
            return "Flatpak";
        case InstallSource::Snap:
            return "Snap";
    }

    return "Unknown";
}

std::string installInfoText(const AppContext& context,
                            const InstallTarget& target,
                            bool dryRun) {
    return std::string(dryRun ? "simulating " : "installing ") +
           sourceName(context, target.source) + " package: " + target.name;
}

bool targetAlreadyInstalled(const AppContext& context, const InstallTarget& target) {
    switch (target.source) {
        case InstallSource::Native:
            return isInstalledNative(context, target.name);
        case InstallSource::Flatpak:
            return isInstalledFlatpak(context, target.name);
        case InstallSource::Snap:
            return isInstalledSnap(context, target.name);
    }

    return false;
}

bool ensureNativeConsistency(AppContext& context, const InstallProgress& progress) {
    if (context.nativeConsistencyChecked) {
        showInstallStep(progress, 3, "system already checked");
        return true;
    }

    int exitCode = 0;
    if (context.packageManager == "apt") {
        exitCode = runLoggedStep({"dpkg", "--configure", "-a"},
                                 "-----checking_system_consistency-----",
                                 progress,
                                 3,
                                 "checking system consistency",
                                 {{"DEBIAN_FRONTEND", "noninteractive"}});
    } else if (context.packageManager == "zypper" || context.packageManager == "dnf") {
        exitCode = runLoggedStep({"rpm", "--rebuilddb"},
                                 "-----checking_system_consistency-----",
                                 progress,
                                 3,
                                 "checking system consistency");
    }

    if (g_interrupted) {
        return false;
    }

    if (exitCode != 0) {
        return false;
    }

    context.nativeConsistencyChecked = true;
    showInstallStep(progress, 3, "system ready");
    return true;
}

bool checkNativeRepositories(AppContext& context,
                             const std::string& package,
                             const InstallProgress& progress) {
    if (context.packageManager == "apt") {
        if (context.aptCacheRefreshed) {
            showInstallStep(progress, 4, "repositories already checked");
            return true;
        }

        const int updateExit = runLoggedStep({"apt-get", "update", "-qq"},
                                             "-----apt_update-----",
                                             progress,
                                             4,
                                             "checking repositories");
        if (g_interrupted) {
            return false;
        }
        if (updateExit != 0) {
            return false;
        }

        context.aptCacheRefreshed = true;
        showInstallStep(progress, 4, "repository metadata ready");
        return true;
    }

    if (context.packageManager == "zypper") {
        runLoggedStep({"zypper", "repos"},
                      "-----zypper_check_repositories_" + package + "-----",
                      progress,
                      4,
                      "checking repositories");
        if (g_interrupted) {
            return false;
        }
        showInstallStep(progress, 4, "repositories checked");
        return true;
    }

    if (context.packageManager == "dnf") {
        runLoggedStep({context.dnfCommand, "repolist", "-q"},
                      "-----" + context.dnfCommand + "_check_repositories_" + package + "-----",
                      progress,
                      4,
                      "checking repositories");
        if (g_interrupted) {
            return false;
        }
        showInstallStep(progress, 4, "repositories checked");
        return true;
    }

    showInstallStep(progress, 4, "repositories checked");
    return true;
}

InstallStatus installNative(AppContext& context,
                            const std::string& package,
                            const InstallProgress& progress) {
    if (!ensureNativeConsistency(context, progress)) {
        if (g_interrupted) {
            return InstallStatus::Interrupted;
        }
        finishInstallStep(progress, "failed");
        return InstallStatus::Failed;
    }

    if (!checkNativeRepositories(context, package, progress)) {
        if (g_interrupted) {
            return InstallStatus::Interrupted;
        }
        finishInstallStep(progress, "failed");
        return InstallStatus::Failed;
    }

    showInstallStep(progress, 5, "preparing transaction");

    int exitCode = 1;
    if (context.packageManager == "apt") {
        exitCode = runLoggedStep({"apt-get", "install", "-y", package},
                                 "-----apt_install_" + package + "-----",
                                 progress,
                                 6,
                                 "installing package",
                                 {{"DEBIAN_FRONTEND", "noninteractive"}});
    } else if (context.packageManager == "zypper") {
        exitCode = runLoggedStep({"zypper", "--non-interactive", "install", "-y", package},
                                 "-----zypper_install_" + package + "-----",
                                 progress,
                                 6,
                                 "installing package");
    } else if (context.packageManager == "dnf") {
        exitCode = runLoggedStep({context.dnfCommand, "install", "-y", package},
                                 "-----" + context.dnfCommand + "_install_" + package + "-----",
                                 progress,
                                 6,
                                 "installing package");
    }

    if (g_interrupted) {
        return InstallStatus::Interrupted;
    }

    if (exitCode != 0) {
        finishInstallStep(progress, "failed");
        return InstallStatus::Failed;
    }

    showInstallStep(progress, 7, "verifying installation");
    if (!isInstalledNative(context, package)) {
        finishInstallStep(progress, "verification failed");
        return InstallStatus::Failed;
    }

    showInstallStep(progress, 8, "finalizing");
    finishInstallStep(progress, "done");
    return InstallStatus::Installed;
}

InstallStatus installFlatpak(const AppContext& context,
                             const std::string& package,
                             const InstallProgress& progress) {
    std::vector<std::string> remoteArgs = {"flatpak", "remotes"};
    if (!context.flatpakFlag.empty()) {
        remoteArgs.push_back(context.flatpakFlag);
    }
    remoteArgs.push_back("--columns=name");

    const int remoteExit = runLoggedStep(remoteArgs,
                                         "-----flatpak_check_remotes_" + package + "-----",
                                         progress,
                                         3,
                                         "checking repositories");
    if (g_interrupted) {
        return InstallStatus::Interrupted;
    }
    if (remoteExit != 0) {
        finishInstallStep(progress, "failed");
        return InstallStatus::Failed;
    }

    showInstallStep(progress, 3, "flathub ready");
    showInstallStep(progress, 4, "preparing transaction");

    std::vector<std::string> args = {"flatpak", "install"};
    if (!context.flatpakFlag.empty()) {
        args.push_back(context.flatpakFlag);
    }
    args.push_back("-y");
    args.push_back("--noninteractive");
    args.push_back("flathub");
    args.push_back(package);

    const int exitCode = runLoggedStep(args,
                                       "-----flatpak_install_" + package + "-----",
                                       progress,
                                       5,
                                       "installing package");
    if (g_interrupted) {
        return InstallStatus::Interrupted;
    }

    if (exitCode != 0) {
        finishInstallStep(progress, "failed");
        return InstallStatus::Failed;
    }

    showInstallStep(progress, 6, "verifying installation");
    if (!isInstalledFlatpak(context, package)) {
        finishInstallStep(progress, "verification failed");
        return InstallStatus::Failed;
    }

    showInstallStep(progress, 7, "finalizing");
    finishInstallStep(progress, "done");
    return InstallStatus::Installed;
}

InstallStatus installSnap(const AppContext& context,
                          const std::string& package,
                          const InstallProgress& progress) {
    const int storeExit = runLoggedStep({"snap", "info", package},
                                        "-----snap_check_store_" + package + "-----",
                                        progress,
                                        3,
                                        "checking store");
    if (g_interrupted) {
        return InstallStatus::Interrupted;
    }
    if (storeExit != 0) {
        finishInstallStep(progress, "failed");
        return InstallStatus::Failed;
    }

    showInstallStep(progress, 3, "store ready");
    showInstallStep(progress, 4, "preparing transaction");

    const int exitCode = runLoggedStep({"snap", "install", package},
                                       "-----snap_install_" + package + "-----",
                                       progress,
                                       5,
                                       "installing package");
    if (g_interrupted) {
        return InstallStatus::Interrupted;
    }

    if (exitCode != 0) {
        finishInstallStep(progress, "failed");
        return InstallStatus::Failed;
    }

    showInstallStep(progress, 6, "verifying installation");
    if (!isInstalledSnap(context, package)) {
        finishInstallStep(progress, "verification failed");
        return InstallStatus::Failed;
    }

    showInstallStep(progress, 7, "finalizing");
    finishInstallStep(progress, "done");
    return InstallStatus::Installed;
}

InstallStatus installTarget(AppContext& context,
                            const InstallTarget& target,
                            bool dryRun,
                            float startPct,
                            float endPct,
                            int index,
                            int total) {
    const std::string label = std::to_string(index) + "/" + std::to_string(total) +
                              " | " + sourceName(context, target.source) + ": " + target.name;
    const InstallProgress progress {
        startPct,
        endPct,
        installStepCount(target.source),
        label
    };

    showInstallStep(progress, 1, "checking selected source");

    if (dryRun) {
        showInstallStep(progress, 2, "would check installed state");
        if (target.source == InstallSource::Native) {
            showInstallStep(progress, 3, "would check system consistency");
            showInstallStep(progress, 4, "would check repositories");
            showInstallStep(progress, 5, "would prepare transaction");
            showInstallStep(progress, 6, "would install package");
            showInstallStep(progress, 7, "would verify installation");
        } else {
            showInstallStep(progress, 3, target.source == InstallSource::Snap
                                       ? "would check store"
                                       : "would check repositories");
            showInstallStep(progress, 4, "would prepare transaction");
            showInstallStep(progress, 5, "would install package");
            showInstallStep(progress, 6, "would verify installation");
        }
        finishInstallStep(progress, "would install");
        return InstallStatus::WouldInstall;
    }

    showInstallStep(progress, 2, "checking installed state");
    if (targetAlreadyInstalled(context, target)) {
        finishInstallStep(progress, "already installed");
        return InstallStatus::AlreadyInstalled;
    }

    switch (target.source) {
        case InstallSource::Native:
            return installNative(context, target.name, progress);
        case InstallSource::Flatpak:
            return installFlatpak(context, target.name, progress);
        case InstallSource::Snap:
            return installSnap(context, target.name, progress);
    }

    return InstallStatus::Failed;
}

int runInstallLoop(AppContext& context,
                   const std::vector<InstallTarget>& targets,
                   bool dryRun) {
    std::vector<PackageResult> results;
    bool anyFailed = false;
    const int total = static_cast<int>(targets.size());

    if (!dryRun) {
        writeLogLine("-----zinst_start-----", LogMode::Truncate);
    }

    int infoStep = 0;
    printInfoHeader();

    LiveLogView liveLog(kLogPath, !dryRun);
    LiveLogView* liveLogView = dryRun ? nullptr : &liveLog;
    liveLog.start();

    for (int i = 0; i < total; ++i) {
        if (g_interrupted) {
            liveLog.stop();
            progressbar_finish("Cancelled!");
            std::cout << "\n" << YELLOW << "Cancelled.\n" << RESET;
            return 130;
        }

        const InstallTarget& target = targets[static_cast<size_t>(i)];
        const float startPct = (100.0f * static_cast<float>(i)) / static_cast<float>(total);
        const float endPct = (100.0f * static_cast<float>(i + 1)) / static_cast<float>(total);
        const std::string progressText = "0/" +
                                         std::to_string(installStepCount(target.source)) +
                                         " | " + std::to_string(i + 1) + "/" +
                                         std::to_string(total) + " | " +
                                         sourceName(context, target.source) + ": " +
                                         target.name;

        PackageResult result;
        result.name = target.name;

        beginInstallStep(startPct,
                         progressText,
                         infoStep,
                         installInfoText(context, target, dryRun),
                         liveLogView);

        const InstallStatus status = installTarget(context,
                                                   target,
                                                   dryRun,
                                                   startPct,
                                                   endPct,
                                                   i + 1,
                                                   total);

        if (status == InstallStatus::Interrupted) {
            liveLog.stop();
            progressbar_finish("Cancelled!");
            std::cout << "\n" << YELLOW << "Cancelled.\n" << RESET;
            return 130;
        }

        switch (status) {
            case InstallStatus::AlreadyInstalled:
                result.message = YELLOW + "Package " + target.name + " is already installed." + RESET;
                result.success = true;
                break;
            case InstallStatus::WouldInstall:
                result.message = "Package " + target.name + " would be installed from " +
                                 sourceName(context, target.source) + ".";
                result.success = true;
                break;
            case InstallStatus::Installed:
                result.message = "Package " + target.name + " installed successfully.";
                result.success = true;
                break;
            case InstallStatus::Failed:
                result.message = RED + "Package " + target.name + " installation failed." + RESET;
                anyFailed = true;
                break;
            case InstallStatus::Interrupted:
                break;
        }

        if (!dryRun) {
            switch (status) {
                case InstallStatus::AlreadyInstalled:
                    writeLogLine("install: " + target.name + " already installed");
                    break;
                case InstallStatus::Installed:
                    writeLogLine("install: " + target.name + " installed successfully");
                    break;
                case InstallStatus::Failed:
                    writeLogLine("install: " + target.name + " failed");
                    break;
                case InstallStatus::WouldInstall:
                case InstallStatus::Interrupted:
                    break;
            }
        }

        results.push_back(result);
    }

    if (anyFailed) {
        liveLog.stop();
        progressbar_finish("Done with errors!");
        std::cout << "\n";
        for (const PackageResult& result : results) {
            std::cout << result.message << "\n";
        }
        std::cout << RED << "Installation finished with errors!\n" << RESET;
        std::cout << YELLOW << "[RAPORT] " << RESET << kLogPath << "\n";
        return 1;
    }

    liveLog.stop();
    progressbar_finish(dryRun ? "Dry run done!" : "Done!");
    std::cout << "\n";
    for (const PackageResult& result : results) {
        std::cout << result.message << "\n";
    }
    if (!dryRun) {
        std::cout << GREEN << "Installation complete!\n" << RESET;
        std::cout << YELLOW << "[RAPORT] " << RESET << kLogPath << "\n";
    }

    return 0;
}

AppContext buildContext() {
    AppContext context;
    context.packageManager = get_package_manager();
    context.dnfCommand = commandExists("dnf5") ? "dnf5" : "dnf";
    context.hasFlatpak = commandExists("flatpak");
    context.hasSnap = commandExists("snap");

    if (context.hasFlatpak) {
        context.flatpakFlag = getFlatpakRemoteFlag();
        if (context.flatpakFlag.empty()) {
            std::cout << YELLOW << "Warning: No flathub remote found for flatpak "
                      << "(tried --system and --user).\n" << RESET;
            context.hasFlatpak = false;
        }
    }

    return context;
}

AppContext buildDryRunContext() {
    AppContext context;
    context.packageManager = get_package_manager();
    if (context.packageManager == "unknown") {
        context.packageManager = "apt";
    }
    context.dnfCommand = commandExists("dnf5") ? "dnf5" : "dnf";
    context.hasFlatpak = commandExists("flatpak");
    context.hasSnap = commandExists("snap");
    if (context.hasFlatpak) {
        context.flatpakFlag = "--system";
    }
    return context;
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

    if (options.packages.empty() && !options.dryRun) {
        std::cout << YELLOW << "No package specified!\n" << RESET;
        return 1;
    }

    SigintGuard sigintGuard;

    if (options.dryRun) {
        AppContext context = buildDryRunContext();
        std::vector<InstallTarget> targets = buildDryRunTargets(context, options.packages);

        return runInstallLoop(context, targets, true);
    }

    if (!options.dryRun && geteuid() != 0) {
        std::cout << RED << "Run with sudo!\n" << RESET;
        return 1;
    }

    zpm_update::checkForUpdates();

    AppContext context = buildContext();

    if (context.packageManager == "unknown") {
        std::cout << RED << "Error: Could not detect a supported package manager "
                  << "(apt / zypper / dnf).\n" << RESET;
        return 1;
    }

    std::vector<InstallTarget> targets = resolveTargets(context, options.packages);
    if (g_interrupted) {
        return 130;
    }

    if (targets.empty()) {
        std::cout << YELLOW << "No packages selected.\n" << RESET;
        return 0;
    }

    FileLock lock;
    if (!lock.acquire()) {
        return 1;
    }

    std::cout << "\n" << RED << "Auto mode: " << context.packageManager
              << " / Flatpak / Snap per package\n" << RESET;
    std::cout << "Installing packages...\n\n";

    return runInstallLoop(context, targets, false);
}
