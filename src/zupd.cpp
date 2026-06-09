#include "main.h"

#include <cerrno>
#include <csignal>
#include <cctype>
#include <cstring>
#include <set>
#include <sys/file.h>
#include <sys/wait.h>

namespace {

constexpr const char* kLogFile = "/tmp/zupd.log";
constexpr const char* kPatchLogFile = "/tmp/zupd_patchcheck.log";

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
};

struct CommandResult {
    int exitCode = 127;
    std::string output;
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

            struct stat st {};
            if (fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode)) {
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
                      (mode == LogMode::Truncate ? O_TRUNC : O_APPEND);

    FileDescriptor fd(open(path, flags, 0600));
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

int runShellLogged(const std::string& command,
                   const std::string& header,
                   LogMode mode = LogMode::Append,
                   const char* logPath = kLogFile) {
    FileDescriptor log = openLogFile(logPath, mode);
    if (!log.valid()) {
        return 127;
    }

    if (!header.empty()) {
        writeAll(log.get(), header + "\n");
    }

    const pid_t pid = fork();
    if (pid < 0) {
        return 127;
    }

    if (pid == 0) {
        dup2(log.get(), STDOUT_FILENO);
        dup2(log.get(), STDERR_FILENO);
        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    return waitForChild(pid);
}

CommandResult runShellCapture(const std::string& command,
                              const char* logPath = nullptr,
                              const std::string& header = {},
                              LogMode mode = LogMode::Append,
                              bool mirrorStdoutToLog = false,
                              bool stderrToLog = false) {
    int pipeFd[2] = {-1, -1};
    if (pipe(pipeFd) != 0) {
        return {};
    }

    fcntl(pipeFd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipeFd[1], F_SETFD, FD_CLOEXEC);

    FileDescriptor readEnd(pipeFd[0]);
    FileDescriptor writeEnd(pipeFd[1]);
    FileDescriptor log;

    if (logPath != nullptr && (!header.empty() || mirrorStdoutToLog || stderrToLog)) {
        log = openLogFile(logPath, mode);
        if (!log.valid()) {
            return {};
        }
        if (!header.empty()) {
            writeAll(log.get(), header + "\n");
        }
    }

    const pid_t pid = fork();
    if (pid < 0) {
        return {};
    }

    if (pid == 0) {
        readEnd.reset();
        dup2(writeEnd.get(), STDOUT_FILENO);

        if (stderrToLog && log.valid()) {
            dup2(log.get(), STDERR_FILENO);
        } else {
            redirectToDevNull(STDERR_FILENO);
        }

        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    writeEnd.reset();

    CommandResult result;
    std::array<char, 4096> buffer {};

    for (;;) {
        const ssize_t count = read(readEnd.get(), buffer.data(), buffer.size());
        if (count > 0) {
            result.output.append(buffer.data(), static_cast<size_t>(count));
            if (mirrorStdoutToLog && log.valid()) {
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

    result.exitCode = waitForChild(pid);
    return result;
}

int runShellSplitLogs(const std::string& command,
                      const char* stdoutLogPath,
                      const std::string& stdoutHeader,
                      LogMode stdoutMode,
                      const char* stderrLogPath) {
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
        dup2(stdoutLog.get(), STDOUT_FILENO);
        dup2(stderrLog.get(), STDERR_FILENO);
        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    return waitForChild(pid);
}

bool runShellOk(const std::string& command,
                const std::string& header,
                LogMode mode = LogMode::Append) {
    return runShellLogged(command, header, mode) == 0;
}

std::string shellQuote(const std::string& value) {
    std::string quoted = "'";
    for (const char c : value) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted += c;
        }
    }
    quoted += "'";
    return quoted;
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

void printCommandLines(const std::string& command, const std::string& prefix) {
    const CommandResult result = runShellCapture(command);
    for (const std::string& rawLine : splitLines(result.output)) {
        const std::string line = trim(rawLine);
        if (!line.empty()) {
            std::cout << prefix << line << "\n";
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
    int steps = 2; // consistency check + cleanup

    if (status.native) {
        ++steps;
    }
    if (status.hasFlatpakUpdates()) {
        ++steps;
    }
    if (status.hasSnapUpdates()) {
        ++steps;
    }

    return steps;
}

bool checkInterrupted(bool& ok) {
    if (!g_interrupted) {
        return false;
    }

    std::cout << "\n" << YELLOW << "Cancelled by user (Ctrl+C).\n" << RESET;
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
        const CommandResult result = runShellCapture("flatpak remote-ls --updates --columns=name");
        status.flatpakPackages = nonEmptyLines(result.output);
    }

    if (status.hasSnap) {
        const CommandResult result = runShellCapture("snap refresh --list");
        status.snapPackages = parseSnapRefreshList(result.output);
    }
}

bool cleanupUniversal(const UpdateStatus& status) {
    bool ok = true;

    if (status.hasFlatpak) {
        ok = runShellOk("flatpak uninstall --unused -y", "----cleaning_flatpak----") && ok;
        writeLogHeader("----cleaning_flatpak_cache----");
        ok = removeEntriesWithPrefix("/var/tmp", "flatpak-cache-") && ok;
    }

    if (status.hasSnap) {
        const CommandResult list = runShellCapture("snap list --all",
                                                   kLogFile,
                                                   "----checking_disabled_snap_revisions----",
                                                   LogMode::Append,
                                                   true,
                                                   true);

        if (list.exitCode != 0) {
            ok = false;
        }

        for (const auto& [name, revision] : parseDisabledSnapRevisions(list.output)) {
            const std::string command = "snap remove " + shellQuote(name) +
                                        " --revision=" + shellQuote(revision);
            ok = runShellOk(command, "----removing_snap_revision_" + name + "_" + revision + "----") && ok;
        }

        writeLogHeader("----cleaning_snap_cache----");
        ok = removeDirectoryEntries("/var/lib/snapd/cache") && ok;
    }

    return ok;
}

bool finishAndReport(const Options& options, bool ok, int total, int step) {
    if (g_interrupted) {
        ok = false;
    }

    if (ok) {
        progressbar_set_state(UiState::DONE, total);
        progressbar_finish("DONE!");
    } else {
        progressbar_set_state(UiState::ERROR, step);
        progressbar_finish("ERROR!");

        std::cout << RED << "ERROR," << RESET
                  << " check " << kLogFile << " for details.\n";
        return false;
    }

    std::cout << YELLOW << "[RAPORT]" << RESET << " " << kLogFile << "\n";

    if (options.reboot) {
        std::cout << YELLOW << "[*] Rebooting in 3s..." << RESET << "\n";
        std::this_thread::sleep_for(std::chrono::seconds(3));
        runShellLogged("reboot", "-----reboot-----");
    } else if (options.shutdown) {
        std::cout << YELLOW << "[*] Shutting down in 3s..." << RESET << "\n";
        std::this_thread::sleep_for(std::chrono::seconds(3));
        runShellLogged("shutdown -h now", "-----shutdown-----");
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
        } else {
            errors.push_back("Unknown option: " + arg);
        }
    }

    if (options.reboot && options.shutdown) {
        errors.push_back("-r/--reboot and -s/--shutdown are mutually exclusive.");
    }

    if ((options.help || options.version) &&
        (options.reboot || options.shutdown || options.yes || options.fullUpdate)) {
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

        printCommandLines(
            "{ grep -rhE '^deb ' /etc/apt/sources.list /etc/apt/sources.list.d/ 2>/dev/null;"
            "  grep -rhE '^URIs:' /etc/apt/sources.list.d/*.sources 2>/dev/null"
            "    | sed 's/^URIs:[[:space:]]*/deb /'; } | sort -u",
            prefix
        );
    } else if (packageManager == "zypper") {
        std::cout << "\n" << YELLOW << "[Z]" << RESET
                  << GREEN << " Zypper Repositories:\n" << RESET;

        printCommandLines(
            "zypper lr 2>/dev/null"
            " | awk -F'|' '/^[[:space:]]*[0-9]+/{"
            "gsub(/^[[:space:]]+|[[:space:]]+$/, \"\", $2); print $2}'",
            prefix
        );
    } else if (packageManager == "dnf") {
        std::cout << "\n" << YELLOW << "[R]" << RESET
                  << GREEN << " DNF Repositories:\n" << RESET;

        printCommandLines(
            commandExists("dnf5")
                ? "dnf5 repolist -q 2>/dev/null | awk 'NR>1 && NF{print $1}'"
                : "dnf repolist -q 2>/dev/null | awk 'NR>1 && NF{print $1}'",
            prefix
        );
    }

    if (commandExists("flatpak") ||
        commandExists("/usr/bin/flatpak") ||
        commandExists("/usr/local/bin/flatpak")) {
        std::cout << "\n" << YELLOW << "[F]" << RESET
                  << GREEN << " Flatpak Remotes:\n" << RESET;

        printCommandLines("flatpak remotes --columns=name 2>/dev/null", prefix);
    }

    if (commandExists("snap") || commandExists("/usr/bin/snap")) {
        std::cout << "\n" << YELLOW << "[S]" << RESET
                  << GREEN << " Snap is available.\n" << RESET;
    }
}

UpdateStatus aptCheckUpdates() {
    UpdateStatus status;

    std::cout << "\n" << YELLOW << "[*] Refreshing package cache..." << RESET << "\n";

    if (!runShellOk("apt-get update -qq", "-----apt_update-----", LogMode::Truncate)) {
        status.checkError = true;
        checkUniversalManagers(status);
        return status;
    }

    const CommandResult simulation = runShellCapture(
        "LC_ALL=C apt-get dist-upgrade -s",
        kLogFile,
        "-----apt_simulate_dist_upgrade-----",
        LogMode::Append,
        true,
        true
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

    if (!runShellOk("zypper --non-interactive refresh",
                    "-----zypper_refresh-----",
                    LogMode::Truncate)) {
        status.checkError = true;
        checkUniversalManagers(status);
        return status;
    }

    const std::string updateCommand = status.zypperDup
        ? "LC_ALL=C zypper --no-refresh list-updates --all -t package"
        : "LC_ALL=C zypper --no-refresh list-updates -t package";

    const CommandResult updates = runShellCapture(
        updateCommand,
        kLogFile,
        "-----zypper_list_updates-----",
        LogMode::Append,
        true,
        true
    );

    if (updates.exitCode != 0) {
        status.checkError = true;
        checkUniversalManagers(status);
        return status;
    }

    status.nativePackages = parseZypperListUpdates(updates.output);

    const int patchExit = runShellSplitLogs(
        "LC_ALL=C zypper --no-refresh patch-check",
        kPatchLogFile,
        "-----zypper_patch_check-----",
        LogMode::Truncate,
        kLogFile
    );

    const bool hasPatches = (patchExit == 100 || patchExit == 101);
    if (patchExit != 0 && !hasPatches) {
        status.checkError = true;
    }

    if (!status.zypperDup) {
        const CommandResult patches = runShellCapture(
            "LC_ALL=C zypper --no-refresh list-patches",
            kLogFile,
            "-----zypper_list_patches-----",
            LogMode::Append,
            true,
            true
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

    const std::string command = status.dnf5
        ? "LC_ALL=C dnf5 check-upgrade -q"
        : "LC_ALL=C dnf check-update -q --refresh";

    const CommandResult result = runShellCapture(
        command,
        kLogFile,
        status.dnf5 ? "-----dnf5_check_upgrade-----" : "-----dnf_check_update-----",
        LogMode::Truncate,
        true,
        true
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

    progressbar_start(total);

    progressbar_set_state(UiState::CHECKING, ++step);
    if (!checkConsistency()) {
        ok = false;
    }
    if (checkInterrupted(ok)) {
        return finishAndReport(options, ok, total, step);
    }

    if (status.native && ok) {
        progressbar_set_state(nativeState, ++step);
        const NativeUpdateResult result = updateNative();

        if (result == NativeUpdateResult::RestartRequired) {
            progressbar_finish("RESTART NEEDED");

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
        return finishAndReport(options, ok, total, step);
    }

    if (status.hasFlatpakUpdates() && ok) {
        progressbar_set_state(UiState::FLATPAK, ++step);
        if (!runShellOk("flatpak update -y", "----updating_flatpak----")) {
            ok = false;
        }
    }
    if (checkInterrupted(ok)) {
        return finishAndReport(options, ok, total, step);
    }

    if (status.hasSnapUpdates() && ok) {
        progressbar_set_state(UiState::SNAP, ++step);
        if (!runShellOk("snap refresh", "----updating_snap----")) {
            ok = false;
        }
    }
    if (checkInterrupted(ok)) {
        return finishAndReport(options, ok, total, step);
    }

    progressbar_set_state(UiState::CLEANUP, ++step);
    if (!cleanupNative()) {
        ok = false;
    }
    if (!cleanupUniversal(status)) {
        ok = false;
    }

    checkInterrupted(ok);
    return finishAndReport(options, ok, total, step);
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
            return runShellOk(
                "DEBIAN_FRONTEND=noninteractive dpkg --configure -a",
                "-----checking_system_consistency-----"
            );
        },
        [] {
            return runShellOk(
                "DEBIAN_FRONTEND=noninteractive apt-get dist-upgrade -y "
                "-o Dpkg::Options::=--force-confdef "
                "-o Dpkg::Options::=--force-confold",
                "-----updating_APT-----"
            ) ? NativeUpdateResult::Ok : NativeUpdateResult::Failed;
        },
        [] {
            return runShellOk(
                "DEBIAN_FRONTEND=noninteractive apt-get autoremove -y && apt-get autoclean",
                "----cleaning_APT----"
            );
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
            writeLogHeader("-----checking_system_consistency-----");
            return true;
        },
        [&status] {
            const std::string command = status.zypperDup
                ? "zypper --non-interactive dup -y --auto-agree-with-licenses"
                : "zypper --non-interactive patch --with-update -y --auto-agree-with-licenses";

            const int exitCode = runShellLogged(
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
            return runShellOk("zypper clean -a", "----cleaning_zypper----");
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
            writeLogHeader("-----checking_system_consistency-----");
            return true;
        },
        [&options, &status] {
            std::string command;
            std::string header;

            if (status.dnf5) {
                command = options.fullUpdate ? "dnf5 distro-sync -y" : "dnf5 upgrade -y";
                header = options.fullUpdate ? "-----updating_DNF5_distro-sync-----" : "-----updating_DNF5-----";
            } else {
                command = options.fullUpdate ? "dnf distro-sync -y" : "dnf upgrade -y";
                header = options.fullUpdate ? "-----updating_DNF_distro-sync-----" : "-----updating_DNF-----";
            }

            return runShellOk(command, header) ? NativeUpdateResult::Ok : NativeUpdateResult::Failed;
        },
        [&status] {
            return status.dnf5
                ? runShellOk("dnf5 autoremove -y && dnf5 clean packages", "----cleaning_DNF5----")
                : runShellOk("dnf autoremove -y && dnf clean packages", "----cleaning_DNF----");
        }
    );
}

int handleApt(const Options& options) {
    UpdateStatus status = aptCheckUpdates();

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
        std::cout << YELLOW << "FULL UPDATE MODE" << RESET << "\n";
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
        std::cout << YELLOW << "FULL UPDATE MODE (dup)" << RESET << "\n";
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
        std::cout << YELLOW << "FULL UPDATE MODE (distro-sync)" << RESET << "\n";
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
