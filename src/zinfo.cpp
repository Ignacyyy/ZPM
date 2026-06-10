// zinfo.cpp - part of ZPM
// Show package information from the native package manager, Flatpak and Snap.

#include "main.h"

#include <cerrno>
#include <cctype>
#include <initializer_list>
#include <optional>

namespace {

constexpr std::size_t kMaxPackageQueryLength = 256;
constexpr std::size_t kMaxCapturedOutput = 2 * 1024 * 1024;

enum class PackageManager {
    Apt,
    Zypper,
    Dnf,
    Unknown
};

struct Options {
    bool showHelp = false;
    bool showVersion = false;
    std::vector<std::string> packages;
};

struct ParseResult {
    Options options;
    std::string error;
};

struct ProcessResult {
    int exitCode = 127;
    std::string output;
};

struct NativeHealth {
    bool checked = false;
    bool ok = true;
    std::string warning;
};

struct AppContext {
    PackageManager packageManager = PackageManager::Unknown;
    bool hasNative = false;
    bool hasFlatpak = false;
    bool hasSnap = false;
    NativeHealth nativeHealth;
};

struct AptInfo {
    std::string name;
    std::string version;
    std::string priority;
    std::string section;
    std::string depends;
    std::string recommends;
    std::string homepage;
    std::string description;
    bool installed = false;
};

struct RpmInfo {
    std::string name;
    std::string version;
    std::string release;
    std::string arch;
    std::string summary;
    std::string homepage;
    std::string group;
    std::string vendor;
    std::string installedSize;
    std::string repo;
    std::string license;
    std::string description;
    std::optional<bool> installed;
};

struct FlatpakInfo {
    std::string appId;
    std::string version;
    std::string description;
    std::string homepage;
    bool installed = false;
};

struct SnapInfo {
    std::string name;
    std::string version;
    std::string stableVersion;
    std::string publisher;
    std::string homepage;
    std::string summary;
    std::string description;
    bool installed = false;
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

std::string ltrim(const std::string& input) {
    const auto begin = input.find_first_not_of(" \t\r\n");
    return begin == std::string::npos ? std::string {} : input.substr(begin);
}

std::string rtrim(const std::string& input) {
    const auto end = input.find_last_not_of(" \t\r\n");
    return end == std::string::npos ? std::string {} : input.substr(0, end + 1);
}

std::string trim(const std::string& input) {
    return rtrim(ltrim(input));
}

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::stringstream stream(text);
    std::string line;

    while (std::getline(stream, line)) {
        lines.push_back(rtrim(line));
    }

    return lines;
}

std::vector<std::string> splitTabs(const std::string& text) {
    std::vector<std::string> columns;
    std::size_t start = 0;

    for (;;) {
        const std::size_t tab = text.find('\t', start);
        if (tab == std::string::npos) {
            columns.push_back(text.substr(start));
            break;
        }

        columns.push_back(text.substr(start, tab - start));
        start = tab + 1;
    }

    return columns;
}

bool isValidPackageQuery(const std::string& value) {
    if (value.empty() || value.size() > kMaxPackageQueryLength) {
        return false;
    }
    if (!std::isalnum(static_cast<unsigned char>(value.front()))) {
        return false;
    }

    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '-' ||
               c == '_' || c == '+' || c == ':';
    });
}

bool executableAt(const std::string& path) {
    return !path.empty() && access(path.c_str(), X_OK) == 0;
}

bool commandExists(const std::string& command) {
    if (command.find('/') != std::string::npos) {
        return executableAt(command);
    }

    const char* pathEnv = getenv("PATH");
    const std::string path = pathEnv != nullptr
        ? pathEnv
        : "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";

    std::stringstream stream(path);
    std::string directory;
    while (std::getline(stream, directory, ':')) {
        if (directory.empty()) {
            directory = ".";
        }
        if (executableAt(directory + "/" + command)) {
            return true;
        }
    }

    return false;
}

bool setCloseOnExec(int fd) {
    const int flags = fcntl(fd, F_GETFD);
    return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

int decodeExitStatus(int status) {
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

    for (;;) {
        const pid_t result = waitpid(pid, &status, 0);
        if (result == pid) {
            return decodeExitStatus(status);
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return 127;
    }
}

void redirectToDevNull(int targetFd) {
    FileDescriptor nullFd(open("/dev/null", O_RDWR | O_CLOEXEC));
    if (nullFd.valid()) {
        dup2(nullFd.get(), targetFd);
    }
}

void appendCaptured(std::string& output, const char* data, std::size_t size) {
    if (output.size() >= kMaxCapturedOutput) {
        return;
    }

    const std::size_t remaining = kMaxCapturedOutput - output.size();
    output.append(data, std::min(size, remaining));
}

ProcessResult runProcess(const std::vector<std::string>& args, bool captureStdout) {
    if (args.empty() || args.front().empty()) {
        return {};
    }

    int pipeFd[2] = {-1, -1};
    FileDescriptor readEnd;
    FileDescriptor writeEnd;

    if (captureStdout) {
        if (pipe(pipeFd) != 0) {
            return {};
        }
        setCloseOnExec(pipeFd[0]);
        setCloseOnExec(pipeFd[1]);
        readEnd.reset(pipeFd[0]);
        writeEnd.reset(pipeFd[1]);
    }

    const pid_t pid = fork();
    if (pid < 0) {
        return {};
    }

    if (pid == 0) {
        if (captureStdout) {
            readEnd.reset();
            if (dup2(writeEnd.get(), STDOUT_FILENO) < 0) {
                _exit(127);
            }
        } else {
            redirectToDevNull(STDOUT_FILENO);
        }
        redirectToDevNull(STDERR_FILENO);

        setenv("LC_ALL", "C", 1);
        setenv("LANG", "C", 1);
        setenv("LANGUAGE", "C", 1);

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const std::string& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execvp(argv[0], argv.data());
        _exit(errno == ENOENT ? 127 : 126);
    }

    if (captureStdout) {
        writeEnd.reset();
    }

    ProcessResult result;
    if (captureStdout && readEnd.valid()) {
        std::array<char, 4096> buffer {};

        for (;;) {
            const ssize_t count = read(readEnd.get(), buffer.data(), buffer.size());
            if (count > 0) {
                appendCaptured(result.output, buffer.data(), static_cast<std::size_t>(count));
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
    }

    result.exitCode = waitForChild(pid);
    return result;
}

ProcessResult capture(const std::vector<std::string>& args) {
    return runProcess(args, true);
}

ProcessResult runQuiet(const std::vector<std::string>& args) {
    return runProcess(args, false);
}

std::string firstToken(const std::string& value) {
    std::istringstream stream(value);
    std::string token;
    stream >> token;
    return token;
}

std::string normalizeKey(const std::string& key) {
    std::string result;
    bool pendingSpace = false;

    for (unsigned char c : trim(key)) {
        if (std::isspace(c)) {
            pendingSpace = true;
            continue;
        }

        if (pendingSpace && !result.empty()) {
            result += ' ';
        }
        result += static_cast<char>(std::tolower(c));
        pendingSpace = false;
    }

    return result;
}

bool keyIs(const std::string& key, std::initializer_list<const char*> names) {
    return std::any_of(names.begin(), names.end(), [&](const char* name) {
        return key == name;
    });
}

bool looksLikeFieldLine(const std::string& line) {
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) {
        return false;
    }

    const std::string key = trim(line.substr(0, colon));
    if (key.empty()) {
        return false;
    }

    return std::all_of(key.begin(), key.end(), [](unsigned char c) {
        return std::isalnum(c) || std::isspace(c) ||
               c == '-' || c == '_' || c == '.' || c == '/';
    });
}

void appendDescriptionLine(std::string& description, const std::string& line) {
    const std::string cleaned = trim(line);
    if (cleaned.empty()) {
        if (!description.empty() && description.back() != '\n') {
            description += '\n';
        }
        return;
    }

    if (!description.empty() && description.back() != '\n') {
        description += '\n';
    }
    description += cleaned;
    description += '\n';
}

std::string fieldValue(const std::map<std::string, std::string>& fields,
                       const std::string& key) {
    const auto found = fields.find(key);
    return found == fields.end() ? std::string {} : found->second;
}

std::map<std::string, std::string> parseDebControlStanza(const std::string& output) {
    std::map<std::string, std::string> fields;
    std::string currentKey;

    for (const std::string& line : splitLines(output)) {
        if (line.empty()) {
            if (!fields.empty()) {
                break;
            }
            currentKey.clear();
            continue;
        }

        if ((line.front() == ' ' || line.front() == '\t') && !currentKey.empty()) {
            std::string value = trim(line);
            std::string& target = fields[currentKey];

            if (value == ".") {
                if (!target.empty() && target.back() != '\n') {
                    target += '\n';
                }
            } else {
                if (!target.empty() && target.back() != '\n') {
                    target += '\n';
                }
                target += value;
            }
            continue;
        }

        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }

        currentKey = normalizeKey(line.substr(0, colon));
        fields[currentKey] = trim(line.substr(colon + 1));
    }

    return fields;
}

PackageManager parsePackageManager(const std::string& value) {
    if (value == "apt") {
        return PackageManager::Apt;
    }
    if (value == "zypper") {
        return PackageManager::Zypper;
    }
    if (value == "dnf") {
        return PackageManager::Dnf;
    }
    return PackageManager::Unknown;
}

std::string nativeCommand(PackageManager packageManager) {
    switch (packageManager) {
        case PackageManager::Apt:
            return "apt";
        case PackageManager::Zypper:
            return "zypper";
        case PackageManager::Dnf:
            return "dnf";
        case PackageManager::Unknown:
            return {};
    }
    return {};
}

std::string nativeLabel(PackageManager packageManager) {
    switch (packageManager) {
        case PackageManager::Apt:
            return "APT";
        case PackageManager::Zypper:
            return "ZYPPER";
        case PackageManager::Dnf:
            return "DNF";
        case PackageManager::Unknown:
            return "Native";
    }
    return "Native";
}

std::string nativeRepairHint(PackageManager packageManager) {
    switch (packageManager) {
        case PackageManager::Apt:
            return "Try: sudo dpkg --configure -a";
        case PackageManager::Zypper:
        case PackageManager::Dnf:
            return "Try: sudo rpm --rebuilddb";
        case PackageManager::Unknown:
            return {};
    }
    return {};
}

NativeHealth checkNativeHealth(PackageManager packageManager) {
    NativeHealth health;

    if (packageManager == PackageManager::Unknown) {
        return health;
    }

    health.checked = true;

    if (packageManager == PackageManager::Apt) {
        if (!commandExists("dpkg-query")) {
            health.ok = false;
            health.warning = "dpkg-query was not found; native package metadata may be incomplete.";
            return health;
        }

        const ProcessResult result = runQuiet({"dpkg-query", "-W"});
        if (result.exitCode != 0) {
            health.ok = false;
            health.warning = "APT/dpkg package database health check failed.";
        }
        return health;
    }

    if (packageManager == PackageManager::Zypper || packageManager == PackageManager::Dnf) {
        if (!commandExists("rpm")) {
            health.ok = false;
            health.warning = "rpm was not found; native package metadata may be incomplete.";
            return health;
        }

        const ProcessResult result = runQuiet({"rpm", "-qa"});
        if (result.exitCode != 0) {
            health.ok = false;
            health.warning = "RPM package database health check failed.";
        }
    }

    return health;
}

void printNativeHealthWarning(const AppContext& context) {
    if (!context.nativeHealth.checked || context.nativeHealth.ok) {
        return;
    }

    std::cerr << YELLOW << "Warning: " << context.nativeHealth.warning << RESET << "\n";

    const std::string hint = nativeRepairHint(context.packageManager);
    if (!hint.empty()) {
        std::cerr << "         " << hint << "\n";
    }
}

bool parseBoolValue(const std::string& value) {
    const std::string lower = toLower(trim(value));
    return lower == "yes" || lower == "true" || lower == "1" || lower == "installed";
}

bool aptPackageInstalled(const std::string& package) {
    if (!commandExists("dpkg-query")) {
        return false;
    }

    const ProcessResult result = capture({
        "dpkg-query",
        "-W",
        "-f=${db:Status-Abbrev}",
        package
    });

    return result.exitCode == 0 && startsWith(trim(result.output), "ii");
}

std::optional<AptInfo> readAptInfo(const std::string& package) {
    if (!commandExists("apt")) {
        return std::nullopt;
    }

    const ProcessResult result = capture({"apt", "show", package});
    if (result.output.empty()) {
        return std::nullopt;
    }

    const std::map<std::string, std::string> fields = parseDebControlStanza(result.output);
    AptInfo info;
    info.name = fieldValue(fields, "package");
    if (info.name.empty()) {
        return std::nullopt;
    }

    info.version = fieldValue(fields, "version");
    info.priority = fieldValue(fields, "priority");
    info.section = fieldValue(fields, "section");
    info.depends = fieldValue(fields, "depends");
    info.recommends = fieldValue(fields, "recommends");
    info.homepage = fieldValue(fields, "homepage");
    info.description = fieldValue(fields, "description");
    info.installed = aptPackageInstalled(package);
    return info;
}

std::string rpmDescriptionContinuation(const std::string& line) {
    const std::size_t colon = line.find(':');
    if (colon != std::string::npos && trim(line.substr(0, colon)).empty()) {
        return line.substr(colon + 1);
    }
    return line;
}

RpmInfo parseRpmInfo(const std::string& output) {
    RpmInfo info;
    bool inDescription = false;

    for (const std::string& line : splitLines(output)) {
        if (inDescription) {
            if (looksLikeFieldLine(line)) {
                inDescription = false;
            } else {
                appendDescriptionLine(info.description, rpmDescriptionContinuation(line));
                continue;
            }
        }

        if (line.empty()) {
            continue;
        }

        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }

        const std::string key = normalizeKey(line.substr(0, colon));
        const std::string value = trim(line.substr(colon + 1));

        if (key == "name") {
            info.name = value;
        } else if (key == "version") {
            info.version = value;
        } else if (key == "release") {
            info.release = value;
        } else if (keyIs(key, {"arch", "architecture"})) {
            info.arch = value;
        } else if (key == "summary") {
            info.summary = value;
        } else if (keyIs(key, {"url", "homepage", "upstream url"})) {
            info.homepage = value;
        } else if (key == "group") {
            info.group = value;
        } else if (key == "vendor") {
            info.vendor = value;
        } else if (keyIs(key, {"installed size", "size"})) {
            info.installedSize = value;
        } else if (keyIs(key, {"repository", "repo", "from repo"})) {
            info.repo = value;
        } else if (key == "license") {
            info.license = value;
        } else if (key == "installed") {
            info.installed = parseBoolValue(value);
        } else if (key == "description") {
            info.description.clear();
            appendDescriptionLine(info.description, value);
            inDescription = true;
        }
    }

    info.description = trim(info.description);
    return info;
}

bool rpmPackageInstalled(const std::string& package, const RpmInfo& info) {
    if (commandExists("rpm")) {
        return runQuiet({"rpm", "-q", package}).exitCode == 0;
    }
    return info.installed.value_or(false);
}

std::optional<RpmInfo> readRpmInfo(const std::string& package,
                                   PackageManager packageManager) {
    std::vector<std::string> args;
    if (packageManager == PackageManager::Zypper) {
        if (!commandExists("zypper")) {
            return std::nullopt;
        }
        args = {"zypper", "--no-refresh", "--non-interactive", "info", package};
    } else if (packageManager == PackageManager::Dnf) {
        if (!commandExists("dnf")) {
            return std::nullopt;
        }
        args = {"dnf", "info", package};
    } else {
        return std::nullopt;
    }

    const ProcessResult result = capture(args);
    if (result.output.empty()) {
        return std::nullopt;
    }

    RpmInfo info = parseRpmInfo(result.output);
    if (info.name.empty()) {
        return std::nullopt;
    }

    info.installed = rpmPackageInstalled(package, info);
    return info;
}

int flatpakMatchScore(const std::string& appId, const std::string& query) {
    const std::string appLower = toLower(appId);
    const std::string queryLower = toLower(query);
    const std::size_t dot = appLower.rfind('.');
    const std::string tail = dot == std::string::npos ? appLower : appLower.substr(dot + 1);

    if (appLower == queryLower || tail == queryLower) {
        return 4;
    }
    if (queryLower.size() >= 2 &&
        (startsWith(appLower, queryLower) || startsWith(tail, queryLower))) {
        return 3;
    }
    if (appLower.find("." + queryLower) != std::string::npos) {
        return 2;
    }
    return 0;
}

std::optional<FlatpakInfo> readFlatpakSearchInfo(const std::string& package) {
    const ProcessResult result = capture({
        "flatpak",
        "search",
        "--columns=application,version,description",
        package
    });

    if (result.output.empty()) {
        return std::nullopt;
    }

    FlatpakInfo best;
    int bestScore = 0;

    for (const std::string& line : splitLines(result.output)) {
        if (trim(line).empty()) {
            continue;
        }

        const std::vector<std::string> columns = splitTabs(line);
        if (columns.empty()) {
            continue;
        }

        const std::string appId = trim(columns[0]);
        if (appId.empty()) {
            continue;
        }

        const int score = flatpakMatchScore(appId, package);
        if (score <= bestScore) {
            continue;
        }

        bestScore = score;
        best.appId = appId;
        best.version = columns.size() > 1 ? trim(columns[1]) : std::string {};
        best.description = columns.size() > 2 ? trim(columns[2]) : std::string {};
    }

    if (bestScore == 0) {
        return std::nullopt;
    }

    return best;
}

bool flatpakInstalled(const std::string& appId) {
    const ProcessResult result = capture({"flatpak", "list", "--columns=application"});
    if (result.output.empty()) {
        return false;
    }

    for (const std::string& line : splitLines(result.output)) {
        if (trim(line) == appId) {
            return true;
        }
    }

    return false;
}

std::string parseKeyValueFromOutput(const std::string& output,
                                    std::initializer_list<const char*> acceptedKeys) {
    for (const std::string& line : splitLines(output)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }

        const std::string key = normalizeKey(line.substr(0, colon));
        if (keyIs(key, acceptedKeys)) {
            return trim(line.substr(colon + 1));
        }
    }

    return {};
}

std::optional<FlatpakInfo> readFlatpakInfo(const std::string& package) {
    if (!commandExists("flatpak")) {
        return std::nullopt;
    }

    std::optional<FlatpakInfo> info = readFlatpakSearchInfo(package);
    if (!info.has_value()) {
        return std::nullopt;
    }

    info->installed = flatpakInstalled(info->appId);
    if (info->installed) {
        const ProcessResult details = capture({"flatpak", "info", info->appId});
        info->homepage = parseKeyValueFromOutput(details.output, {"url", "homepage", "website"});
    }

    return info;
}

SnapInfo parseSnapInfo(const std::string& output) {
    SnapInfo info;
    bool inDescription = false;

    for (const std::string& line : splitLines(output)) {
        if (inDescription) {
            if (looksLikeFieldLine(line) && !std::isspace(static_cast<unsigned char>(line.front()))) {
                inDescription = false;
            } else {
                appendDescriptionLine(info.description, line);
                continue;
            }
        }

        if (line.empty()) {
            continue;
        }

        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }

        const std::string key = normalizeKey(line.substr(0, colon));
        const std::string value = trim(line.substr(colon + 1));

        if (key == "name") {
            info.name = value;
        } else if (key == "summary") {
            info.summary = value;
        } else if (key == "publisher") {
            info.publisher = value;
        } else if (keyIs(key, {"store-url", "website"})) {
            if (info.homepage.empty()) {
                info.homepage = value;
            }
        } else if (key == "contact") {
            if (info.homepage.empty() && startsWith(value, "http")) {
                info.homepage = value;
            }
        } else if (key == "description") {
            info.description.clear();
            if (value != "|" && value != ">") {
                appendDescriptionLine(info.description, value);
            }
            inDescription = true;
        } else if (key == "installed") {
            info.version = firstToken(value);
            info.installed = !info.version.empty();
        } else if (key == "stable" || key == "latest/stable") {
            info.stableVersion = firstToken(value);
        }
    }

    info.description = trim(info.description);
    if (info.version.empty()) {
        info.version = info.stableVersion;
    }

    return info;
}

bool snapInstalled(const std::string& package) {
    const ProcessResult result = capture({"snap", "list", package});
    if (result.exitCode != 0 || result.output.empty()) {
        return false;
    }

    bool sawHeader = false;
    for (const std::string& line : splitLines(result.output)) {
        const std::string cleaned = trim(line);
        if (cleaned.empty()) {
            continue;
        }
        if (!sawHeader) {
            sawHeader = true;
            continue;
        }
        return true;
    }

    return false;
}

std::optional<SnapInfo> readSnapInfo(const std::string& package) {
    if (!commandExists("snap")) {
        return std::nullopt;
    }

    const ProcessResult result = capture({"snap", "info", package});
    if (result.output.empty()) {
        return std::nullopt;
    }

    SnapInfo info = parseSnapInfo(result.output);
    if (info.name.empty()) {
        return std::nullopt;
    }

    info.installed = info.installed || snapInstalled(package);
    return info;
}

void printVersion() {
    std::cout << RED << "zinfo component version: v" << zpm_version::version()
              << " of ZPM\n" << RESET;
    std::cout << "https://github.com/Zielina-Konrad-productions/ZPM\n";
    std::cout << "Copyright (c) 2026 Ignacyyy & Ry3ball\nLicense: MIT\n";
}

void printHelp(const char* programName) {
    std::cout << RED << "Usage: " << RESET << programName << " <package> [package...] [options] or zpm info <package> [options]\n";
    std::cout << RED << "Options:\n" << RESET;
    std::cout << "  Finds package metadata in native PM / Flatpak / Snap.\n";
    std::cout << "  --version, -v   Show version information\n";
    std::cout << "  --help,    -h   Show this help message\n";
}

void printHeader(const std::string& source,
                 const std::string& name,
                 const std::string& version,
                 bool installed) {
    std::cout << YELLOW << "[" << source << "] " << GREEN << name << RESET;
    if (!version.empty()) {
        std::cout << CYAN << " (" << version << ")" << RESET;
    }
    std::cout << BLUE << (installed ? " [✓ Installed]" : " [ ] Not installed")
              << RESET << "\n\n";
}

void printSection(const std::string& marker, const std::string& title) {
    std::cout << YELLOW << "[" << marker << "] " << GREEN << title << "\n" << RESET;
}

bool printField(const std::string& label, const std::string& value) {
    if (value.empty()) {
        return false;
    }
    std::cout << "  " << label << " : " << value << "\n";
    return true;
}

void printDescription(const std::string& description) {
    if (description.empty()) {
        return;
    }

    printSection("d", "Description");
    std::cout << description << "\n";
    std::cout << "----------------------------------------\n\n";
}

bool showAptPackageInfo(const std::string& package) {
    const std::optional<AptInfo> info = readAptInfo(package);
    if (!info.has_value()) {
        return false;
    }

    printHeader("APT", info->name, info->version, info->installed);

    bool printedBasic = false;
    if (!info->priority.empty() || !info->section.empty()) {
        printSection("I", "Basic info");
        printedBasic = printField("Priority", info->priority) || printedBasic;
        printedBasic = printField("Section ", info->section) || printedBasic;
        if (printedBasic) {
            std::cout << "\n";
        }
    }

    if (!info->depends.empty() || !info->recommends.empty()) {
        printSection("D", "Dependencies");
        printField("Depends   ", info->depends);
        printField("Recommends", info->recommends);
        std::cout << "\n";
    }

    if (!info->homepage.empty()) {
        printSection("H", "Homepage");
        std::cout << "  " << info->homepage << "\n\n";
    }

    printDescription(info->description);
    return true;
}

std::string rpmFullVersion(const RpmInfo& info) {
    std::string version = info.version;
    if (!info.release.empty()) {
        version += version.empty() ? info.release : "-" + info.release;
    }
    if (!info.arch.empty()) {
        version += version.empty() ? info.arch : "." + info.arch;
    }
    return version;
}

bool showRpmPackageInfo(const std::string& package, PackageManager packageManager) {
    const std::optional<RpmInfo> info = readRpmInfo(package, packageManager);
    if (!info.has_value()) {
        return false;
    }

    printHeader(nativeLabel(packageManager), info->name, rpmFullVersion(*info),
                info->installed.value_or(false));

    if (!info->summary.empty() || !info->group.empty() || !info->repo.empty() ||
        !info->vendor.empty() || !info->installedSize.empty() || !info->license.empty()) {
        printSection("I", "Basic info");
        printField("Summary", info->summary);
        printField("Group  ", info->group);
        printField("Repo   ", info->repo);
        printField("Vendor ", info->vendor);
        printField("Size   ", info->installedSize);
        printField("License", info->license);
        std::cout << "\n";
    }

    if (!info->homepage.empty()) {
        printSection("H", "Homepage");
        std::cout << "  " << info->homepage << "\n\n";
    }

    printDescription(info->description);
    return true;
}

bool showFlatpakPackageInfo(const std::string& package) {
    const std::optional<FlatpakInfo> info = readFlatpakInfo(package);
    if (!info.has_value()) {
        return false;
    }

    printHeader("FLATPAK", info->appId, info->version, info->installed);

    if (!info->homepage.empty()) {
        printSection("H", "Homepage");
        std::cout << "  " << info->homepage << "\n\n";
    }

    printDescription(info->description);
    return true;
}

bool showSnapPackageInfo(const std::string& package) {
    const std::optional<SnapInfo> info = readSnapInfo(package);
    if (!info.has_value()) {
        return false;
    }

    printHeader("SNAP", info->name, info->version, info->installed);

    if (!info->publisher.empty() || !info->summary.empty()) {
        printSection("I", "Basic info");
        printField("Publisher", info->publisher);
        printField("Summary  ", info->summary);
        std::cout << "\n";
    }

    if (!info->homepage.empty()) {
        printSection("H", "Homepage");
        std::cout << "  " << info->homepage << "\n\n";
    }

    printDescription(info->description);
    return true;
}

std::string checkedSources(const AppContext& context) {
    std::vector<std::string> sources;

    if (context.hasNative) {
        sources.push_back(nativeLabel(context.packageManager));
    }
    if (context.hasFlatpak) {
        sources.push_back("Flatpak");
    }
    if (context.hasSnap) {
        sources.push_back("Snap");
    }

    if (sources.empty()) {
        return "no supported backends";
    }

    std::string output;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (i > 0) {
            output += i + 1 == sources.size() ? " and " : ", ";
        }
        output += sources[i];
    }

    if (context.packageManager == PackageManager::Unknown) {
        output += "; native package manager was not detected";
    }

    return output;
}

bool showPackageInfo(const std::string& package, const AppContext& context) {
    bool found = false;

    if (context.hasNative) {
        if (context.packageManager == PackageManager::Apt) {
            found = showAptPackageInfo(package) || found;
        } else if (context.packageManager == PackageManager::Zypper ||
                   context.packageManager == PackageManager::Dnf) {
            found = showRpmPackageInfo(package, context.packageManager) || found;
        }
    }

    if (context.hasFlatpak) {
        found = showFlatpakPackageInfo(package) || found;
    }
    if (context.hasSnap) {
        found = showSnapPackageInfo(package) || found;
    }

    if (!found) {
        std::cerr << RED << "No package information found for '" << package << "'"
                  << " (checked " << checkedSources(context) << ").\n" << RESET;
    }

    return found;
}

ParseResult parseArgs(int argc, char* argv[]) {
    ParseResult result;
    bool stopOptions = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (!stopOptions && arg == "--") {
            stopOptions = true;
            continue;
        }
        if (!stopOptions && (arg == "--help" || arg == "-h")) {
            result.options.showHelp = true;
            continue;
        }
        if (!stopOptions && (arg == "--version" || arg == "-v")) {
            result.options.showVersion = true;
            continue;
        }
        if (!stopOptions && startsWith(arg, "-")) {
            result.error = "Unknown option: " + arg;
            return result;
        }

        result.options.packages.push_back(arg);
    }

    if (!result.options.showHelp && !result.options.showVersion) {
        for (const std::string& package : result.options.packages) {
            if (!isValidPackageQuery(package)) {
                result.error = "Invalid package name: " + package;
                return result;
            }
        }
    }

    return result;
}

AppContext makeContext() {
    AppContext context;
    context.packageManager = parsePackageManager(get_package_manager());
    const std::string native = nativeCommand(context.packageManager);
    context.hasNative = !native.empty() && commandExists(native);
    context.hasFlatpak = commandExists("flatpak");
    context.hasSnap = commandExists("snap");
    if (context.hasNative) {
        context.nativeHealth = checkNativeHealth(context.packageManager);
    }
    return context;
}

} // namespace

int main(int argc, char* argv[]) {
    const ParseResult parsed = parseArgs(argc, argv);
    if (!parsed.error.empty()) {
        std::cerr << RED << "Error: " << parsed.error << "\n" << RESET;
        return 1;
    }

    const Options& options = parsed.options;

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

    if (options.packages.empty()) {
        std::cerr << YELLOW << "No package specified.\n" << RESET;
        return 1;
    }

    zpm_update::checkForUpdates();

    const AppContext context = makeContext();
    printNativeHealthWarning(context);

    bool allFound = true;

    for (const std::string& package : options.packages) {
        allFound = showPackageInfo(package, context) && allFound;
    }

    return allFound ? 0 : 1;
}
