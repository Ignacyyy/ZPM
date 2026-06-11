#include "main.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <thread>
#include <sys/file.h>
#include <sys/wait.h>

namespace {

constexpr const char* kLogPath = "/tmp/zinst.log";
constexpr std::chrono::milliseconds kCommandProgressInterval{350};
constexpr std::chrono::milliseconds kDryRunStepDelay{120};
constexpr float kCommandProgressInitialFraction = 0.08f;
constexpr float kCommandProgressMaxFraction = 0.94f;

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

struct ResolveResult {
    std::string name;
    bool exists = false;
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

void beginInstallStep(float progress,
                      const std::string& progressText,
                      int& step,
                      const std::string& infoText) {
    std::lock_guard<std::mutex> outputLock(zpm::progressbar_detail::outputMutex());

    if (step > 0) {
        std::cout << "\r\033[K\033[1A\r\033[K";
    } else {
        std::cout << "\r\033[K";
    }

    std::cout << CYAN << "[>]" << RESET << " " << infoText << "\n\n";

    ++step;
    progressbar_update(progress, progressText);
    std::cout << std::flush;
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

void writeLogLine(const std::string& line, LogMode mode = LogMode::Append) {
    FileDescriptor fd = openLog(mode);
    if (!fd.valid()) {
        return;
    }
    writeAll(fd.get(), line + "\n");
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

class CommandProgress {
public:
    CommandProgress(float startPct, float endPct, const std::string& label, const std::string& activity)
        : startPct_(startPct),
          endPct_(endPct),
          task_(label + " - " + activity + "...") {
        if (endPct_ <= startPct_) {
            return;
        }

        running_ = true;
        try {
            worker_ = std::thread(&CommandProgress::run, this);
        } catch (...) {
            running_ = false;
        }
    }

    CommandProgress(const CommandProgress&) = delete;
    CommandProgress& operator=(const CommandProgress&) = delete;

    ~CommandProgress() {
        stop();
    }

    void stop() {
        if (!running_.exchange(false)) {
            return;
        }

        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    void run() {
        int tick = 0;
        while (running_) {
            std::this_thread::sleep_for(kCommandProgressInterval);
            if (!running_) {
                return;
            }

            ++tick;
            const float eased = kCommandProgressInitialFraction +
                                ((kCommandProgressMaxFraction - kCommandProgressInitialFraction) *
                                 (1.0f - std::exp(-static_cast<float>(tick) / 8.0f)));
            progressbar_update(progressBetween(startPct_, endPct_, eased), task_);
        }
    }

    float startPct_ = 0.0f;
    float endPct_ = 0.0f;
    std::string task_;
    std::atomic_bool running_ = false;
    std::thread worker_;
};

int runLoggedWithProgress(const std::vector<std::string>& args,
                          const std::string& header,
                          float startPct,
                          float endPct,
                          const std::string& label,
                          const std::string& activity,
                          const std::vector<std::pair<std::string, std::string>>& environment = {}) {
    progressbar_update(startPct, label + " - " + activity + "...");
    CommandProgress progress(startPct, endPct, label, activity);
    const int exitCode = runLogged(args, header, environment);
    progress.stop();
    return exitCode;
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
}