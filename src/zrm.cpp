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
    Failed,
    Interrupted
};

struct Options {
    bool showHelp = false;
    bool showVersion = false;
    bool purge = false;
    std::vector<std::string> packages;
};

struct ProcessResult {
    int exitCode = 127;
    std::string output;
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
    UserIdentity invokingUser;
    std::vector<FlatpakPackage> installedFlatpaks;
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

            struct stat st {};
            if (fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode)) {
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
                      (mode == LogMode::Truncate ? O_TRUNC : O_APPEND);

    FileDescriptor fd(open(kLogPath, flags, 0600));
    if (!fd.valid()) {
        return {};
    }

    struct stat st {};
    if (fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return {};
    }

    fchmod(fd.get(), 0600);
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
    std::vector<FlatpakPackage> exact;
    for (const FlatpakPackage& app : context.installedFlatpaks) {
        if (app.name == package) {
            addUniqueFlatpak(exact, app);
        }
    }

    if (!exact.empty()) {
        return exact;
    }

    const std::string lowerPackage = toLower(package);
    std::vector<FlatpakPackage> fuzzy;
    for (const FlatpakPackage& app : context.installedFlatpaks) {
        if (toLower(app.name).find(lowerPackage) != std::string::npos) {
            addUniqueFlatpak(fuzzy, app);
        }
    }

    std::sort(fuzzy.begin(), fuzzy.end(), [](const FlatpakPackage& lhs,
                                             const FlatpakPackage& rhs) {
        if (lhs.name != rhs.name) {
            return lhs.name < rhs.name;
        }
        return lhs.flag < rhs.flag;
    });

    return fuzzy;
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

void printHelp(const char* progName) {
    std::cout << RED << "Usage: " << RESET << progName << " [options] [packages...]"
              << " or zpm rm/remove [options] [packages...]\n"
              << RED << "Options:\n" << RESET
              << "  (auto)         Picks native PM / Flatpak / Snap per package\n"
              << "  --purge, -p    APT purge instead of remove (APT only)\n"
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
        } else if (startsWith(arg, "-")) {
            errors.push_back("Unknown option: " + arg);
        } else if (!isValidPackageArgument(arg)) {
            errors.push_back("Invalid package name: " + arg);
        } else {
            options.packages.push_back(arg);
        }
    }

    if ((options.showHelp || options.showVersion) &&
        (options.purge || !options.packages.empty())) {
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
                             bool nativeInstalled,
                             bool snapInstalled,
                             bool flatpakInstalled) {
    struct Option {
        int number = 0;
        std::string key;
    };

    std::vector<Option> options;
    int index = 1;

    std::cout << "\n" << BOLD << "Package: " << CYAN << package << RESET << "\n";

    std::cout << "  " << BOLD << index << ". " << nativeLabel(context) << RESET;
    if (nativeInstalled) {
        std::cout << GREEN << "installed" << RESET << "\n";
        options.push_back({index, "native"});
    } else {
        std::cout << RED << "none" << RESET << "\n";
    }
    ++index;

    if (context.hasSnap) {
        std::cout << "  " << BOLD << index << ". Snap:    " << RESET;
        if (snapInstalled) {
            std::cout << GREEN << "installed" << RESET << "\n";
            options.push_back({index, "snap"});
        } else {
            std::cout << RED << "none" << RESET << "\n";
        }
        ++index;
    }

    if (context.hasFlatpak) {
        std::cout << "  " << BOLD << index << ". Flatpak: " << RESET;
        if (flatpakInstalled) {
            std::cout << GREEN << "installed" << RESET << "\n";
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

std::vector<FlatpakPackage> chooseFlatpakToRemove(const std::vector<FlatpakPackage>& installed,
                                                  const std::string& query) {
    if (installed.empty()) {
        std::cout << YELLOW << "No installed Flatpak packages";
        if (!query.empty()) {
            std::cout << " matching '" << query << "'";
        }
        std::cout << ".\n" << RESET;
        return {};
    }

    std::cout << GREEN << "\nInstalled Flatpak packages";
    if (!query.empty()) {
        std::cout << " matching '" << query << "'";
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

        const bool nativeInstalled = isInstalledNative(context, package);
        const bool snapInstalled = isInstalledSnap(context, package);
        const std::vector<FlatpakPackage> flatpakMatches =
            context.hasFlatpak ? findFlatpakMatches(context, package)
                               : std::vector<FlatpakPackage>{};
        const bool flatpakInstalled = !flatpakMatches.empty();

        const std::string source = chooseRemoveMenu(context,
                                                    package,
                                                    nativeInstalled,
                                                    snapInstalled,
                                                    flatpakInstalled);

        if (source == "native") {
            targets.push_back({package, RemoveSource::Native, purge, {}});
        } else if (source == "snap") {
            targets.push_back({package, RemoveSource::Snap, false, {}});
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

RemoveStatus removeNative(const AppContext& context,
                          const std::string& package,
                          bool purge,
                          float startPct,
                          float endPct,
                          int index,
                          int total) {
    const std::string operation = (context.packageManager == "apt" && purge) ? "purge" : "remove";
    const std::string label = std::to_string(index) + "/" + std::to_string(total) +
                              " | " + nativeShortLabel(context) + " " + operation +
                              ": " + package;

    progressbar_start(startPct, label + " - removing...");

    int exitCode = 1;
    if (context.packageManager == "apt") {
        exitCode = runLogged({"apt-get", "-y", operation, package},
                             "-----apt_" + operation + "_" + package + "-----",
                             {{"DEBIAN_FRONTEND", "noninteractive"}});
    } else if (context.packageManager == "zypper") {
        exitCode = runLogged({"zypper", "--non-interactive", "remove", "-y", package},
                             "-----zypper_remove_" + package + "-----");
    } else if (context.packageManager == "dnf") {
        exitCode = runLogged({context.dnfCommand, "remove", "-y", package},
                             "-----" + context.dnfCommand + "_remove_" + package + "-----");
    }

    if (g_interrupted) {
        return RemoveStatus::Interrupted;
    }

    progressbar_update(endPct, label + (exitCode == 0 ? " - done" : " - failed"));
    return exitCode == 0 ? RemoveStatus::Removed : RemoveStatus::Failed;
}

RemoveStatus removeFlatpak(const AppContext& context,
                           const RemoveTarget& target,
                           float startPct,
                           float endPct,
                           int index,
                           int total) {
    const std::string label = std::to_string(index) + "/" + std::to_string(total) +
                              " | Flatpak: " + target.name +
                              " [" + flatpakScopeName(target.flatpakFlag) + "]";

    progressbar_start(startPct, label + " - removing...");

    std::vector<std::string> args = {"flatpak", "uninstall"};
    if (!target.flatpakFlag.empty()) {
        args.push_back(target.flatpakFlag);
    }
    args.push_back("-y");
    args.push_back("--delete-data");
    args.push_back(target.name);

    runLogged(args,
              "-----flatpak_remove_" + target.name + "-----",
              {},
              userForFlatpakFlag(context, target.flatpakFlag));
    if (g_interrupted) {
        return RemoveStatus::Interrupted;
    }

    const bool stillInstalled = isInstalledFlatpak(context, {target.name, target.flatpakFlag});
    const bool removed = !stillInstalled;
    progressbar_update(endPct, label + (removed ? " - done" : " - failed"));

    if (removed) {
        return RemoveStatus::Removed;
    }
    return RemoveStatus::Failed;
}

RemoveStatus removeSnap(const std::string& package,
                        float startPct,
                        float endPct,
                        int index,
                        int total) {
    const std::string label = std::to_string(index) + "/" + std::to_string(total) +
                              " | Snap: " + package;

    progressbar_start(startPct, label + " - removing...");

    const int exitCode = runLogged({"snap", "remove", package},
                                   "-----snap_remove_" + package + "-----");
    if (g_interrupted) {
        return RemoveStatus::Interrupted;
    }

    progressbar_update(endPct, label + (exitCode == 0 ? " - done" : " - failed"));
    return exitCode == 0 ? RemoveStatus::Removed : RemoveStatus::Failed;
}

RemoveStatus removeTarget(const AppContext& context,
                          const RemoveTarget& target,
                          float startPct,
                          float endPct,
                          int index,
                          int total) {
    switch (target.source) {
        case RemoveSource::Native:
            return removeNative(context, target.name, target.purge, startPct, endPct, index, total);
        case RemoveSource::Flatpak:
            return removeFlatpak(context, target, startPct, endPct, index, total);
        case RemoveSource::Snap:
            return removeSnap(target.name, startPct, endPct, index, total);
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

int runRemoveLoop(const AppContext& context, const std::vector<RemoveTarget>& targets) {
    std::vector<PackageResult> results;
    results.reserve(targets.size());

    bool anyFailed = false;
    const int total = static_cast<int>(targets.size());

    writeLogLine("-----zrm_start-----", LogMode::Truncate);

    for (int i = 0; i < total; ++i) {
        if (g_interrupted) {
            progressbar_finish("Cancelled!");
            std::cout << "\n" << YELLOW << "Cancelled.\n" << RESET;
            return 130;
        }

        const RemoveTarget& target = targets[static_cast<size_t>(i)];
        const float startPct = (100.0f * static_cast<float>(i)) / static_cast<float>(total);
        const float endPct = (100.0f * static_cast<float>(i + 1)) / static_cast<float>(total);

        PackageResult result;
        result.name = target.name;

        const RemoveStatus status = removeTarget(context,
                                                 target,
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

    progressbar_finish("Done!");
    std::cout << "\n";
    for (const PackageResult& result : results) {
        std::cout << result.message << "\n";
    }
    std::cout << GREEN << "Removal complete!\n" << RESET;
    std::cout << YELLOW << "[RAPORT] " << RESET << kLogPath << "\n";

    return 0;
}

AppContext buildContext() {
    AppContext context;
    context.packageManager = get_package_manager();
    context.dnfCommand = commandExists("dnf5") ? "dnf5" : "dnf";
    context.hasFlatpak = commandExists("flatpak");
    context.hasSnap = commandExists("snap");
    context.invokingUser = getInvokingUser();

    if (context.hasFlatpak) {
        for (const char* flag : {"--system", "--user"}) {
            for (const FlatpakPackage& package :
                 listInstalledFlatpaks(flag, userForFlatpakFlag(context, flag))) {
                addUniqueFlatpak(context.installedFlatpaks, package);
            }
        }
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

    if (options.packages.empty()) {
        std::cout << YELLOW << "No package specified!\n" << RESET;
        return 1;
    }

    if (geteuid() != 0) {
        std::cout << RED << "Run with sudo!\n" << RESET;
        return 1;
    }

    zpm_update::checkForUpdates();

    SigintGuard sigintGuard;
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

    return runRemoveLoop(context, targets);
}
