#include "main.h"

#include <cerrno>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <grp.h>
#include <limits>
#include <pwd.h>
#include <sys/file.h>
#include <sys/wait.h>

namespace {

constexpr const char* kLogPath = "/tmp/zrm.log";

volatile std::sig_atomic_t g_interrupted = 0;

enum class LogMode {
    Truncate,
    Append
};

enum class RemoveSource {
    Native,
    Flatpak,
    Snap
};

enum class RemoveStatus {
    Removed,
    WouldRemove,
    Failed,
    Interrupted
};

struct Options {
    bool showHelp = false;
    bool showVersion = false;
    bool purge = false;
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

struct UserIdentity {
    bool valid = false;
    uid_t uid = 0;
    gid_t gid = 0;
    std::string name;
    std::string home;
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
    UserIdentity runAsUser;
};

struct FlatpakPackage {
    std::string name;
    std::string flag;
};

struct AppContext {
    std::string packageManager = "apt";
    std::string dnfCommand = "dnf";
    bool hasFlatpak = false;
    bool hasSnap = false;
    bool nativeConsistencyChecked = false;
    UserIdentity invokingUser;
    std::vector<std::string> installedNativePackages;
    std::vector<FlatpakPackage> installedFlatpaks;
    std::vector<std::string> installedSnaps;
};

struct PackageResult {
    std::string name;
    std::string message;
    bool success = false;
};

struct RemoveTarget {
    std::string name;
    RemoveSource source = RemoveSource::Native;
    bool purge = false;
    std::string flatpakFlag;
};

struct RemoveProgress {
    float startPct = 0.0f;
    float endPct = 100.0f;
    int totalSteps = 1;
    std::string label;
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
        const char* paths[] = {"/run/zrm.lock", "/tmp/zrm.lock"};
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
            std::cerr << RED << "Error: Another zrm instance is already running.\n" << RESET;
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

void beginRemoveStep(float progress,
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
           value[0] != '-' &&
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

bool parseInteger(const std::string& input, int& value) {
    const std::string cleaned = trim(input);
    if (cleaned.empty()) {
        return false;
    }

    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(cleaned.c_str(), &end, 10);
    if (errno != 0 || end == cleaned.c_str() || *end != '\0') {
        return false;
    }

    if (parsed < std::numeric_limits<int>::min() ||
        parsed > std::numeric_limits<int>::max()) {
        return false;
    }

    value = static_cast<int>(parsed);
    return true;
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

        if (config.runAsUser.valid) {
            if (!config.runAsUser.home.empty()) {
                setenv("HOME", config.runAsUser.home.c_str(), 1);
            }
            if (!config.runAsUser.name.empty()) {
                setenv("USER", config.runAsUser.name.c_str(), 1);
                setenv("LOGNAME", config.runAsUser.name.c_str(), 1);
            }

            const std::string runtimeDir =
                "/run/user/" + std::to_string(static_cast<unsigned long>(config.runAsUser.uid));
            setenv("XDG_RUNTIME_DIR", runtimeDir.c_str(), 1);

            for (const auto& [key, value] : config.environment) {
                setenv(key.c_str(), value.c_str(), 1);
            }

            if (!config.runAsUser.name.empty() &&
                initgroups(config.runAsUser.name.c_str(), config.runAsUser.gid) != 0) {
                _exit(127);
            }
            if (setgid(config.runAsUser.gid) != 0 || setuid(config.runAsUser.uid) != 0) {
                _exit(127);
            }
        } else {
            for (const auto& [key, value] : config.environment) {
                setenv(key.c_str(), value.c_str(), 1);
            }
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

ProcessResult capture(const std::vector<std::string>& args,
                      const UserIdentity* runAsUser = nullptr) {
    ProcessConfig config;
    config.captureStdout = true;
    if (runAsUser != nullptr && runAsUser->valid) {
        config.runAsUser = *runAsUser;
    }
    return runProcess(args, config);
}

bool runQuiet(const std::vector<std::string>& args,
              const UserIdentity* runAsUser = nullptr) {
    ProcessConfig config;
    if (runAsUser != nullptr && runAsUser->valid) {
        config.runAsUser = *runAsUser;
    }
    return runProcess(args, config).exitCode == 0;
}

int runLogged(const std::vector<std::string>& args,
              const std::string& header,
              const std::vector<std::pair<std::string, std::string>>& environment = {},
              const UserIdentity* runAsUser = nullptr) {
    ProcessConfig config;
    config.logStdout = true;
    config.logStderr = true;
    config.header = header;
    config.environment = environment;
    if (runAsUser != nullptr && runAsUser->valid) {
        config.runAsUser = *runAsUser;
    }
    return runProcess(args, config).exitCode;
}

float progressBetween(float startPct, float endPct, float fraction) {
    const float safeFraction = std::clamp(fraction, 0.0f, 1.0f);
    return startPct + ((endPct - startPct) * safeFraction);
}

int removeStepCount(RemoveSource source) {
    switch (source) {
        case RemoveSource::Native:
            return 7;
        case RemoveSource::Flatpak:
        case RemoveSource::Snap:
            return 6;
    }

    return 6;
}

float stepFraction(const RemoveProgress& progress, int step) {
    const int safeTotal = std::max(progress.totalSteps, 1);
    const int safeStep = std::clamp(step, 0, safeTotal);
    return static_cast<float>(safeStep) / static_cast<float>(safeTotal);
}

std::string removeStepLabel(const RemoveProgress& progress,
                            int step,
                            const std::string& activity) {
    const int safeTotal = std::max(progress.totalSteps, 1);
    const int safeStep = std::clamp(step, 0, safeTotal);
    return std::to_string(safeStep) + "/" + std::to_string(safeTotal) +
           " | " + progress.label + " - " + activity;
}

void showRemoveStep(const RemoveProgress& progress,
                    int step,
                    const std::string& activity) {
    progressbar_update(progressBetween(progress.startPct,
                                       progress.endPct,
                                       stepFraction(progress, step)),
                       removeStepLabel(progress, step, activity));
}

void finishRemoveStep(const RemoveProgress& progress, const std::string& activity) {
    progressbar_update(progress.endPct,
                       removeStepLabel(progress, progress.totalSteps, activity));
}

int runLoggedStep(const std::vector<std::string>& args,
                  const std::string& header,
                  const RemoveProgress& progress,
                  int step,
                  const std::string& activity,
                  const std::vector<std::pair<std::string, std::string>>& environment = {},
                  const UserIdentity* runAsUser = nullptr) {
    showRemoveStep(progress, step, activity + "...");
    return runLogged(args, header, environment, runAsUser);
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

bool parseUid(const char* text, uid_t& uid) {
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

    if (parsed > static_cast<unsigned long>(std::numeric_limits<uid_t>::max())) {
        return false;
    }

    uid = static_cast<uid_t>(parsed);
    return true;
}

UserIdentity getInvokingUser() {
    uid_t uid = 0;
    if (!parseUid(getenv("SUDO_UID"), uid) || uid == 0) {
        return {};
    }

    passwd* entry = getpwuid(uid);
    if (entry == nullptr) {
        return {};
    }

    UserIdentity user;
    user.valid = true;
    user.uid = entry->pw_uid;
    user.gid = entry->pw_gid;
    user.name = entry->pw_name != nullptr ? entry->pw_name : "";
    user.home = entry->pw_dir != nullptr ? entry->pw_dir : "";
    return user;
}

std::string nativeLabel(const AppContext& context) {
    if (context.packageManager == "zypper") {
        return "Zypper: ";
    }
    if (context.packageManager == "dnf") {
        return context.dnfCommand == "dnf5" ? "DNF5:   " : "DNF:    ";
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
    return "APT";
}

std::string flatpakScopeName(const std::string& flag) {
    if (flag == "--system") {
        return "system";
    }
    if (flag == "--user") {
        return "user";
    }
    return "default";
}

const UserIdentity* userForFlatpakFlag(const AppContext& context, const std::string& flag) {
    if (flag == "--user" && context.invokingUser.valid) {
        return &context.invokingUser;
    }
    return nullptr;
}

bool sameFlatpakPackage(const FlatpakPackage& lhs, const FlatpakPackage& rhs) {
    return lhs.name == rhs.name && lhs.flag == rhs.flag;
}

void addUniqueFlatpak(std::vector<FlatpakPackage>& packages, const FlatpakPackage& package) {
    if (package.name.empty()) {
        return;
    }

    const auto found = std::find_if(packages.begin(), packages.end(),
                                    [&](const FlatpakPackage& current) {
                                        return sameFlatpakPackage(current, package);
                                    });
    if (found == packages.end()) {
        packages.push_back(package);
    }
}

std::vector<std::string> parseFlatpakApplications(const std::string& output) {
    std::vector<std::string> apps;

    for (const std::string& rawLine : splitLines(output)) {
        const std::string line = trim(rawLine);
        if (line.empty() || line == "Application") {
            continue;
        }

        if (std::find(apps.begin(), apps.end(), line) == apps.end()) {
            apps.push_back(line);
        }
    }

    return apps;
}

void addUniqueString(std::vector<std::string>& values, const std::string& value) {
    const std::string cleaned = trim(value);
    if (cleaned.empty()) {
        return;
    }

    if (std::find(values.begin(), values.end(), cleaned) == values.end()) {
        values.push_back(cleaned);
    }
}

bool startsWithInsensitive(const std::string& value, const std::string& prefix) {
    const std::string lowerValue = toLower(value);
    const std::string lowerPrefix = toLower(prefix);
    return !lowerPrefix.empty() && lowerValue.rfind(lowerPrefix, 0) == 0;
}

std::vector<std::string> prefixMatches(const std::vector<std::string>& installed,
                                       const std::string& query,
                                       bool exactInstalled) {
    std::vector<std::string> matches;
    if (exactInstalled) {
        addUniqueString(matches, query);
    }

    for (const std::string& package : installed) {
        if (startsWithInsensitive(package, query)) {
            addUniqueString(matches, package);
        }
    }

    std::vector<std::string> sorted;
    for (const std::string& package : matches) {
        if (toLower(package) != toLower(query)) {
            sorted.push_back(package);
        }
    }
    std::sort(sorted.begin(), sorted.end());

    std::vector<std::string> ordered;
    for (const std::string& package : matches) {
        if (toLower(package) == toLower(query)) {
            addUniqueString(ordered, package);
        }
    }
    for (const std::string& package : sorted) {
        addUniqueString(ordered, package);
    }

    return ordered;
}

std::vector<FlatpakPackage> listInstalledFlatpaks(const std::string& flag,
                                                  const UserIdentity* runAsUser) {
    std::vector<std::string> args = {"flatpak", "list"};
    if (!flag.empty()) {
        args.push_back(flag);
    }
    args.push_back("--columns=application");

    ProcessResult result = capture(args, runAsUser);
    if (result.exitCode != 0) {
        return {};
    }

    std::vector<FlatpakPackage> packages;
    for (const std::string& app : parseFlatpakApplications(result.output)) {
        addUniqueFlatpak(packages, {app, flag});
    }

    return packages;
}

std::vector<FlatpakPackage> findFlatpakMatches(const AppContext& context,
                                               const std::string& package) {
    std::vector<FlatpakPackage> matches;
    for (const FlatpakPackage& app : context.installedFlatpaks) {
        if (toLower(app.name) == toLower(package)) {
            addUniqueFlatpak(matches, app);
        }
    }

    for (const FlatpakPackage& app : context.installedFlatpaks) {
        if (startsWithInsensitive(app.name, package)) {
            addUniqueFlatpak(matches, app);
        }
    }

    std::stable_sort(matches.begin(), matches.end(), [&](const FlatpakPackage& lhs,
                                                         const FlatpakPackage& rhs) {
        const bool lhsExact = toLower(lhs.name) == toLower(package);
        const bool rhsExact = toLower(rhs.name) == toLower(package);
        if (lhsExact != rhsExact) {
            return lhsExact;
        }
        if (lhs.name != rhs.name) {
            return lhs.name < rhs.name;
        }
        return lhs.flag < rhs.flag;
    });

    return matches;
}

bool isInstalledNative(const AppContext& context, const std::string& package) {
    if (context.packageManager == "apt") {
        const ProcessResult result = capture({"dpkg-query", "-W", "-f=${Status}", package});
        return result.exitCode == 0 &&
               result.output.find("install ok installed") != std::string::npos;
    }

    return runQuiet({"rpm", "-q", package});
}

bool isInstalledFlatpak(const AppContext& context, const FlatpakPackage& package) {
    if (package.name.empty()) {
        return false;
    }

    std::vector<std::string> args = {"flatpak", "info"};
    if (!package.flag.empty()) {
        args.push_back(package.flag);
    }
    args.push_back(package.name);

    return runQuiet(args, userForFlatpakFlag(context, package.flag));
}

bool isInstalledSnap(const AppContext& context, const std::string& package) {
    return context.hasSnap && runQuiet({"snap", "list", package});
}

std::vector<std::string> listInstalledNativePackages(const AppContext& context) {
    ProcessResult result;
    if (context.packageManager == "apt") {
        result = capture({"dpkg-query", "-W", "-f=${binary:Package}\\n"});
    } else if (context.packageManager == "zypper" || context.packageManager == "dnf") {
        result = capture({"rpm", "-qa", "--qf", "%{NAME}\\n"});
    } else {
        return {};
    }

    if (result.exitCode != 0) {
        return {};
    }

    std::vector<std::string> packages;
    for (const std::string& line : splitLines(result.output)) {
        addUniqueString(packages, line);
    }

    std::sort(packages.begin(), packages.end());
    return packages;
}

std::vector<std::string> listInstalledSnaps() {
    const ProcessResult result = capture({"snap", "list"});
    if (result.exitCode != 0) {
        return {};
    }

    std::vector<std::string> snaps;
    for (const std::string& rawLine : splitLines(result.output)) {
        const std::string line = trim(rawLine);
        const std::string lower = toLower(line);
        if (line.empty() || startsWith(lower, "name ")) {
            continue;
        }

        std::istringstream stream(line);
        std::string name;
        if (stream >> name) {
            addUniqueString(snaps, name);
        }
    }

    std::sort(snaps.begin(), snaps.end());
    return snaps;
}

std::vector<std::string> findNativeMatches(const AppContext& context,
                                           const std::string& package) {
    return prefixMatches(context.installedNativePackages,
                         package,
                         isInstalledNative(context, package));
}

std::vector<std::string> findSnapMatches(const AppContext& context,
                                         const std::string& package) {
    if (!context.hasSnap) {
        return {};
    }

    return prefixMatches(context.installedSnaps,
                         package,
                         isInstalledSnap(context, package));
}

void printHelp(const char* progName) {
    std::cout << RED << "Usage: " << RESET << progName << " [options] [packages...]"
              << " or zpm rm/remove [options] [packages...]\n"
              << RED << "Options:\n" << RESET
              << "  (auto)         Picks native PM / Flatpak / Snap per package\n"
              << "  --purge, -p    APT purge instead of remove (APT only)\n"
              << "  --dry-run      Simulate remove flow; fake packages are allowed\n"
              << "  --version, -v  Show version information\n"
              << "  --help,    -h  Show this help message\n";
}

void printVersion() {
    std::cout << RED << "zrm component version: v" << zpm_version::version()
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
        } else if (arg == "--purge" || arg == "-p") {
            options.purge = true;
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
        (options.purge || options.dryRun || !options.packages.empty())) {
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

std::string chooseRemoveMenu(const AppContext& context,
                             const std::string& package,
                             const std::vector<std::string>& nativeMatches,
                             const std::vector<std::string>& snapMatches,
                             const std::vector<FlatpakPackage>& flatpakMatches) {
    struct Option {
        int number = 0;
        std::string key;
    };

    std::vector<Option> options;
    int index = 1;

    std::cout << "\n" << BOLD << "Package: " << CYAN << package << RESET << "\n";

    std::cout << "  " << BOLD << index << ". " << nativeLabel(context) << RESET;
    if (!nativeMatches.empty()) {
        if (nativeMatches.size() == 1) {
            std::cout << GREEN << "installed (" << nativeMatches.front() << ")" << RESET << "\n";
        } else {
            std::cout << GREEN << "installed (" << nativeMatches.size()
                      << " results)" << RESET << "\n";
        }
        options.push_back({index, "native"});
    } else {
        std::cout << RED << "none" << RESET << "\n";
    }
    ++index;

    if (context.hasSnap) {
        std::cout << "  " << BOLD << index << ". Snap:    " << RESET;
        if (!snapMatches.empty()) {
            if (snapMatches.size() == 1) {
                std::cout << GREEN << "installed (" << snapMatches.front() << ")" << RESET << "\n";
            } else {
                std::cout << GREEN << "installed (" << snapMatches.size()
                          << " results)" << RESET << "\n";
            }
            options.push_back({index, "snap"});
        } else {
            std::cout << RED << "none" << RESET << "\n";
        }
        ++index;
    }

    if (context.hasFlatpak) {
        std::cout << "  " << BOLD << index << ". Flatpak: " << RESET;
        if (!flatpakMatches.empty()) {
            if (flatpakMatches.size() == 1) {
                std::cout << GREEN << "installed (" << flatpakMatches.front().name
                          << " [" << flatpakScopeName(flatpakMatches.front().flag)
                          << "])" << RESET << "\n";
            } else {
                std::cout << GREEN << "installed (" << flatpakMatches.size()
                          << " results)" << RESET << "\n";
            }
            options.push_back({index, "flatpak"});
        } else {
            std::cout << RED << "none" << RESET << "\n";
        }
        ++index;
    }

    std::cout << "  " << BOLD << "0. Skip" << RESET << "\n";

    if (options.empty()) {
        std::cout << YELLOW << "Package '" << package << "' is not installed anywhere.\n" << RESET;
        return {};
    }

    for (;;) {
        std::cout << BOLD << "Choose: " << RESET;

        std::string input;
        if (!readChoice(input)) {
            return {};
        }

        int choice = -1;
        if (!parseInteger(input, choice)) {
            std::cout << RED << "Invalid choice, try again.\n" << RESET;
            continue;
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

std::vector<std::string> choosePackagesToRemove(const std::vector<std::string>& installed,
                                                const std::string& query,
                                                const std::string& sourceName) {
    if (installed.empty()) {
        std::cout << YELLOW << "No installed " << sourceName << " packages";
        if (!query.empty()) {
            std::cout << " starting with '" << query << "'";
        }
        std::cout << ".\n" << RESET;
        return {};
    }

    std::cout << GREEN << "\nInstalled " << sourceName << " packages";
    if (!query.empty()) {
        std::cout << " starting with '" << query << "'";
    }
    std::cout << ":\n" << RESET;

    for (size_t i = 0; i < installed.size(); ++i) {
        std::cout << "  " << (i + 1) << ". " << installed[i] << "\n";
    }
    std::cout << "  0. Cancel\n";

    for (;;) {
        std::cout << BOLD << "Enter number(s) to remove (e.g. 1 3): " << RESET;

        std::string input;
        if (!readChoice(input)) {
            return {};
        }

        std::replace(input.begin(), input.end(), ',', ' ');
        std::stringstream ss(input);
        std::string token;
        std::vector<std::string> selected;
        bool invalid = false;

        while (ss >> token) {
            int choice = -1;
            if (!parseInteger(token, choice)) {
                invalid = true;
                break;
            }

            if (choice == 0) {
                return {};
            }

            if (choice < 1 || choice > static_cast<int>(installed.size())) {
                invalid = true;
                break;
            }

            addUniqueString(selected, installed[static_cast<size_t>(choice - 1)]);
        }

        if (!invalid && !selected.empty()) {
            return selected;
        }

        std::cout << RED << "Invalid choice, try again.\n" << RESET;
    }
}

std::vector<FlatpakPackage> chooseFlatpakToRemove(const std::vector<FlatpakPackage>& installed,
                                                  const std::string& query) {
    if (installed.empty()) {
        std::cout << YELLOW << "No installed Flatpak packages";
        if (!query.empty()) {
            std::cout << " starting with '" << query << "'";
        }
        std::cout << ".\n" << RESET;
        return {};
    }

    std::cout << GREEN << "\nInstalled Flatpak packages";
    if (!query.empty()) {
        std::cout << " starting with '" << query << "'";
    }
    std::cout << ":\n" << RESET;

    for (size_t i = 0; i < installed.size(); ++i) {
        std::cout << "  " << (i + 1) << ". " << installed[i].name
                  << " [" << flatpakScopeName(installed[i].flag) << "]\n";
    }
    std::cout << "  0. Cancel\n";

    for (;;) {
        std::cout << BOLD << "Enter number(s) to remove (e.g. 1 3): " << RESET;

        std::string input;
        if (!readChoice(input)) {
            return {};
        }

        std::replace(input.begin(), input.end(), ',', ' ');
        std::stringstream ss(input);
        std::string token;
        std::vector<FlatpakPackage> selected;
        bool invalid = false;

        while (ss >> token) {
            int choice = -1;
            if (!parseInteger(token, choice)) {
                invalid = true;
                break;
            }

            if (choice == 0) {
                return {};
            }

            if (choice < 1 || choice > static_cast<int>(installed.size())) {
                invalid = true;
                break;
            }

            addUniqueFlatpak(selected, installed[static_cast<size_t>(choice - 1)]);
        }

        if (!invalid && !selected.empty()) {
            return selected;
        }

        std::cout << RED << "Invalid choice, try again.\n" << RESET;
    }
}

std::vector<RemoveTarget> resolveRemoveTargets(const AppContext& context,
                                               const std::vector<std::string>& packages,
                                               bool purge) {
    std::vector<RemoveTarget> targets;

    for (const std::string& package : packages) {
        if (g_interrupted) {
            break;
        }

        const std::vector<std::string> nativeMatches = findNativeMatches(context, package);
        const std::vector<std::string> snapMatches = findSnapMatches(context, package);
        const std::vector<FlatpakPackage> flatpakMatches =
            context.hasFlatpak ? findFlatpakMatches(context, package)
                               : std::vector<FlatpakPackage>{};

        const std::string source = chooseRemoveMenu(context,
                                                    package,
                                                    nativeMatches,
                                                    snapMatches,
                                                    flatpakMatches);

        if (source == "native") {
            const std::vector<std::string> selected = nativeMatches.size() == 1
                ? nativeMatches
                : choosePackagesToRemove(nativeMatches, package, nativeShortLabel(context));

            for (const std::string& name : selected) {
                targets.push_back({name, RemoveSource::Native, purge, {}});
            }
        } else if (source == "snap") {
            const std::vector<std::string> selected = snapMatches.size() == 1
                ? snapMatches
                : choosePackagesToRemove(snapMatches, package, "Snap");

            for (const std::string& name : selected) {
                targets.push_back({name, RemoveSource::Snap, false, {}});
            }
        } else if (source == "flatpak") {
            const std::vector<FlatpakPackage> selected = flatpakMatches.size() == 1
                ? flatpakMatches
                : chooseFlatpakToRemove(flatpakMatches, package);

            for (const FlatpakPackage& app : selected) {
                targets.push_back({app.name, RemoveSource::Flatpak, false, app.flag});
            }
        }
    }

    return targets;
}

bool ensureNativeConsistency(AppContext& context, const RemoveProgress& progress) {
    if (context.nativeConsistencyChecked) {
        showRemoveStep(progress, 3, "system already checked");
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
    showRemoveStep(progress, 3, "system ready");
    return true;
}

RemoveStatus removeNative(AppContext& context,
                          const std::string& package,
                          bool purge,
                          const RemoveProgress& progress) {
    const std::string operation = (context.packageManager == "apt" && purge) ? "purge" : "remove";

    if (!ensureNativeConsistency(context, progress)) {
        if (g_interrupted) {
            return RemoveStatus::Interrupted;
        }
        finishRemoveStep(progress, "failed");
        return RemoveStatus::Failed;
    }

    showRemoveStep(progress, 4, "preparing " + operation + " transaction");

    int exitCode = 1;
    if (context.packageManager == "apt") {
        exitCode = runLoggedStep({"apt-get", "-y", operation, package},
                                 "-----apt_" + operation + "_" + package + "-----",
                                 progress,
                                 5,
                                 operation + " package",
                                 {{"DEBIAN_FRONTEND", "noninteractive"}});
    } else if (context.packageManager == "zypper") {
        exitCode = runLoggedStep({"zypper", "--non-interactive", "remove", "-y", package},
                                 "-----zypper_remove_" + package + "-----",
                                 progress,
                                 5,
                                 "removing package");
    } else if (context.packageManager == "dnf") {
        exitCode = runLoggedStep({context.dnfCommand, "remove", "-y", package},
                                 "-----" + context.dnfCommand + "_remove_" + package + "-----",
                                 progress,
                                 5,
                                 "removing package");
    }

    if (g_interrupted) {
        return RemoveStatus::Interrupted;
    }

    if (exitCode != 0) {
        finishRemoveStep(progress, "failed");
        return RemoveStatus::Failed;
    }

    showRemoveStep(progress, 6, "verifying removal");
    if (isInstalledNative(context, package)) {
        finishRemoveStep(progress, "verification failed");
        return RemoveStatus::Failed;
    }

    showRemoveStep(progress, 7, "finalizing");
    finishRemoveStep(progress, "done");
    return RemoveStatus::Removed;
}

RemoveStatus removeFlatpak(const AppContext& context,
                           const RemoveTarget& target,
                           const RemoveProgress& progress) {
    showRemoveStep(progress, 3, "checking flatpak scope");
    showRemoveStep(progress, 4, "preparing removal transaction");

    std::vector<std::string> args = {"flatpak", "uninstall"};
    if (!target.flatpakFlag.empty()) {
        args.push_back(target.flatpakFlag);
    }
    args.push_back("-y");
    args.push_back("--delete-data");
    args.push_back(target.name);

    const int exitCode = runLoggedStep(args,
                                       "-----flatpak_remove_" + target.name + "-----",
                                       progress,
                                       5,
                                       "removing package",
                                       {},
                                       userForFlatpakFlag(context, target.flatpakFlag));
    if (g_interrupted) {
        return RemoveStatus::Interrupted;
    }
    if (exitCode != 0) {
        finishRemoveStep(progress, "failed");
        return RemoveStatus::Failed;
    }

    showRemoveStep(progress, 6, "verifying removal");
    const bool stillInstalled = isInstalledFlatpak(context, {target.name, target.flatpakFlag});
    const bool removed = !stillInstalled;

    if (removed) {
        finishRemoveStep(progress, "done");
        return RemoveStatus::Removed;
    }
    finishRemoveStep(progress, "verification failed");
    return RemoveStatus::Failed;
}

RemoveStatus removeSnap(const AppContext& context,
                        const std::string& package,
                        const RemoveProgress& progress) {
    showRemoveStep(progress, 3, "checking snap state");
    showRemoveStep(progress, 4, "preparing removal transaction");

    const int exitCode = runLoggedStep({"snap", "remove", package},
                                       "-----snap_remove_" + package + "-----",
                                       progress,
                                       5,
                                       "removing package");
    if (g_interrupted) {
        return RemoveStatus::Interrupted;
    }

    if (exitCode != 0) {
        finishRemoveStep(progress, "failed");
        return RemoveStatus::Failed;
    }

    showRemoveStep(progress, 6, "verifying removal");
    if (isInstalledSnap(context, package)) {
        finishRemoveStep(progress, "verification failed");
        return RemoveStatus::Failed;
    }

    finishRemoveStep(progress, "done");
    return RemoveStatus::Removed;
}

std::string removeTargetDisplay(const AppContext& context, const RemoveTarget& target) {
    switch (target.source) {
        case RemoveSource::Native: {
            const std::string operation = (context.packageManager == "apt" && target.purge)
                ? "purge"
                : "remove";
            return nativeShortLabel(context) + " " + operation + ": " + target.name;
        }
        case RemoveSource::Flatpak:
            return "Flatpak: " + target.name +
                   " [" + flatpakScopeName(target.flatpakFlag) + "]";
        case RemoveSource::Snap:
            return "Snap: " + target.name;
    }

    return target.name;
}

std::string removeTargetLabel(const AppContext& context,
                              const RemoveTarget& target,
                              int index,
                              int total) {
    return std::to_string(index) + "/" + std::to_string(total) +
           " | " + removeTargetDisplay(context, target);
}

std::string removeSourceDescription(const AppContext& context, const RemoveTarget& target) {
    switch (target.source) {
        case RemoveSource::Native:
            return nativeShortLabel(context);
        case RemoveSource::Flatpak:
            return "Flatpak [" + flatpakScopeName(target.flatpakFlag) + "]";
        case RemoveSource::Snap:
            return "Snap";
    }

    return "selected source";
}

bool targetStillInstalled(const AppContext& context, const RemoveTarget& target) {
    switch (target.source) {
        case RemoveSource::Native:
            return isInstalledNative(context, target.name);
        case RemoveSource::Flatpak:
            return isInstalledFlatpak(context, {target.name, target.flatpakFlag});
        case RemoveSource::Snap:
            return isInstalledSnap(context, target.name);
    }

    return false;
}

RemoveStatus removeTarget(AppContext& context,
                          const RemoveTarget& target,
                          bool dryRun,
                          float startPct,
                          float endPct,
                          int index,
                          int total) {
    const RemoveProgress progress {
        startPct,
        endPct,
        removeStepCount(target.source),
        removeTargetLabel(context, target, index, total)
    };

    progressbar_start(startPct, removeStepLabel(progress, 0, "preparing"));
    showRemoveStep(progress, 1, "checking selected source");

    if (dryRun) {
        showRemoveStep(progress, 2, "would check installed state");
        if (target.source == RemoveSource::Native) {
            showRemoveStep(progress, 3, "would check system consistency");
            showRemoveStep(progress, 4, "would prepare removal transaction");
            showRemoveStep(progress, 5, "would remove package");
            showRemoveStep(progress, 6, "would verify removal");
        } else {
            showRemoveStep(progress, 3, target.source == RemoveSource::Flatpak
                                       ? "would check flatpak scope"
                                       : "would check snap state");
            showRemoveStep(progress, 4, "would prepare removal transaction");
            showRemoveStep(progress, 5, "would remove package");
        }
        finishRemoveStep(progress, "would remove");
        return RemoveStatus::WouldRemove;
    }

    showRemoveStep(progress, 2, "checking installed state");
    if (!targetStillInstalled(context, target)) {
        finishRemoveStep(progress, "already removed");
        return RemoveStatus::Removed;
    }

    switch (target.source) {
        case RemoveSource::Native:
            return removeNative(context, target.name, target.purge, progress);
        case RemoveSource::Flatpak:
            return removeFlatpak(context, target, progress);
        case RemoveSource::Snap:
            return removeSnap(context, target.name, progress);
    }

    return RemoveStatus::Failed;
}

std::string successMessage(const AppContext& context, const RemoveTarget& target) {
    if (target.source == RemoveSource::Native) {
        const std::string operation = (context.packageManager == "apt" && target.purge)
            ? "purged"
            : "removed";
        return "Package " + target.name + " " + operation + " successfully.";
    }

    return "Package " + target.name + " removed successfully.";
}

std::string removeInfoText(const AppContext& context,
                           const RemoveTarget& target,
                           bool dryRun) {
    return std::string(dryRun ? "simulating removal of " : "removing ") +
           removeTargetDisplay(context, target);
}

int runRemoveLoop(AppContext& context,
                  const std::vector<RemoveTarget>& targets,
                  bool dryRun) {
    std::vector<PackageResult> results;
    results.reserve(targets.size());

    bool anyFailed = false;
    const int total = static_cast<int>(targets.size());

    if (!dryRun) {
        writeLogLine("-----zrm_start-----", LogMode::Truncate);
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

        const RemoveTarget& target = targets[static_cast<size_t>(i)];
        const float startPct = (100.0f * static_cast<float>(i)) / static_cast<float>(total);
        const float endPct = (100.0f * static_cast<float>(i + 1)) / static_cast<float>(total);
        const std::string progressText = "0/" +
                                         std::to_string(removeStepCount(target.source)) +
                                         " | " +
                                         removeTargetLabel(context, target, i + 1, total);

        PackageResult result;
        result.name = target.name;

        beginRemoveStep(startPct,
                        progressText,
                        infoStep,
                        removeInfoText(context, target, dryRun));

        const RemoveStatus status = removeTarget(context,
                                                 target,
                                                 dryRun,
                                                 startPct,
                                                 endPct,
                                                 i + 1,
                                                 total);

        if (status == RemoveStatus::Interrupted) {
            progressbar_finish("Cancelled!");
            std::cout << "\n" << YELLOW << "Cancelled.\n" << RESET;
            return 130;
        }

        if (status == RemoveStatus::Removed) {
            result.message = successMessage(context, target);
            result.success = true;
        } else if (status == RemoveStatus::WouldRemove) {
            result.message = "Package " + target.name + " would be removed from " +
                             removeSourceDescription(context, target) + ".";
            result.success = true;
        } else {
            result.message = RED + "Package " + target.name + " removal failed." + RESET;
            anyFailed = true;
        }

        results.push_back(result);
    }

    if (anyFailed) {
        progressbar_finish("Done with errors!");
        std::cout << "\n";
        for (const PackageResult& result : results) {
            std::cout << result.message << "\n";
        }
        std::cout << RED << "Removal finished with errors!\n" << RESET;
        std::cout << YELLOW << "[RAPORT] " << RESET << kLogPath << "\n";
        return 1;
    }

    progressbar_finish(dryRun ? "Dry run done!" : "Done!");
    std::cout << "\n";
    for (const PackageResult& result : results) {
        std::cout << result.message << "\n";
    }
    std::cout << GREEN << (dryRun ? "Dry run complete!\n" : "Removal complete!\n") << RESET;
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
    context.invokingUser = getInvokingUser();
    context.installedNativePackages = listInstalledNativePackages(context);

    if (context.hasFlatpak) {
        for (const char* flag : {"--system", "--user"}) {
            for (const FlatpakPackage& package :
                 listInstalledFlatpaks(flag, userForFlatpakFlag(context, flag))) {
                addUniqueFlatpak(context.installedFlatpaks, package);
            }
        }
    }

    if (context.hasSnap) {
        context.installedSnaps = listInstalledSnaps();
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
    return context;
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

std::vector<RemoveTarget> buildDryRunTargets(const AppContext& context,
                                             const std::vector<std::string>& packages,
                                             bool purge) {
    std::vector<RemoveSource> sources = {RemoveSource::Native};
    if (context.hasFlatpak) {
        sources.push_back(RemoveSource::Flatpak);
    }
    if (context.hasSnap) {
        sources.push_back(RemoveSource::Snap);
    }

    std::vector<RemoveTarget> targets;
    const std::vector<std::string> names = dryRunPackages(packages);
    targets.reserve(names.size());

    for (size_t i = 0; i < names.size(); ++i) {
        const RemoveSource source = sources[i % sources.size()];
        RemoveTarget target;
        target.name = names[i];
        target.source = source;
        target.purge = source == RemoveSource::Native &&
                       context.packageManager == "apt" &&
                       purge;
        if (source == RemoveSource::Flatpak) {
            target.flatpakFlag = "--system";
        }
        targets.push_back(target);
    }

    return targets;
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
        std::vector<RemoveTarget> targets =
            buildDryRunTargets(context, options.packages, options.purge);

        std::cout << "\n" << RED << "Dry run demo: " << RESET
                  << "no packages will be removed or checked.\n\n";
        return runRemoveLoop(context, targets, true);
    }

    if (geteuid() != 0) {
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

    if (options.purge && context.packageManager != "apt") {
        std::cout << YELLOW << "Warning: --purge is APT-only, ignored on "
                  << context.packageManager << ".\n" << RESET;
        options.purge = false;
    }

    std::vector<RemoveTarget> targets = resolveRemoveTargets(context,
                                                             options.packages,
                                                             options.purge);
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
    std::cout << "Removing packages...\n\n";

    return runRemoveLoop(context, targets, false);
}
