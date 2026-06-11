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

std::string parseFirstAptSearchResult(const std::string& output) {
    for (const std::string& rawLine : splitLines(output)) {
        std::istringstream ss(rawLine);
        std::string package;
        if (ss >> package) {
            return package;
        }
    }
    return {};
}

std::string parseFirstZypperSearchResult(const std::string& output) {
    for (const std::string& rawLine : splitLines(output)) {
        const std::string line = trim(rawLine);
        if (line.empty() || line.find("-+-") != std::string::npos) {
            continue;
        }

        const std::vector<std::string> columns = split(line, '|');
        if (columns.size() < 2) {
            continue;
        }

        const std::string status = trim(columns[0]);
        const std::string name = trim(columns[1]);
        if (!name.empty() && status != "S") {
            return name;
        }
    }

    return {};
}

ResolveResult resolveNative(const AppContext& context, const std::string& package) {
    if (context.packageManager == "apt") {
        if (runQuiet({"apt-cache", "show", package})) {
            return {package, true};
        }

        const std::string pattern = "^" + regexEscape(package);
        const std::string found = parseFirstAptSearchResult(
            capture({"apt-cache", "search", "--names-only", pattern}).output
        );

        return found.empty() ? ResolveResult{package, false} : ResolveResult{found, true};
    }

    if (context.packageManager == "zypper") {
        if (runQuiet({"zypper", "--no-refresh", "info", package})) {
            return {package, true};
        }

        const std::string found = parseFirstZypperSearchResult(
            capture({"zypper", "--no-refresh", "search", "-x", package}).output
        );

        return found.empty() ? ResolveResult{package, false} : ResolveResult{found, true};
    }

    const bool exists = runQuiet({context.dnfCommand, "info", package});
    return {package, exists};
}

void printHelp(const char* progName) {
    std::cout << RED << "Usage: " << RESET << progName << " [options] [packages...]"
              << " or zpm inst/install [options] [packages...]\n"
              << RED << "Options:\n" << RESET
              << "  (auto)         Picks native PM / Flatpak / Snap per package\n"
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
        } else if (!isValidPackageArgument(arg)) {
            errors.push_back("Invalid package name: " + arg);
        } else {
            options.packages.push_back(arg);
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
                             bool nativeAvailable,
                             const std::string& resolvedNativeName,
                             bool snapAvailable,
                             const std::vector<std::string>& flatpakResults) {
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
        std::cout << GREEN << "exist (" << resolvedNativeName << ")" << RESET << "\n";
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
                                                    native.exists,
                                                    native.name,
                                                    snapAvailable,
                                                    flatpakResults);

        if (source == "native") {
            targets.push_back({native.name, InstallSource::Native});
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

bool ensureNativeConsistency(AppContext& context,
                             float startPct,
                             float endPct,
                             const std::string& label) {
    if (context.nativeConsistencyChecked) {
        return true;
    }

    const float checkStartPct = progressBetween(startPct, endPct, 0.05f);
    const float checkEndPct = progressBetween(startPct, endPct, 0.20f);

    int exitCode = 0;
    if (context.packageManager == "apt") {
        exitCode = runLoggedWithProgress({"dpkg", "--configure", "-a"},
                                         "-----checking_system_consistency-----",
                                         checkStartPct,
                                         checkEndPct,
                                         label,
                                         "checking system",
                                         {{"DEBIAN_FRONTEND", "noninteractive"}});
    } else if (context.packageManager == "zypper" || context.packageManager == "dnf") {
        exitCode = runLoggedWithProgress({"rpm", "--rebuilddb"},
                                         "-----checking_system_consistency-----",
                                         checkStartPct,
                                         checkEndPct,
                                         label,
                                         "checking system");
    }

    if (g_interrupted) {
        return false;
    }

    if (exitCode != 0) {
        return false;
    }

    context.nativeConsistencyChecked = true;
    progressbar_update(checkEndPct, label + " - system ready");
    return true;
}

InstallStatus installNative(AppContext& context,
                            const std::string& package,
                            float startPct,
                            float endPct,
                            int index,
                            int total) {
    const std::string label = std::to_string(index) + "/" + std::to_string(total) +
                              " | " + nativeShortLabel(context) + ": " + package;

    progressbar_start(startPct, label + " - preparing...");

    if (!ensureNativeConsistency(context, startPct, endPct, label)) {
        if (g_interrupted) {
            return InstallStatus::Interrupted;
        }
        progressbar_update(endPct, label + " - failed");
        return InstallStatus::Failed;
    }

    if (context.packageManager == "apt" && !context.aptCacheRefreshed) {
        const float cacheStartPct = progressBetween(startPct, endPct, 0.22f);
        const float cacheEndPct = progressBetween(startPct, endPct, 0.40f);
        const int updateExit = runLoggedWithProgress({"apt-get", "update", "-qq"},
                                                     "-----apt_update-----",
                                                     cacheStartPct,
                                                     cacheEndPct,
                                                     label,
                                                     "refreshing cache");
        if (g_interrupted) {
            return InstallStatus::Interrupted;
        }
        if (updateExit != 0) {
            progressbar_update(endPct, label + " - failed");
            return InstallStatus::Failed;
        }
        progressbar_update(cacheEndPct, label + " - cache ready");
        context.aptCacheRefreshed = true;
    } else {
        progressbar_update(progressBetween(startPct, endPct, 0.30f),
                           label + " - package cache ready");
    }

    const float installStartPct = progressBetween(startPct, endPct, 0.45f);
    const float installEndPct = progressBetween(startPct, endPct, 0.92f);

    int exitCode = 1;
    if (context.packageManager == "apt") {
        exitCode = runLoggedWithProgress({"apt-get", "install", "-y", package},
                                         "-----apt_install_" + package + "-----",
                                         installStartPct,
                                         installEndPct,
                                         label,
                                         "installing",
                                         {{"DEBIAN_FRONTEND", "noninteractive"}});
    } else if (context.packageManager == "zypper") {
        exitCode = runLoggedWithProgress({"zypper", "--non-interactive", "install", "-y", package},
                                         "-----zypper_install_" + package + "-----",
                                         installStartPct,
                                         installEndPct,
                                         label,
                                         "installing");
    } else if (context.packageManager == "dnf") {
        exitCode = runLoggedWithProgress({context.dnfCommand, "install", "-y", package},
                                         "-----" + context.dnfCommand + "_install_" + package + "-----",
                                         installStartPct,
                                         installEndPct,
                                         label,
                                         "installing");
    }

    if (g_interrupted) {
        return InstallStatus::Interrupted;
    }

    progressbar_update(progressBetween(startPct, endPct, 0.96f), label + " - finalizing...");
    progressbar_update(endPct, label + (exitCode == 0 ? " - done" : " - failed"));
    return exitCode == 0 ? InstallStatus::Installed : InstallStatus::Failed;
}

InstallStatus installFlatpak(const AppContext& context,
                             const std::string& package,
                             float startPct,
                             float endPct,
                             int index,
                             int total) {
    const std::string label = std::to_string(index) + "/" + std::to_string(total) +
                              " | Flatpak: " + package;

    progressbar_start(startPct, label + " - preparing...");

    std::vector<std::string> args = {"flatpak", "install"};
    if (!context.flatpakFlag.empty()) {
        args.push_back(context.flatpakFlag);
    }
    args.push_back("-y");
    args.push_back("--noninteractive");
    args.push_back("flathub");
    args.push_back(package);

    const int exitCode = runLoggedWithProgress(args,
                                               "-----flatpak_install_" + package + "-----",
                                               progressBetween(startPct, endPct, 0.15f),
                                               progressBetween(startPct, endPct, 0.92f),
                                               label,
                                               "installing");
    if (g_interrupted) {
        return InstallStatus::Interrupted;
    }

    progressbar_update(progressBetween(startPct, endPct, 0.96f), label + " - finalizing...");
    progressbar_update(endPct, label + (exitCode == 0 ? " - done" : " - failed"));
    return exitCode == 0 ? InstallStatus::Installed : InstallStatus::Failed;
}

InstallStatus installSnap(const std::string& package,
                          float startPct,
                          float endPct,
                          int index,
                          int total) {
    const std::string label = std::to_string(index) + "/" + std::to_string(total) +
                              " | Snap: " + package;

    progressbar_start(startPct, label + " - preparing...");

    const int exitCode = runLoggedWithProgress({"snap", "install", package},
                                               "-----snap_install_" + package + "-----",
                                               progressBetween(startPct, endPct, 0.15f),
                                               progressBetween(startPct, endPct, 0.92f),
                                               label,
                                               "installing");
    if (g_interrupted) {
        return InstallStatus::Interrupted;
    }

    progressbar_update(progressBetween(startPct, endPct, 0.96f), label + " - finalizing...");
    progressbar_update(endPct, label + (exitCode == 0 ? " - done" : " - failed"));
    return exitCode == 0 ? InstallStatus::Installed : InstallStatus::Failed;
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

    if (dryRun) {
        progressbar_start(startPct, label + " - demo start");
        std::this_thread::sleep_for(kDryRunStepDelay);
        progressbar_update(progressBetween(startPct, endPct, 0.25f), label + " - checking plan");
        std::this_thread::sleep_for(kDryRunStepDelay);
        progressbar_update(progressBetween(startPct, endPct, 0.50f), label + " - selecting source");
        std::this_thread::sleep_for(kDryRunStepDelay);
        progressbar_update(progressBetween(startPct, endPct, 0.75f), label + " - simulating install");
        std::this_thread::sleep_for(kDryRunStepDelay);
        progressbar_update(endPct, label + " - would install");
        return InstallStatus::WouldInstall;
    }

    if (targetAlreadyInstalled(context, target)) {
        progressbar_start(startPct, label + " - already installed");
        progressbar_update(progressBetween(startPct, endPct, 0.50f),
                           label + " - already installed");
        progressbar_update(endPct, label + " - already installed");
        return InstallStatus::AlreadyInstalled;
    }

    switch (target.source) {
        case InstallSource::Native:
            return installNative(context, target.name, startPct, endPct, index, total);
        case InstallSource::Flatpak:
            return installFlatpak(context, target.name, startPct, endPct, index, total);
        case InstallSource::Snap:
            return installSnap(target.name, startPct, endPct, index, total);
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
    progressbar_start(0.0f, "0/" + std::to_string(total) +
                             (dryRun ? " | starting dry run..." : " | starting..."));

    for (int i = 0; i < total; ++i) {
        if (g_interrupted) {
            progressbar_finish("Cancelled!");
            std::cout << "\n" << YELLOW << "Cancelled.\n" << RESET;
            return 130;
        }

        const InstallTarget& target = targets[static_cast<size_t>(i)];
        const float startPct = (100.0f * static_cast<float>(i)) / static_cast<float>(total);
        const float endPct = (100.0f * static_cast<float>(i + 1)) / static_cast<float>(total);
        const std::string progressText = std::to_string(i + 1) + "/" +
                                         std::to_string(total) + " | " +
                                         sourceName(context, target.source) + ": " +
                                         target.name;

        PackageResult result;
        result.name = target.name;

        beginInstallStep(startPct,
                         progressText,
                         infoStep,
                         installInfoText(context, target, dryRun));

        const InstallStatus status = installTarget(context,
                                                   target,
                                                   dryRun,
                                                   startPct,
                                                   endPct,
                                                   i + 1,
                                                   total);

        if (status == InstallStatus::Interrupted) {
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

        results.push_back(result);
    }

    if (anyFailed) {
        progressbar_finish("Done with errors!");
        std::cout << "\n";
        for (const PackageResult& result : results) {
            std::cout << result.message << "\n";
        }
        std::cout << RED << "Installation finished with errors!\n" << RESET;
        std::cout << YELLOW << "[RAPORT] " << RESET << kLogPath << "\n";
        return 1;
    }

    progressbar_finish(dryRun ? "Dry run done!" : "Done!");
    std::cout << "\n";
    for (const PackageResult& result : results) {
        std::cout << result.message << "\n";
    }
    std::cout << GREEN << (dryRun ? "Dry run complete!\n" : "Installation complete!\n") << RESET;
    if (!dryRun) {
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

        std::cout << "\n" << RED << "Dry run demo: " << RESET
                  << "no packages will be installed or checked.\n\n";
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
