#include "main.h"

#include <cerrno>
#include <csignal>
#include <cctype>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <sys/file.h>
#include <sys/wait.h>

namespace {

constexpr const char* kLogPath = "/tmp/zupgr.log";
constexpr const char* kInstallDir = "/opt/ZPM";
constexpr const char* kRepoApiUrl =
    "https://api.github.com/repos/Zielina-Konrad-productions/ZPM/releases";
constexpr const char* kArchiveBaseUrl =
    "https://github.com/Zielina-Konrad-productions/ZPM/archive/refs/tags/";
constexpr std::chrono::milliseconds kDryRunStepDelay{160};

volatile std::sig_atomic_t g_interrupted = 0;

enum class LogMode {
    Truncate,
    Append
};

struct Options {
    bool showHelp = false;
    bool showVersion = false;
    bool force = false;
    bool experimental = false;
    bool dryRun = false;
};

struct ProcessResult {
    int exitCode = 127;
    std::string output;
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
};

struct ReportAccess {
    bool valid = false;
    gid_t gid = 0;
};

struct LocalVersions {
    std::string stable;
    std::string prerelease;
};

struct ReleaseInfo {
    std::string tag;
    std::string version;

    bool valid() const {
        return !tag.empty() && !version.empty();
    }
};

struct RemoteVersions {
    ReleaseInfo stable;
    ReleaseInfo prerelease;
};

struct ConfigEntry {
    std::string section;
    std::string key;
    std::string line;
    std::vector<std::string> comments;
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

bool isSafeOwnedRegularFile(int fd);

class FileLock {
public:
    bool acquire() {
        const char* paths[] = {"/run/zupgr.lock", "/tmp/zupgr.lock"};
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
            std::cerr << RED << "Error: Another zupgr instance is already running.\n" << RESET;
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

class TempDirectory {
public:
    static std::optional<TempDirectory> create() {
        std::string pattern = "/tmp/zupgr-XXXXXX";
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');

        char* created = mkdtemp(buffer.data());
        if (created == nullptr) {
            return std::nullopt;
        }

        return TempDirectory(std::filesystem::path(created));
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    TempDirectory(TempDirectory&& other) noexcept : path_(std::move(other.path_)) {
        other.path_.clear();
    }

    TempDirectory& operator=(TempDirectory&& other) noexcept {
        if (this != &other) {
            cleanup();
            path_ = std::move(other.path_);
            other.path_.clear();
        }
        return *this;
    }

    ~TempDirectory() {
        cleanup();
    }

    const std::filesystem::path& path() const {
        return path_;
    }

private:
    explicit TempDirectory(std::filesystem::path path) : path_(std::move(path)) {}

    void cleanup() {
        if (path_.empty()) {
            return;
        }

        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        path_.clear();
    }

    std::filesystem::path path_;
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
    std::cout << "\n" << CYAN << "[ZPM-INFO]" << RESET << "\n";
}

void beginUpgradeStep(float progress,
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

bool isSafeOwnedRegularFile(int fd) {
    struct stat st {};
    if (fstat(fd, &st) != 0) {
        return false;
    }

    return S_ISREG(st.st_mode) &&
           st.st_nlink == 1 &&
           st.st_uid == geteuid();
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

ProcessResult runProcess(const std::vector<std::string>& args,
                         const ProcessConfig& config = {}) {
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

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

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

ProcessResult captureLogged(const std::vector<std::string>& args,
                            const std::string& header,
                            LogMode mode = LogMode::Append) {
    ProcessConfig config;
    config.captureStdout = true;
    config.logStderr = true;
    config.header = header;
    config.logMode = mode;
    return runProcess(args, config);
}

int runLogged(const std::vector<std::string>& args,
              const std::string& header,
              LogMode mode = LogMode::Append) {
    ProcessConfig config;
    config.logStdout = true;
    config.logStderr = true;
    config.header = header;
    config.logMode = mode;
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

std::string readFirstLine(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::string line;
    if (std::getline(file, line)) {
        return trim(line);
    }
    return {};
}

std::string normalizeVersionText(std::string value) {
    value = trim(value);
    if (!value.empty() && (value.front() == 'v' || value.front() == 'V')) {
        value.erase(value.begin());
    }
    return value;
}

LocalVersions readLocalVersions() {
    LocalVersions versions;

    const std::string versionFile =
        normalizeVersionText(readFirstLine(std::filesystem::path(kInstallDir) / "VERSION.txt"));
    const std::string preversionFile =
        normalizeVersionText(readFirstLine(std::filesystem::path(kInstallDir) / "PREVERSION.txt"));

    if (!versionFile.empty()) {
        if (versionFile.find('-') != std::string::npos) {
            versions.prerelease = versionFile;
        } else {
            versions.stable = versionFile;
        }
    }

    if (!preversionFile.empty()) {
        versions.prerelease = preversionFile;
    }

    return versions;
}

void printHelp(const char* progName) {
    std::cout << RED << "Usage: " << RESET << progName
              << " [options] or zpm upgr/upgrade [options]\n"
              << RED << "Options:\n" << RESET
              << "  -h, --help           Show this help message\n"
              << "  -v, --version        Show version information\n"
              << "  -f, --force          Force reinstall even if already up to date\n"
              << "  --experimental, -ex  Update ZPM to prerelease versions\n"
              << "  --dry-run            Simulate upgrade flow; no files are changed\n";
}

void printVersion() {
    std::cout << RED << "zupgr component version: v" << zpm_version::version()
              << " of ZPM\n" << RESET
              << "https://github.com/Zielina-Konrad-productions/ZPM\n"
              << "Copyright (c) 2026 Ignacyyy & Ry3ball\n"
              << "License: MIT\n";
}

bool parseOptions(int argc, char* argv[], Options& options) {
    std::vector<std::string> errors;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            options.showHelp = true;
        } else if (arg == "--version" || arg == "-v") {
            options.showVersion = true;
        } else if (arg == "--force" || arg == "-f") {
            options.force = true;
        } else if (arg == "--experimental" || arg == "-ex") {
            options.experimental = true;
        } else if (arg == "--dry-run") {
            options.dryRun = true;
        } else {
            errors.push_back("Unknown option: " + arg);
        }
    }

    if ((options.showHelp || options.showVersion) &&
        (options.force || options.experimental || options.dryRun)) {
        errors.push_back("--help and --version can only be combined with each other.");
    }

    for (const std::string& error : errors) {
        std::cerr << RED << "Error: " << error << RESET << "\n";
    }

    return errors.empty();
}

std::vector<int> parseVersionBase(std::string version) {
    version = normalizeVersionText(version);
    const auto dash = version.find('-');
    if (dash != std::string::npos) {
        version = version.substr(0, dash);
    }

    std::vector<int> segments;
    std::stringstream ss(version);
    std::string part;

    while (std::getline(ss, part, '.')) {
        part = trim(part);
        int value = 0;
        bool ok = !part.empty();

        for (const unsigned char c : part) {
            if (!std::isdigit(c)) {
                ok = false;
                break;
            }
            const int digit = c - '0';
            if (value > (std::numeric_limits<int>::max() - digit) / 10) {
                ok = false;
                break;
            }
            value = value * 10 + digit;
        }

        segments.push_back(ok ? value : 0);
    }

    while (segments.size() < 3) {
        segments.push_back(0);
    }

    return segments;
}

bool isVersionOlder(const std::string& current, const std::string& candidate) {
    if (current.empty()) {
        return true;
    }

    const std::vector<int> a = parseVersionBase(current);
    const std::vector<int> b = parseVersionBase(candidate);
    const size_t count = std::max(a.size(), b.size());

    for (size_t i = 0; i < count; ++i) {
        const int left = i < a.size() ? a[i] : 0;
        const int right = i < b.size() ? b[i] : 0;
        if (left < right) {
            return true;
        }
        if (left > right) {
            return false;
        }
    }

    const auto currentDash = current.find('-');
    const auto candidateDash = candidate.find('-');

    if (currentDash != std::string::npos && candidateDash == std::string::npos) {
        return true;
    }
    if (currentDash == std::string::npos && candidateDash != std::string::npos) {
        return false;
    }
    if (currentDash != std::string::npos && candidateDash != std::string::npos) {
        return current.substr(currentDash + 1) < candidate.substr(candidateDash + 1);
    }

    return false;
}

bool isSafeRemoteTag(const std::string& tag) {
    return !tag.empty() &&
           tag.size() <= 128 &&
           tag != "." &&
           tag != ".." &&
           std::all_of(tag.begin(), tag.end(), [](unsigned char c) {
               return std::isalnum(c) || c == '.' || c == '_' || c == '-';
           });
}

bool parseJsonStringToken(const std::string& text,
                          size_t quote,
                          std::string& value,
                          size_t& next) {
    if (quote >= text.size() || text[quote] != '"') {
        return false;
    }

    value.clear();
    for (size_t i = quote + 1; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '"') {
            next = i + 1;
            return true;
        }

        if (c != '\\') {
            value += c;
            continue;
        }

        if (++i >= text.size()) {
            return false;
        }

        switch (text[i]) {
            case '"':
            case '\\':
            case '/':
                value += text[i];
                break;
            case 'b':
                value += '\b';
                break;
            case 'f':
                value += '\f';
                break;
            case 'n':
                value += '\n';
                break;
            case 'r':
                value += '\r';
                break;
            case 't':
                value += '\t';
                break;
            case 'u':
                if (i + 4 >= text.size()) {
                    return false;
                }
                i += 4;
                value += '?';
                break;
            default:
                return false;
        }
    }

    return false;
}

std::vector<std::string> topLevelJsonObjects(const std::string& json) {
    std::vector<std::string> objects;
    int depth = 0;
    size_t start = std::string::npos;

    for (size_t i = 0; i < json.size(); ++i) {
        if (json[i] == '"') {
            std::string ignored;
            size_t next = 0;
            if (parseJsonStringToken(json, i, ignored, next)) {
                i = next - 1;
            }
            continue;
        }

        if (json[i] == '{') {
            if (depth == 0) {
                start = i;
            }
            ++depth;
        } else if (json[i] == '}') {
            --depth;
            if (depth == 0 && start != std::string::npos) {
                objects.push_back(json.substr(start, i - start + 1));
                start = std::string::npos;
            }
        }
    }

    return objects;
}

std::optional<size_t> findTopLevelFieldValue(const std::string& object,
                                             const std::string& key) {
    int objectDepth = 0;
    int arrayDepth = 0;

    for (size_t i = 0; i < object.size(); ++i) {
        if (object[i] == '"') {
            std::string parsedKey;
            size_t next = 0;
            if (!parseJsonStringToken(object, i, parsedKey, next)) {
                return std::nullopt;
            }

            if (objectDepth == 1 && arrayDepth == 0 && parsedKey == key) {
                size_t value = next;
                while (value < object.size() &&
                       std::isspace(static_cast<unsigned char>(object[value]))) {
                    ++value;
                }

                if (value < object.size() && object[value] == ':') {
                    ++value;
                    while (value < object.size() &&
                           std::isspace(static_cast<unsigned char>(object[value]))) {
                        ++value;
                    }
                    return value;
                }
            }

            i = next - 1;
            continue;
        }

        switch (object[i]) {
            case '{':
                ++objectDepth;
                break;
            case '}':
                --objectDepth;
                break;
            case '[':
                ++arrayDepth;
                break;
            case ']':
                --arrayDepth;
                break;
            default:
                break;
        }
    }

    return std::nullopt;
}

std::optional<std::string> extractJsonString(const std::string& object,
                                             const std::string& key) {
    const auto value = findTopLevelFieldValue(object, key);
    if (!value || *value >= object.size() || object[*value] != '"') {
        return std::nullopt;
    }

    std::string parsed;
    size_t next = 0;
    if (!parseJsonStringToken(object, *value, parsed, next)) {
        return std::nullopt;
    }

    return parsed;
}

std::optional<bool> extractJsonBool(const std::string& object,
                                    const std::string& key) {
    const auto value = findTopLevelFieldValue(object, key);
    if (!value) {
        return std::nullopt;
    }

    if (object.compare(*value, 4, "true") == 0) {
        return true;
    }
    if (object.compare(*value, 5, "false") == 0) {
        return false;
    }
    return std::nullopt;
}

ReleaseInfo makeReleaseInfo(const std::string& rawTag) {
    const std::string tag = trim(rawTag);
    if (!isSafeRemoteTag(tag)) {
        writeLogLine("github: skipped unsafe tag name: " + tag);
        return {};
    }

    ReleaseInfo info;
    info.tag = tag;
    info.version = normalizeVersionText(tag);
    return info;
}

RemoteVersions parseGitHubReleases(const std::string& json) {
    RemoteVersions versions;

    for (const std::string& object : topLevelJsonObjects(json)) {
        const auto tag = extractJsonString(object, "tag_name");
        if (!tag) {
            continue;
        }

        const bool draft = extractJsonBool(object, "draft").value_or(false);
        const bool prerelease = extractJsonBool(object, "prerelease").value_or(false);
        if (draft) {
            continue;
        }

        ReleaseInfo info = makeReleaseInfo(*tag);
        if (!info.valid()) {
            continue;
        }

        if (prerelease) {
            if (!versions.prerelease.valid()) {
                versions.prerelease = info;
            }
        } else if (!versions.stable.valid()) {
            versions.stable = info;
        }

        if (versions.stable.valid() && versions.prerelease.valid()) {
            break;
        }
    }

    return versions;
}

RemoteVersions fetchGitHubVersions() {
    const ProcessResult result = captureLogged(
        {
            "curl",
            "-fsSL",
            "--connect-timeout", "10",
            "--max-time", "30",
            "-H", "User-Agent: ZPM",
            kRepoApiUrl
        },
        "-----fetching_github_releases-----",
        LogMode::Truncate
    );

    if (result.exitCode != 0) {
        writeLogLine("github: curl failed with exit code " + std::to_string(result.exitCode));
        return {};
    }

    RemoteVersions versions = parseGitHubReleases(result.output);
    if (!versions.stable.valid() && !versions.prerelease.valid()) {
        writeLogLine("github: no usable releases found in API response");
    }
    return versions;
}

bool askConfirm() {
    if (g_interrupted) {
        return false;
    }

    std::string answer;
    if (!std::getline(std::cin >> std::ws, answer)) {
        return false;
    }

    answer = toLower(trim(answer));
    return answer == "y" || answer == "yes";
}

bool createDirectories(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        writeLogLine("fs: cannot create " + path.string() + ": " + ec.message());
        return false;
    }
    return true;
}

bool removePath(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    if (ec) {
        writeLogLine("fs: cannot remove " + path.string() + ": " + ec.message());
        return false;
    }
    return true;
}

bool writeTextFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
        writeLogLine("fs: cannot open " + path.string() + " for writing");
        return false;
    }

    file << text;
    if (!file.good()) {
        writeLogLine("fs: cannot write " + path.string());
        return false;
    }

    return true;
}

std::string normalizeConfigLine(std::string line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

std::optional<std::string> parseConfigSection(const std::string& line) {
    const std::string cleaned = trim(line);
    if (cleaned.size() < 3 || cleaned.front() != '[' || cleaned.back() != ']') {
        return std::nullopt;
    }

    std::string section = trim(cleaned.substr(1, cleaned.size() - 2));
    if (section.empty()) {
        return std::nullopt;
    }

    return section;
}

std::optional<std::string> parseConfigKey(const std::string& line) {
    const std::string cleaned = trim(line);
    if (cleaned.empty() || cleaned.front() == '#' || cleaned.front() == ';') {
        return std::nullopt;
    }

    const auto equals = cleaned.find('=');
    if (equals == std::string::npos) {
        return std::nullopt;
    }

    std::string key = trim(cleaned.substr(0, equals));
    if (key.empty()) {
        return std::nullopt;
    }

    return key;
}

std::string configEntryId(const std::string& section, const std::string& key) {
    return section + "\n" + key;
}

std::set<std::string> readConfigKeys(const std::filesystem::path& configPath) {
    std::set<std::string> keys;
    std::ifstream file(configPath);
    if (!file.is_open()) {
        return keys;
    }

    std::string section;
    std::string line;
    while (std::getline(file, line)) {
        line = normalizeConfigLine(line);

        if (const auto parsedSection = parseConfigSection(line)) {
            section = *parsedSection;
            continue;
        }

        if (const auto key = parseConfigKey(line)) {
            keys.insert(configEntryId(section, *key));
        }
    }

    return keys;
}

std::vector<std::string> trimCommentBlock(std::vector<std::string> comments) {
    while (!comments.empty() && trim(comments.front()).empty()) {
        comments.erase(comments.begin());
    }

    while (!comments.empty() && trim(comments.back()).empty()) {
        comments.pop_back();
    }

    return comments;
}

std::vector<ConfigEntry> collectMissingDefaultConfigEntries(
    const std::filesystem::path& defaultPath,
    const std::set<std::string>& existingKeys) {
    std::vector<ConfigEntry> entries;
    std::ifstream file(defaultPath);
    if (!file.is_open()) {
        writeLogLine("config: default config not found: " + defaultPath.string());
        return entries;
    }

    std::string section;
    std::vector<std::string> comments;
    std::string line;

    while (std::getline(file, line)) {
        line = normalizeConfigLine(line);

        if (const auto parsedSection = parseConfigSection(line)) {
            section = *parsedSection;
            comments.clear();
            continue;
        }

        const std::string cleaned = trim(line);
        if (cleaned.empty() || cleaned.front() == '#' || cleaned.front() == ';') {
            comments.push_back(line);
            continue;
        }

        const auto key = parseConfigKey(line);
        if (!key) {
            comments.clear();
            continue;
        }

        if (existingKeys.count(configEntryId(section, *key)) == 0) {
            entries.push_back({section, *key, line, trimCommentBlock(comments)});
        }

        comments.clear();
    }

    return entries;
}

bool appendMissingDefaultConfig(const std::filesystem::path& configPath,
                                const std::filesystem::path& defaultPath) {
    const std::set<std::string> existingKeys = readConfigKeys(configPath);
    const std::vector<ConfigEntry> missing =
        collectMissingDefaultConfigEntries(defaultPath, existingKeys);

    if (missing.empty()) {
        writeLogLine("config: zielina.conf is up to date with default config");
        return true;
    }

    std::ofstream file(configPath, std::ios::app);
    if (!file.is_open()) {
        writeLogLine("config: cannot append missing defaults to " + configPath.string());
        return false;
    }

    file << "\n\n# ---- Added by zupgr from .zielina.conf.default ----\n";

    std::optional<std::string> currentSection;
    for (const ConfigEntry& entry : missing) {
        if (!currentSection || *currentSection != entry.section) {
            file << "\n";
            if (!entry.section.empty()) {
                file << "[" << entry.section << "]\n";
            }
            currentSection = entry.section;
        }

        for (const std::string& comment : entry.comments) {
            file << comment << "\n";
        }

        file << entry.line << "\n";
    }

    if (!file.good()) {
        writeLogLine("config: failed while appending missing defaults");
        return false;
    }

    writeLogLine("config: appended " + std::to_string(missing.size()) +
                 " missing default option(s)");
    return true;
}

bool ensureRequiredCommands() {
    bool ok = true;

    constexpr const char* commands[] = {"curl", "unzip", "g++"};
    for (const char* command : commands) {
        if (!commandExists(command)) {
            std::cerr << RED << "Error: required command not found: "
                      << command << RESET << "\n";
            writeLogLine("dependency: missing command " + std::string(command));
            ok = false;
        }
    }

    return ok;
}

int parseCompilerMajor(const std::string& version) {
    int value = 0;
    bool seenDigit = false;

    for (const unsigned char c : version) {
        if (!std::isdigit(c)) {
            break;
        }
        seenDigit = true;
        const int digit = c - '0';
        if (value > (std::numeric_limits<int>::max() - digit) / 10) {
            return 0;
        }
        value = value * 10 + digit;
    }

    return seenDigit ? value : 0;
}

bool compilerSupportsCpp20() {
    ProcessResult result = captureLogged(
        {"g++", "-dumpfullversion"},
        "-----checking_compiler_version-----"
    );
    if (result.exitCode != 0 || result.output.empty()) {
        result = captureLogged({"g++", "-dumpversion"}, "-----checking_compiler_version-----");
    }

    const int major = parseCompilerMajor(result.output);
    if (major >= 10) {
        return true;
    }

    std::cerr << RED << "Error: g++ 10 or newer is required for ZPM.\n" << RESET;
    writeLogLine("dependency: g++ version is too old or unknown: " + result.output);
    return false;
}

bool downloadRelease(const ReleaseInfo& release, const std::filesystem::path& archivePath) {
    const std::string url = std::string(kArchiveBaseUrl) + release.tag + ".zip";
    const int exitCode = runLogged(
        {
            "curl",
            "-fL",
            "--retry", "3",
            "--retry-delay", "1",
            "--connect-timeout", "10",
            "--max-time", "180",
            "--show-error",
            "--silent",
            "-H", "User-Agent: ZPM",
            "-o", archivePath.string(),
            url
        },
        "-----downloading_ZPM_" + release.version + "-----"
    );

    if (exitCode != 0) {
        writeLogLine("download: curl failed with exit code " + std::to_string(exitCode));
        return false;
    }

    std::error_code ec;
    const auto size = std::filesystem::file_size(archivePath, ec);
    if (ec || size == 0) {
        writeLogLine("download: archive is empty or unreadable: " + archivePath.string());
        return false;
    }

    return true;
}

bool validateSourceTree(const std::filesystem::path& sourceDir) {
    const std::filesystem::path src = sourceDir / "src";
    std::error_code ec;

    if (!std::filesystem::exists(src / "build.sh", ec) ||
        !std::filesystem::exists(src / "common" / "main.h", ec) ||
        !std::filesystem::exists(src / "ZPM.cpp", ec)) {
        writeLogLine("archive: extracted tree is missing required ZPM source files");
        return false;
    }

    std::filesystem::recursive_directory_iterator iterator(
        sourceDir,
        std::filesystem::directory_options::skip_permission_denied,
        ec
    );
    if (ec) {
        writeLogLine("archive: cannot inspect extracted tree: " + ec.message());
        return false;
    }

    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end) {
        std::error_code statusError;
        const auto status = iterator->symlink_status(statusError);
        if (!statusError && std::filesystem::is_symlink(status)) {
            std::error_code linkError;
            const std::filesystem::path target = std::filesystem::read_symlink(iterator->path(), linkError);
            if (linkError || target.is_absolute()) {
                writeLogLine("archive: unsafe symlink " + iterator->path().string());
                return false;
            }

            for (const auto& part : target) {
                if (part == "..") {
                    writeLogLine("archive: unsafe symlink " + iterator->path().string());
                    return false;
                }
            }
        }

        iterator.increment(ec);
        if (ec) {
            writeLogLine("archive: cannot continue tree inspection: " + ec.message());
            return false;
        }
    }

    return true;
}

std::optional<std::filesystem::path> findExtractedSource(const std::filesystem::path& extractDir) {
    std::vector<std::filesystem::path> candidates;
    std::error_code ec;

    std::filesystem::directory_iterator iterator(extractDir, ec);
    if (ec) {
        writeLogLine("archive: cannot read extraction directory: " + ec.message());
        return std::nullopt;
    }

    for (const auto& entry : iterator) {
        std::error_code statusError;
        if (!std::filesystem::is_directory(entry.status(statusError)) || statusError) {
            continue;
        }

        if (std::filesystem::exists(entry.path() / "src" / "build.sh", ec) &&
            std::filesystem::exists(entry.path() / "src" / "common" / "main.h", ec)) {
            candidates.push_back(entry.path());
        }
    }

    if (candidates.size() != 1) {
        writeLogLine("archive: expected one source directory, found " +
                     std::to_string(candidates.size()));
        return std::nullopt;
    }

    if (!validateSourceTree(candidates.front())) {
        return std::nullopt;
    }

    return candidates.front();
}

std::optional<std::filesystem::path> extractRelease(const std::filesystem::path& archivePath,
                                                    const std::filesystem::path& extractDir) {
    if (!createDirectories(extractDir)) {
        return std::nullopt;
    }

    const int exitCode = runLogged(
        {"unzip", "-q", "-o", archivePath.string(), "-d", extractDir.string()},
        "-----extracting_ZPM-----"
    );
    if (exitCode != 0) {
        writeLogLine("archive: unzip failed with exit code " + std::to_string(exitCode));
        return std::nullopt;
    }

    return findExtractedSource(extractDir);
}

std::vector<std::filesystem::path> collectCppFiles(const std::filesystem::path& srcDir) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;

    std::filesystem::directory_iterator iterator(srcDir, ec);
    if (ec) {
        writeLogLine("build: cannot read source directory: " + ec.message());
        return files;
    }

    for (const auto& entry : iterator) {
        std::error_code statusError;
        const auto status = entry.symlink_status(statusError);
        if (!statusError &&
            std::filesystem::is_regular_file(status) &&
            entry.path().extension() == ".cpp") {
            files.push_back(entry.path());
        }
    }

    std::sort(files.begin(), files.end());
    return files;
}

std::string binaryNameForSource(const std::filesystem::path& sourceFile) {
    const std::string stem = sourceFile.stem().string();
    return stem == "ZPM" ? "zpm" : stem;
}

bool setExecutablePermissions(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write |
            std::filesystem::perms::owner_exec |
            std::filesystem::perms::group_read |
            std::filesystem::perms::group_exec |
            std::filesystem::perms::others_read |
            std::filesystem::perms::others_exec,
        std::filesystem::perm_options::replace,
        ec
    );
    if (ec) {
        writeLogLine("fs: cannot chmod " + path.string() + ": " + ec.message());
        return false;
    }
    return true;
}

bool buildRelease(const std::filesystem::path& sourceDir,
                  const std::filesystem::path& outputDir) {
    if (!compilerSupportsCpp20() || !createDirectories(outputDir)) {
        return false;
    }

    const std::filesystem::path srcDir = sourceDir / "src";
    const std::filesystem::path includeDir = srcDir / "common";
    const std::vector<std::filesystem::path> sources = collectCppFiles(srcDir);

    if (sources.empty()) {
        writeLogLine("build: no .cpp files found");
        return false;
    }

    for (const std::filesystem::path& source : sources) {
        if (g_interrupted) {
            return false;
        }

        const std::string outName = binaryNameForSource(source);
        const std::filesystem::path outPath = outputDir / outName;
        const int exitCode = runLogged(
            {
                "g++",
                source.string(),
                "-std=c++20",
                "-O2",
                "-I", includeDir.string(),
                "-o", outPath.string()
            },
            "-----building_" + outName + "-----"
        );

        if (exitCode != 0) {
            writeLogLine("build: " + outName + " failed with exit code " +
                         std::to_string(exitCode));
            return false;
        }

        if (!setExecutablePermissions(outPath)) {
            return false;
        }
    }

    return true;
}

std::filesystem::path uniqueOptPath(const std::string& name) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    return std::filesystem::path("/opt") /
           (name + "." + std::to_string(getpid()) + "." + std::to_string(seconds));
}

bool copyBuiltBinaries(const std::filesystem::path& builtDir,
                       const std::filesystem::path& installBinDir) {
    if (!removePath(installBinDir) || !createDirectories(installBinDir)) {
        return false;
    }

    std::error_code ec;
    std::filesystem::directory_iterator iterator(builtDir, ec);
    if (ec) {
        writeLogLine("install: cannot read built binaries: " + ec.message());
        return false;
    }

    for (const auto& entry : iterator) {
        std::error_code statusError;
        const auto status = entry.symlink_status(statusError);
        if (statusError || !std::filesystem::is_regular_file(status)) {
            continue;
        }

        const std::filesystem::path target = installBinDir / entry.path().filename();
        std::filesystem::copy_file(
            entry.path(),
            target,
            std::filesystem::copy_options::overwrite_existing,
            ec
        );
        if (ec) {
            writeLogLine("install: cannot copy " + entry.path().string() + ": " + ec.message());
            return false;
        }

        if (!setExecutablePermissions(target)) {
            return false;
        }
    }

    return true;
}

bool isRegularFile(const std::filesystem::path& path) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    return !ec && std::filesystem::is_regular_file(status);
}

bool copyConfigFile(const std::filesystem::path& from,
                    const std::filesystem::path& to,
                    const std::string& description) {
    std::error_code ec;
    std::filesystem::copy_file(
        from,
        to,
        std::filesystem::copy_options::overwrite_existing,
        ec
    );
    if (ec) {
        writeLogLine("config: cannot " + description + ": " + ec.message());
        return false;
    }

    return true;
}

bool installMergedConfig(const std::filesystem::path& newInstallDir) {
    const std::filesystem::path installedConfig =
        std::filesystem::path(kInstallDir) / "zielina.conf";
    const std::filesystem::path defaultConfig =
        newInstallDir / ".zielina.conf.default";
    const std::filesystem::path targetConfig =
        newInstallDir / "zielina.conf";

    std::error_code ec;
    const bool hasInstalledConfig = std::filesystem::exists(installedConfig, ec);
    if (hasInstalledConfig && isRegularFile(installedConfig)) {
        if (!copyConfigFile(installedConfig, targetConfig, "preserve existing zielina.conf")) {
            return false;
        }
    } else if (hasInstalledConfig) {
        writeLogLine("config: existing zielina.conf is not a regular file, using defaults");
        if (isRegularFile(defaultConfig) &&
            !copyConfigFile(defaultConfig, targetConfig, "install default zielina.conf")) {
            return false;
        }
    } else if (isRegularFile(defaultConfig)) {
        if (!copyConfigFile(defaultConfig, targetConfig, "install default zielina.conf")) {
            return false;
        }
    }

    if (!isRegularFile(targetConfig)) {
        writeLogLine("config: no zielina.conf available after install preparation");
        return true;
    }

    if (!isRegularFile(defaultConfig)) {
        writeLogLine("config: .zielina.conf.default not found, merge skipped");
        return true;
    }

    return appendMissingDefaultConfig(targetConfig, defaultConfig);
}

bool writeVersionFiles(const std::filesystem::path& installDir,
                       const ReleaseInfo& release,
                       bool prerelease) {
    if (!writeTextFile(installDir / "VERSION.txt", release.version + "\n")) {
        return false;
    }

    const std::filesystem::path preversion = installDir / "PREVERSION.txt";
    if (prerelease) {
        return writeTextFile(preversion, release.version + "\n");
    }

    std::error_code ec;
    std::filesystem::remove(preversion, ec);
    if (ec) {
        writeLogLine("install: cannot remove stale PREVERSION.txt: " + ec.message());
        return false;
    }
    return true;
}

bool prepareInstallTree(const std::filesystem::path& sourceDir,
                        const std::filesystem::path& builtDir,
                        const std::filesystem::path& newInstallDir,
                        const ReleaseInfo& release,
                        bool prerelease) {
    std::error_code ec;
    if (std::filesystem::exists(newInstallDir, ec)) {
        writeLogLine("install: staging directory already exists: " + newInstallDir.string());
        return false;
    }

    std::filesystem::copy(
        sourceDir,
        newInstallDir,
        std::filesystem::copy_options::recursive |
            std::filesystem::copy_options::copy_symlinks,
        ec
    );
    if (ec) {
        writeLogLine("install: cannot copy source tree: " + ec.message());
        return false;
    }

    return copyBuiltBinaries(builtDir, newInstallDir / "bin") &&
           installMergedConfig(newInstallDir) &&
           writeVersionFiles(newInstallDir, release, prerelease);
}

bool replaceSymlinkTarget(const std::filesystem::path& target,
                          const std::filesystem::path& newTarget) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(target, ec);
    std::filesystem::path oldTarget;

    if (ec) {
        if (ec == std::errc::no_such_file_or_directory) {
            ec.clear();
        } else {
            writeLogLine("symlink: cannot inspect " + target.string() + ": " + ec.message());
            return false;
        }
    } else if (std::filesystem::exists(status)) {
        if (!std::filesystem::is_symlink(status)) {
            writeLogLine("symlink: refusing to overwrite non-symlink " + target.string());
            return false;
        }

        oldTarget = std::filesystem::read_symlink(target, ec);
        if (ec) {
            writeLogLine("symlink: cannot read " + target.string() + ": " + ec.message());
            return false;
        }

        std::filesystem::remove(target, ec);
        if (ec) {
            writeLogLine("symlink: cannot remove " + target.string() + ": " + ec.message());
            return false;
        }
    } else if (!std::filesystem::is_symlink(status)) {
        writeLogLine("symlink: refusing to overwrite non-symlink " + target.string());
        return false;
    }

    std::filesystem::create_symlink(newTarget, target, ec);
    if (ec) {
        writeLogLine("symlink: cannot create " + target.string() + ": " + ec.message());
        if (!oldTarget.empty()) {
            std::error_code restoreError;
            std::filesystem::create_symlink(oldTarget, target, restoreError);
            if (restoreError) {
                writeLogLine("symlink: cannot restore old " + target.string() + ": " +
                             restoreError.message());
            }
        }
        return false;
    }

    return true;
}

bool updateSymlinks(const std::filesystem::path& installBinDir) {
    std::error_code ec;
    std::filesystem::directory_iterator iterator(installBinDir, ec);
    if (ec) {
        writeLogLine("symlink: cannot read bin directory: " + ec.message());
        return false;
    }

    for (const auto& entry : iterator) {
        std::error_code statusError;
        const auto status = entry.symlink_status(statusError);
        const std::string name = entry.path().filename().string();

        if (statusError ||
            !std::filesystem::is_regular_file(status) ||
            !startsWith(name, "z")) {
            continue;
        }

        const std::filesystem::path target = std::filesystem::path("/usr/bin") / name;
        if (!replaceSymlinkTarget(target, entry.path())) {
            return false;
        }
    }

    return true;
}

bool rollbackInstall(const std::filesystem::path& backupDir) {
    if (backupDir.empty()) {
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::exists(backupDir, ec)) {
        return false;
    }

    std::filesystem::remove_all(kInstallDir, ec);
    if (ec) {
        writeLogLine("rollback: cannot remove failed install: " + ec.message());
        return false;
    }

    std::filesystem::rename(backupDir, kInstallDir, ec);
    if (ec) {
        writeLogLine("rollback: cannot restore previous install: " + ec.message());
        return false;
    }

    writeLogLine("rollback: previous installation restored");
    return true;
}

bool commitInstallTree(const std::filesystem::path& newInstallDir) {
    const std::filesystem::path backupDir = uniqueOptPath("ZPM.backup.zupgr");
    bool hasBackup = false;
    std::error_code ec;

    if (std::filesystem::exists(kInstallDir, ec)) {
        std::filesystem::rename(kInstallDir, backupDir, ec);
        if (ec) {
            writeLogLine("install: cannot move current installation to backup: " + ec.message());
            return false;
        }
        hasBackup = true;
    }

    std::filesystem::rename(newInstallDir, kInstallDir, ec);
    if (ec) {
        writeLogLine("install: cannot activate new installation: " + ec.message());
        if (hasBackup) {
            rollbackInstall(backupDir);
        }
        return false;
    }

    if (!updateSymlinks(std::filesystem::path(kInstallDir) / "bin")) {
        if (hasBackup) {
            rollbackInstall(backupDir);
        }
        return false;
    }

    if (hasBackup) {
        std::filesystem::remove_all(backupDir, ec);
        if (ec) {
            writeLogLine("install: update succeeded, but backup cleanup failed: " + ec.message());
        }
    }

    return true;
}

bool installRelease(const std::filesystem::path& sourceDir,
                    const std::filesystem::path& builtDir,
                    const ReleaseInfo& release,
                    bool prerelease) {
    const std::filesystem::path newInstallDir = uniqueOptPath("ZPM.new.zupgr");

    if (!prepareInstallTree(sourceDir, builtDir, newInstallDir, release, prerelease)) {
        removePath(newInstallDir);
        return false;
    }

    if (!commitInstallTree(newInstallDir)) {
        removePath(newInstallDir);
        return false;
    }

    return true;
}

bool runUpdate(const ReleaseInfo& release, bool prerelease) {
    if (!release.valid()) {
        std::cerr << RED << "Error: invalid release metadata.\n" << RESET;
        return false;
    }

    writeLogLine("---starting update to " + release.version + "---", LogMode::Truncate);

    auto temp = TempDirectory::create();
    if (!temp) {
        writeLogLine("temp: cannot create temporary directory");
        std::cerr << RED << "Error: Cannot create temporary directory.\n" << RESET;
        return false;
    }

    const std::filesystem::path archivePath = temp->path() / "ZPM.zip";
    const std::filesystem::path extractDir = temp->path() / "extract";
    const std::filesystem::path buildDir = temp->path() / "bin";

    bool ok = true;
    std::optional<std::filesystem::path> sourceDir;
    int progressStep = 0;

    printInfoHeader();
    progressbar_start(0.0f, "0/6 | Starting update...");

    if (ok) {
        beginUpgradeStep(10.0f,
                         "1/6 | Checking tools...",
                         progressStep,
                         "checking required tools");
        ok = ensureRequiredCommands();
    }

    if (ok && !g_interrupted) {
        beginUpgradeStep(25.0f,
                         "2/6 | Downloading release...",
                         progressStep,
                         "downloading ZPM release");
        ok = downloadRelease(release, archivePath);
    }

    if (ok && !g_interrupted) {
        beginUpgradeStep(40.0f,
                         "3/6 | Extracting release...",
                         progressStep,
                         "extracting release archive");
        sourceDir = extractRelease(archivePath, extractDir);
        ok = sourceDir.has_value();
    }

    if (ok && !g_interrupted) {
        beginUpgradeStep(60.0f,
                         "4/6 | Building ZPM...",
                         progressStep,
                         "building ZPM");
        ok = buildRelease(*sourceDir, buildDir);
    }

    if (ok && !g_interrupted) {
        beginUpgradeStep(85.0f,
                         "5/6 | Installing ZPM...",
                         progressStep,
                         "installing ZPM");
        ok = installRelease(*sourceDir, buildDir, release, prerelease);
    }

    if (g_interrupted) {
        ok = false;
        writeLogLine("update: interrupted by user");
    }

    beginUpgradeStep(95.0f,
                     "6/6 | Cleaning up...",
                     progressStep,
                     "cleaning");

    if (ok) {
        progressbar_finish("6/6 | DONE!");
        std::cout << YELLOW << "[RAPORT]" << RESET << " " << kLogPath << "\n";
        return true;
    }

    progressbar_finish("6/6 | ERROR!");
    std::cout << RED << "ERROR," << RESET << " check " << kLogPath << " for details.\n"
              << RED << "If ZPM is not usable, reinstall with:\n" << RESET
              << BOLD << "sudo bash -c \"$(curl -fsSL https://raw.githubusercontent.com/"
                         "Zielina-Konrad-productions/ZPM/main/INETINSTALL.sh)\"\n"
              << RESET;
    return false;
}

bool dryRunStep() {
    std::this_thread::sleep_for(kDryRunStepDelay);
    return !g_interrupted;
}

int handleDryRun(const Options& options) {
    const std::string releaseType = options.experimental ? "prerelease" : "stable";

    std::cout << "\n" << RED << "Dry run demo: " << RESET
              << "no files will be changed.\n";
    std::cout << YELLOW << "[SYS] " << RESET
              << "Simulating " << releaseType << " ZPM upgrade flow";
    if (options.force) {
        std::cout << " (force)";
    }
    std::cout << "\n";

    int progressStep = 0;
    bool ok = true;

    printInfoHeader();
    progressbar_start(0.0f, "0/6 | Starting dry run...");

    const auto runStep = [&progressStep, &ok](float progress,
                                              const std::string& progressText,
                                              const std::string& infoText) {
        if (!ok || g_interrupted) {
            return;
        }

        beginUpgradeStep(progress, progressText, progressStep, infoText);
        ok = dryRunStep();
    };

    runStep(10.0f, "1/6 | Checking tools...", "checking required tools");
    runStep(25.0f, "2/6 | Downloading release...", "downloading ZPM release");
    runStep(40.0f, "3/6 | Extracting release...", "extracting release archive");
    runStep(60.0f, "4/6 | Building ZPM...", "building ZPM");
    runStep(85.0f, "5/6 | Installing ZPM...", "installing ZPM");
    runStep(95.0f, "6/6 | Cleaning up...", "cleaning");

    if (!ok || g_interrupted) {
        progressbar_finish("Dry run interrupted!");
        std::cout << RED << "Dry run failed or was interrupted.\n" << RESET;
        return g_interrupted ? 130 : 1;
    }

    progressbar_finish("Dry run done!");
    std::cout << GREEN << "Dry run complete! No files were changed.\n" << RESET;
    return 0;
}

void printLocalVersions(const LocalVersions& versions) {
    if (!versions.stable.empty()) {
        std::cout << "ZPM installed version:" << YELLOW << " v"
                  << versions.stable << RESET << "\n";
    } else {
        std::cout << "ZPM installed version: " << YELLOW << "none" << RESET << "\n";
    }

    if (!versions.prerelease.empty()) {
        std::cout << "ZPM installed preversion:" << YELLOW << " v"
                  << versions.prerelease << RESET << "\n";
    } else {
        std::cout << "ZPM installed preversion: " << YELLOW << "none" << RESET << "\n";
    }
}

void printRemoteVersions(const RemoteVersions& versions) {
    if (versions.stable.valid()) {
        std::cout << "ZPM latest version:" << YELLOW << " v"
                  << versions.stable.version << RESET << "\n";
    } else {
        std::cout << "ZPM latest version: " << YELLOW
                  << "unknown (no network?)" << RESET << "\n";
    }

    if (versions.prerelease.valid()) {
        std::cout << "ZPM latest preversion:" << YELLOW << " v"
                  << versions.prerelease.version << RESET << "\n";
    } else {
        std::cout << "ZPM latest preversion: " << YELLOW
                  << "none available" << RESET << "\n";
    }
}

int handleExperimental(const Options& options,
                       const LocalVersions& local,
                       const RemoteVersions& remote) {
    if (!remote.prerelease.valid()) {
        std::cout << RED << "No prerelease version available.\n" << RESET;
        return 0;
    }

    if (local.prerelease.empty() ||
        isVersionOlder(local.prerelease, remote.prerelease.version) ||
        options.force) {
        std::cout << GREEN << "ZPM prerelease update available" << RESET
                  << ", continue? [y/n]: ";
        if (!askConfirm()) {
            std::cout << "Update cancelled.\n";
            return 0;
        }

        std::cout << RED << "Updating ZPM...\n" << RESET;
        return runUpdate(remote.prerelease, true) ? 0 : 1;
    }

    std::cout << "ZPM prerelease is up to date.\n";
    return 0;
}

int handleStable(const Options& options,
                 const LocalVersions& local,
                 const RemoteVersions& remote) {
    if (!remote.stable.valid()) {
        std::cout << RED << "Could not fetch latest version (no network?).\n" << RESET;
        return 1;
    }

    const bool localIsPrerelease = local.stable.empty() && !local.prerelease.empty();
    if (localIsPrerelease && !options.force) {
        std::cout << YELLOW << "Currently on prerelease (" << local.prerelease
                  << "), stable " << remote.stable.version << " available.\n" << RESET;
        std::cout << RED << "To update ZPM to normal release use -f or --force"
                  << RESET << "\n";
        return 0;
    }

    if (local.stable.empty() ||
        isVersionOlder(local.stable, remote.stable.version) ||
        options.force) {
        std::cout << GREEN << "ZPM update available" << RESET << "\n"
                  << "Continue? [y/n]: ";
        if (!askConfirm()) {
            std::cout << "Update cancelled.\n";
            return 0;
        }

        return runUpdate(remote.stable, false) ? 0 : 1;
    }

    std::cout << RED << "ZPM is up to date.\n" << RESET;
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    Options options;

    if (!parseOptions(argc, argv, options)) {
        std::cerr << YELLOW << "Use --help to show available options." << RESET << "\n";
        return 1;
    }

    if (options.showVersion && options.showHelp) {
        std::cout << YELLOW << "--version\n" << RESET;
        printVersion();
        std::cout << "\n" << YELLOW << "--help\n" << RESET;
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
        return handleDryRun(options);
    }

    if (geteuid() != 0) {
        std::cerr << RED << "Run with sudo!\n" << RESET;
        return 1;
    }

    FileLock lock;
    if (!lock.acquire()) {
        return 1;
    }

    SigintGuard sigintGuard;

    std::cout << RED << "ZPM Update program\n\n" << RESET;

    std::cout << BOLD << "INSTALLED ZPM VERSIONS\n" << RESET
              << CYAN << "------------------------------------------------------\n" << RESET;
    const LocalVersions local = readLocalVersions();
    printLocalVersions(local);
    std::cout << CYAN << "------------------------------------------------------\n\n" << RESET;

    std::cout << BOLD << "INTERNET ZPM VERSIONS\n" << RESET
              << CYAN << "------------------------------------------------------\n" << RESET;
    const RemoteVersions remote = fetchGitHubVersions();
    printRemoteVersions(remote);
    std::cout << CYAN << "------------------------------------------------------\n\n" << RESET;

    const int result = options.experimental
        ? handleExperimental(options, local, remote)
        : handleStable(options, local, remote);

    if (g_interrupted) {
        return 130;
    }
    return result;
}
