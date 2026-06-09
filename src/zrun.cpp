// zrun.cpp — part of ZPM
// Launch an installed application from native PM, Flatpak, or Snap.

#include "main.h"

#include <cerrno>
#include <cctype>
#include <cstring>
#include <optional>
#include <poll.h>
#include <termios.h>
#include <utility>

namespace {

constexpr std::size_t kMaxPackageQueryLength = 256;
constexpr std::size_t kNonInteractiveListLimit = 40;
constexpr std::size_t kMinPrefixQueryLength = 2;
constexpr std::size_t kMinContainsQueryLength = 3;

enum class Source {
    Native,
    Flatpak,
    Snap
};

enum class SelectionStatus {
    Selected,
    Cancelled,
    Unavailable
};

struct Options {
    bool showHelp = false;
    bool showVersion = false;
    std::string package;
};

struct ParseResult {
    Options options;
    std::string error;
};

struct ProcessResult {
    int exitCode = 127;
    std::string output;
};

struct LaunchResult {
    bool started = false;
    int errorNumber = 0;
};

struct MenuItem {
    std::string label;
    Source source = Source::Native;
    std::string sourceLabel;
    std::string packageName;
    std::string commandDisplay;
    std::vector<std::vector<std::string>> launchCommands;
};

struct SelectionResult {
    SelectionStatus status = SelectionStatus::Cancelled;
    std::size_t index = 0;
};

struct MatchBuckets {
    std::vector<std::string> exact;
    std::vector<std::string> prefix;
    std::vector<std::string> contains;
};

struct DesktopApp {
    std::string id;
    std::string name;
    std::string genericName;
    std::string commandDisplay;
    std::vector<std::string> execArgs;
};

struct DesktopBuckets {
    std::vector<DesktopApp> exact;
    std::vector<DesktopApp> prefix;
    std::vector<DesktopApp> contains;
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

class TerminalModeGuard {
public:
    explicit TerminalModeGuard(int fd) : fd_(fd) {
        if (tcgetattr(fd_, &original_) != 0) {
            return;
        }

        termios raw = original_;
        raw.c_iflag &= ~(IXON | ICRNL);
        raw.c_lflag &= ~(ECHO | ICANON | ISIG);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;

        active_ = (tcsetattr(fd_, TCSAFLUSH, &raw) == 0);
    }

    TerminalModeGuard(const TerminalModeGuard&) = delete;
    TerminalModeGuard& operator=(const TerminalModeGuard&) = delete;

    ~TerminalModeGuard() {
        if (active_) {
            tcsetattr(fd_, TCSAFLUSH, &original_);
        }
    }

    bool active() const {
        return active_;
    }

private:
    int fd_ = -1;
    bool active_ = false;
    termios original_ {};
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
        lines.push_back(line);
    }

    return lines;
}

std::string baseName(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string removeSuffix(const std::string& value, const std::string& suffix) {
    if (value.size() < suffix.size()) {
        return value;
    }
    if (value.compare(value.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return value;
    }
    return value.substr(0, value.size() - suffix.size());
}

std::string desktopIdFromPath(const std::filesystem::path& path) {
    return removeSuffix(path.filename().string(), ".desktop");
}

bool parseDesktopBool(const std::string& value) {
    const std::string lower = toLower(trim(value));
    return lower == "true" || lower == "1" || lower == "yes";
}

bool isValidPackageQuery(const std::string& value) {
    if (value.empty() || value.size() > kMaxPackageQueryLength) {
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

std::optional<std::string> cleanDesktopExecToken(const std::string& token) {
    std::string cleaned;
    cleaned.reserve(token.size());

    for (std::size_t i = 0; i < token.size(); ++i) {
        if (token[i] != '%') {
            cleaned += token[i];
            continue;
        }

        if (i + 1 < token.size() && token[i + 1] == '%') {
            cleaned += '%';
            ++i;
            continue;
        }

        return std::nullopt;
    }

    return cleaned;
}

std::vector<std::string> parseDesktopExec(const std::string& execLine) {
    std::vector<std::string> tokens;
    std::string token;
    bool inQuotes = false;
    bool escaping = false;

    for (char c : execLine) {
        if (escaping) {
            token += c;
            escaping = false;
            continue;
        }

        if (c == '\\') {
            escaping = true;
            continue;
        }

        if (c == '"') {
            inQuotes = !inQuotes;
            continue;
        }

        if (!inQuotes && std::isspace(static_cast<unsigned char>(c))) {
            if (!token.empty()) {
                if (const auto cleaned = cleanDesktopExecToken(token)) {
                    if (!cleaned->empty()) {
                        tokens.push_back(*cleaned);
                    }
                }
                token.clear();
            }
            continue;
        }

        token += c;
    }

    if (escaping) {
        token += '\\';
    }

    if (!token.empty()) {
        if (const auto cleaned = cleanDesktopExecToken(token)) {
            if (!cleaned->empty()) {
                tokens.push_back(*cleaned);
            }
        }
    }

    if (tokens.empty()) {
        return {};
    }

    const std::string& command = tokens.front();
    if (command.find('/') != std::string::npos) {
        if (!executableAt(command)) {
            return {};
        }
    } else if (!commandExists(command)) {
        return {};
    }

    return tokens;
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

void addLaunchCommand(std::vector<std::vector<std::string>>& commands,
                      std::vector<std::string> command) {
    if (command.empty() || command.front().empty()) {
        return;
    }

    if (std::find(commands.begin(), commands.end(), command) == commands.end()) {
        commands.push_back(std::move(command));
    }
}

void addMatch(MatchBuckets& buckets, const std::string& name, const std::string& queryLower) {
    const std::string cleaned = trim(name);
    if (cleaned.empty()) {
        return;
    }

    const std::string lower = toLower(cleaned);
    if (lower == queryLower) {
        addUnique(buckets.exact, cleaned);
    } else if (queryLower.size() >= kMinPrefixQueryLength && startsWith(lower, queryLower)) {
        addUnique(buckets.prefix, cleaned);
    } else if (queryLower.size() >= kMinContainsQueryLength &&
               lower.find(queryLower) != std::string::npos) {
        addUnique(buckets.contains, cleaned);
    }
}

std::vector<std::string> flattenMatches(const MatchBuckets& buckets) {
    std::vector<std::string> result;
    result.reserve(buckets.exact.size() + buckets.prefix.size() + buckets.contains.size());

    for (const auto& bucket : {buckets.exact, buckets.prefix, buckets.contains}) {
        for (const auto& value : bucket) {
            addUnique(result, value);
        }
    }

    return result;
}

void addUniqueDesktop(std::vector<DesktopApp>& apps, const DesktopApp& app) {
    if (app.id.empty()) {
        return;
    }

    const auto exists = std::any_of(apps.begin(), apps.end(), [&](const DesktopApp& existing) {
        return existing.id == app.id;
    });
    if (!exists) {
        apps.push_back(app);
    }
}

void addDesktopMatch(DesktopBuckets& buckets,
                     const DesktopApp& app,
                     const std::string& queryLower) {
    std::vector<std::string> terms {
        app.name,
        app.genericName,
        app.id,
        baseName(app.commandDisplay)
    };

    bool prefixMatch = false;
    bool containsMatch = false;

    for (const std::string& rawTerm : terms) {
        const std::string term = toLower(trim(rawTerm));
        if (term.empty()) {
            continue;
        }

        if (term == queryLower) {
            addUniqueDesktop(buckets.exact, app);
            return;
        }
        if (queryLower.size() >= kMinPrefixQueryLength && startsWith(term, queryLower)) {
            prefixMatch = true;
        } else if (queryLower.size() >= kMinContainsQueryLength &&
                   term.find(queryLower) != std::string::npos) {
            containsMatch = true;
        }
    }

    if (prefixMatch) {
        addUniqueDesktop(buckets.prefix, app);
    } else if (containsMatch) {
        addUniqueDesktop(buckets.contains, app);
    }
}

std::vector<DesktopApp> flattenDesktopMatches(const DesktopBuckets& buckets) {
    std::vector<DesktopApp> result;
    result.reserve(buckets.exact.size() + buckets.prefix.size() + buckets.contains.size());

    for (const auto& bucket : {buckets.exact, buckets.prefix, buckets.contains}) {
        for (const DesktopApp& app : bucket) {
            addUniqueDesktop(result, app);
        }
    }

    return result;
}

bool setCloseOnExec(int fd) {
    const int flags = fcntl(fd, F_GETFD);
    return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

bool writeAll(int fd, const void* data, std::size_t size) {
    const char* cursor = static_cast<const char*>(data);

    while (size > 0) {
        const ssize_t written = write(fd, cursor, size);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        cursor += written;
        size -= static_cast<std::size_t>(written);
    }

    return true;
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
            dup2(writeEnd.get(), STDOUT_FILENO);
        } else {
            redirectToDevNull(STDOUT_FILENO);
        }
        redirectToDevNull(STDERR_FILENO);

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const std::string& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execvp(argv[0], argv.data());
        _exit(127);
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
                result.output.append(buffer.data(), static_cast<std::size_t>(count));
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
    result.output = trim(result.output);
    return result;
}

ProcessResult capture(const std::vector<std::string>& args) {
    return runProcess(args, true);
}

std::optional<int> readExecFailure(int fd) {
    int errorNumber = 0;
    std::size_t received = 0;
    char* cursor = reinterpret_cast<char*>(&errorNumber);

    while (received < sizeof(errorNumber)) {
        const ssize_t count = read(fd, cursor + received, sizeof(errorNumber) - received);
        if (count > 0) {
            received += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0) {
            return std::nullopt;
        }
        if (errno == EINTR) {
            continue;
        }
        return errno;
    }

    return errorNumber;
}

LaunchResult launchDetached(const std::vector<std::vector<std::string>>& candidates) {
    if (candidates.empty()) {
        return {false, EINVAL};
    }

    int pipeFd[2] = {-1, -1};
    if (pipe(pipeFd) != 0) {
        return {false, errno};
    }

    setCloseOnExec(pipeFd[0]);
    setCloseOnExec(pipeFd[1]);

    FileDescriptor readEnd(pipeFd[0]);
    FileDescriptor writeEnd(pipeFd[1]);

    const pid_t pid = fork();
    if (pid < 0) {
        return {false, errno};
    }

    if (pid == 0) {
        readEnd.reset();
        setsid();
        redirectToDevNull(STDIN_FILENO);
        redirectToDevNull(STDOUT_FILENO);
        redirectToDevNull(STDERR_FILENO);

        int lastError = ENOENT;
        for (const auto& args : candidates) {
            if (args.empty() || args.front().empty()) {
                continue;
            }

            std::vector<char*> argv;
            argv.reserve(args.size() + 1);
            for (const std::string& arg : args) {
                argv.push_back(const_cast<char*>(arg.c_str()));
            }
            argv.push_back(nullptr);

            execvp(argv[0], argv.data());
            lastError = errno;
        }

        writeAll(writeEnd.get(), &lastError, sizeof(lastError));
        _exit(127);
    }

    writeEnd.reset();

    const std::optional<int> failure = readExecFailure(readEnd.get());
    if (!failure.has_value()) {
        return {true, 0};
    }

    waitForChild(pid);
    return {false, *failure};
}

std::string nativePmLabel(const std::string& packageManager) {
    if (packageManager == "zypper") {
        return "Zypper";
    }
    if (packageManager == "dnf") {
        return "DNF";
    }
    if (packageManager == "apt") {
        return "APT";
    }
    return "Native";
}

std::string matchedSuffix(const std::string& matchedName, const std::string& query) {
    if (toLower(matchedName) == toLower(query)) {
        return {};
    }

    return "  " + std::string(YELLOW) + "(matched '" + query + "')" + RESET;
}

std::optional<DesktopApp> readDesktopApp(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return std::nullopt;
    }

    bool inDesktopEntry = false;
    std::string type;
    std::string name;
    std::string genericName;
    std::string execLine;
    std::string tryExec;
    bool hidden = false;
    bool noDisplay = false;
    bool terminal = false;

    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            inDesktopEntry = (line == "[Desktop Entry]");
            continue;
        }
        if (!inDesktopEntry) {
            continue;
        }

        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        const std::string key = line.substr(0, separator);
        const std::string value = line.substr(separator + 1);

        if (key == "Type") {
            type = value;
        } else if (key == "Name") {
            name = value;
        } else if (key == "GenericName") {
            genericName = value;
        } else if (key == "Exec") {
            execLine = value;
        } else if (key == "TryExec") {
            tryExec = value;
        } else if (key == "Hidden") {
            hidden = parseDesktopBool(value);
        } else if (key == "NoDisplay") {
            noDisplay = parseDesktopBool(value);
        } else if (key == "Terminal") {
            terminal = parseDesktopBool(value);
        }
    }

    if ((!type.empty() && type != "Application") ||
        hidden ||
        noDisplay ||
        terminal ||
        name.empty() ||
        execLine.empty()) {
        return std::nullopt;
    }

    if (!tryExec.empty() && !commandExists(tryExec)) {
        return std::nullopt;
    }

    std::vector<std::string> execArgs = parseDesktopExec(execLine);
    if (execArgs.empty()) {
        return std::nullopt;
    }

    DesktopApp app;
    app.id = desktopIdFromPath(path);
    app.name = name;
    app.genericName = genericName;
    app.commandDisplay = execArgs.front();
    app.execArgs = std::move(execArgs);
    return app;
}

std::vector<DesktopApp> nativeFindDesktopApps(const std::string& query) {
    const std::array<std::filesystem::path, 2> directories {
        "/usr/share/applications",
        "/usr/local/share/applications"
    };

    std::vector<DesktopApp> apps;
    for (const auto& directory : directories) {
        std::error_code error;
        if (!std::filesystem::is_directory(directory, error)) {
            continue;
        }

        for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
            if (error) {
                break;
            }
            if (!entry.is_regular_file(error) || entry.path().extension() != ".desktop") {
                continue;
            }

            if (auto app = readDesktopApp(entry.path())) {
                addUniqueDesktop(apps, *app);
            }
        }
    }

    std::sort(apps.begin(), apps.end(), [](const DesktopApp& left, const DesktopApp& right) {
        const std::string leftName = toLower(left.name);
        const std::string rightName = toLower(right.name);
        if (leftName != rightName) {
            return leftName < rightName;
        }
        return left.id < right.id;
    });

    const std::string queryLower = toLower(query);
    DesktopBuckets buckets;
    for (const DesktopApp& app : apps) {
        addDesktopMatch(buckets, app, queryLower);
    }

    return flattenDesktopMatches(buckets);
}

std::vector<std::string> flatpakFindMatches(const std::string& query) {
    const ProcessResult result = capture({"flatpak", "list", "--columns=application"});
    if (result.output.empty()) {
        return {};
    }

    const std::string queryLower = toLower(query);
    MatchBuckets buckets;

    for (const std::string& rawLine : splitLines(result.output)) {
        const std::string appId = trim(rawLine);
        if (appId.empty()) {
            continue;
        }

        const std::string appIdLower = toLower(appId);
        const auto dot = appIdLower.rfind('.');
        const std::string tail = dot == std::string::npos
            ? appIdLower
            : appIdLower.substr(dot + 1);

        if (appIdLower == queryLower || tail == queryLower) {
            addUnique(buckets.exact, appId);
        } else if (queryLower.size() >= kMinPrefixQueryLength &&
                   (startsWith(appIdLower, queryLower) || startsWith(tail, queryLower))) {
            addUnique(buckets.prefix, appId);
        } else if (queryLower.size() >= kMinContainsQueryLength &&
                   appIdLower.find(queryLower) != std::string::npos) {
            addUnique(buckets.contains, appId);
        }
    }

    return flattenMatches(buckets);
}

std::vector<std::string> snapFindMatches(const std::string& query) {
    const ProcessResult result = capture({"snap", "list"});
    if (result.output.empty()) {
        return {};
    }

    const std::string queryLower = toLower(query);
    MatchBuckets buckets;
    bool firstLine = true;

    for (const std::string& rawLine : splitLines(result.output)) {
        const std::string line = trim(rawLine);
        if (line.empty()) {
            continue;
        }

        std::istringstream stream(line);
        std::string name;
        stream >> name;

        const std::string nameLower = toLower(name);
        if (firstLine && (nameLower == "name" || nameLower == "nazwa")) {
            firstLine = false;
            continue;
        }
        firstLine = false;

        addMatch(buckets, name, queryLower);
    }

    return flattenMatches(buckets);
}

std::vector<std::vector<std::string>> snapLaunchCommands(const std::string& package) {
    std::vector<std::vector<std::string>> commands;
    addLaunchCommand(commands, {"/snap/bin/" + package});
    addLaunchCommand(commands, {package});
    return commands;
}

std::vector<MenuItem> findLaunchTargets(const std::string& query,
                                        const std::string& packageManager) {
    std::vector<MenuItem> items;
    const std::string pmLabel = nativePmLabel(packageManager);

    for (const DesktopApp& app : nativeFindDesktopApps(query)) {
        MenuItem item;
        item.source = Source::Native;
        item.sourceLabel = pmLabel;
        item.packageName = app.name;
        item.commandDisplay = app.commandDisplay;
        item.launchCommands = {app.execArgs};
        item.label = pmLabel + ": " + app.name + matchedSuffix(app.name, query);
        items.push_back(std::move(item));
    }

    if (commandExists("flatpak")) {
        for (const std::string& appId : flatpakFindMatches(query)) {
            MenuItem item;
            item.source = Source::Flatpak;
            item.sourceLabel = "Flatpak";
            item.packageName = appId;
            item.commandDisplay = appId;
            item.launchCommands = {{"flatpak", "run", appId}};
            item.label = "Flatpak: " + appId + matchedSuffix(appId, query);
            items.push_back(std::move(item));
        }
    }

    if (commandExists("snap")) {
        for (const std::string& package : snapFindMatches(query)) {
            MenuItem item;
            item.source = Source::Snap;
            item.sourceLabel = "Snap";
            item.packageName = package;
            item.commandDisplay = package;
            item.launchCommands = snapLaunchCommands(package);
            item.label = "Snap:    " + package + matchedSuffix(package, query);
            items.push_back(std::move(item));
        }
    }

    return items;
}

void ensureLaunchCommands(MenuItem& item) {
    if (!item.launchCommands.empty()) {
        return;
    }
}

std::string displayLabel(const MenuItem& item) {
    if (item.source == Source::Native && !item.commandDisplay.empty()) {
        return item.label + "  (" + item.commandDisplay + ")";
    }
    return item.label;
}

std::optional<unsigned char> readByte(int fd, int timeoutMs = -1) {
    if (timeoutMs >= 0) {
        pollfd descriptor {};
        descriptor.fd = fd;
        descriptor.events = POLLIN;

        for (;;) {
            const int ready = poll(&descriptor, 1, timeoutMs);
            if (ready > 0) {
                break;
            }
            if (ready == 0) {
                return std::nullopt;
            }
            if (errno == EINTR) {
                continue;
            }
            return std::nullopt;
        }
    }

    unsigned char c = 0;
    for (;;) {
        const ssize_t count = read(fd, &c, 1);
        if (count == 1) {
            return c;
        }
        if (count == 0) {
            return std::nullopt;
        }
        if (errno == EINTR) {
            continue;
        }
        return std::nullopt;
    }
}

SelectionResult arrowMenu(const std::string& package, const std::vector<MenuItem>& items) {
    if (items.empty()) {
        return {SelectionStatus::Cancelled, 0};
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        return {SelectionStatus::Unavailable, 0};
    }

    TerminalModeGuard terminal(STDIN_FILENO);
    if (!terminal.active()) {
        return {SelectionStatus::Unavailable, 0};
    }

    std::size_t selected = 0;
    const std::size_t count = items.size();
    const std::size_t menuLines = count + 2;

    auto draw = [&](bool first) {
        if (!first) {
            std::cout << "\033[" << menuLines << "A";
        }

        std::cout << "\r\033[K" << BOLD << "Run: " << CYAN << package << RESET << "\n";
        for (std::size_t i = 0; i < count; ++i) {
            if (i == selected) {
                std::cout << "\r\033[K  " << GREEN << "▶ " << BOLD
                          << displayLabel(items[i]) << RESET << "\n";
            } else {
                std::cout << "\r\033[K    " << displayLabel(items[i]) << "\n";
            }
        }
        std::cout << "\r\033[K" << YELLOW
                  << "  ↑↓ move  Enter launch  q quit" << RESET << "\n";
        std::cout << std::flush;
    };

    std::cout << "\n";
    draw(true);

    for (;;) {
        const std::optional<unsigned char> key = readByte(STDIN_FILENO);
        if (!key.has_value()) {
            return {SelectionStatus::Cancelled, 0};
        }

        const unsigned char c = *key;
        if (c == '\r' || c == '\n') {
            return {SelectionStatus::Selected, selected};
        }
        if (c == 'q' || c == 'Q' || c == 3 || c == 4) {
            return {SelectionStatus::Cancelled, 0};
        }

        if (c != '\033') {
            continue;
        }

        const std::optional<unsigned char> first = readByte(STDIN_FILENO, 60);
        if (!first.has_value()) {
            return {SelectionStatus::Cancelled, 0};
        }
        if (*first != '[') {
            continue;
        }

        const std::optional<unsigned char> second = readByte(STDIN_FILENO, 60);
        if (!second.has_value()) {
            continue;
        }

        if (*second == 'A') {
            selected = selected == 0 ? count - 1 : selected - 1;
            draw(false);
        } else if (*second == 'B') {
            selected = (selected + 1) % count;
            draw(false);
        }
    }
}

void printVersion() {
    std::cout << RED << "zrun component version: v" << zpm_version::version()
              << " of ZPM\n" << RESET;
    std::cout << "https://github.com/Zielina-Konrad-productions/ZPM\n";
    std::cout << "Copyright (c) 2026 Ignacyyy & Ry3ball\nLicense: MIT\n";
}

void printHelp(const char* programName) {
    std::cout << RED << "Usage: " << RESET << programName
              << " <package> or zpm run <package>\n";
    std::cout << RED << "Options:\n" << RESET;
    std::cout << "  Finds the app in native PM / Flatpak / Snap and launches it.\n";
    std::cout << "  Search supports fuzzy matching (e.g. 'firefox' finds 'firefox-esr').\n";
    std::cout << "  If installed from multiple sources, lets you pick with ↑↓.\n\n";
    std::cout << "  --version, -v   Show version information\n";
    std::cout << "  --help,    -h   Show this help message\n";
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
        if (!result.options.package.empty()) {
            result.error = "Too many package arguments.";
            return result;
        }

        result.options.package = arg;
    }

    if (!result.options.package.empty() && !isValidPackageQuery(result.options.package)) {
        result.error = "Invalid package name.";
    }

    return result;
}

std::string formatCommands(const std::vector<std::vector<std::string>>& commands) {
    std::string output;

    for (const auto& command : commands) {
        if (command.empty()) {
            continue;
        }

        if (!output.empty()) {
            output += ", ";
        }

        output += command.front();
    }

    return output;
}

bool launchItem(MenuItem item) {
    ensureLaunchCommands(item);

    std::cout << GREEN << "Launching (" << item.sourceLabel << "): " << RESET
              << item.commandDisplay;
    if (item.source == Source::Native) {
        std::cout << YELLOW << "  [" << item.packageName << "]" << RESET;
    }
    std::cout << "\n";

    const LaunchResult result = launchDetached(item.launchCommands);
    if (result.started) {
        return true;
    }

    std::cerr << RED << "Error: Could not launch '" << item.packageName << "'.\n"
              << RESET;
    std::cerr << "Tried: " << formatCommands(item.launchCommands) << "\n";
    if (result.errorNumber != 0) {
        std::cerr << "Reason: " << std::strerror(result.errorNumber) << "\n";
    }
    return false;
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

    if (options.package.empty()) {
        std::cerr << YELLOW << "No package specified.\n" << RESET;
        return 1;
    }

    zpm_update::checkForUpdates();

    const std::string packageManager = get_package_manager();

    std::cout << YELLOW << "Searching for '" << options.package << "'...\r"
              << RESET << std::flush;

    const std::vector<MenuItem> items = findLaunchTargets(options.package, packageManager);
    std::cout << "\033[K" << std::flush;

    if (items.empty()) {
        std::cerr << RED << "No launchable app found for '" << options.package << "'"
                  << " (checked native desktop apps, Flatpak and Snap";
        if (packageManager == "unknown") {
            std::cerr << "; native package manager was not detected";
        }
        std::cerr << ").\n" << RESET;
        return 1;
    }

    std::size_t chosen = 0;
    if (items.size() > 1) {
        const SelectionResult selection = arrowMenu(options.package, items);
        std::cout << "\n";

        if (selection.status == SelectionStatus::Cancelled) {
            std::cout << YELLOW << "Cancelled.\n" << RESET;
            return 0;
        }
        if (selection.status == SelectionStatus::Unavailable) {
            std::cerr << RED
                      << "Multiple matches found, but an interactive terminal is required.\n"
                      << RESET;
            const std::size_t shown = std::min(items.size(), kNonInteractiveListLimit);
            for (std::size_t i = 0; i < shown; ++i) {
                const MenuItem& item = items[i];
                std::cerr << "  " << displayLabel(item) << "\n";
            }
            if (items.size() > shown) {
                std::cerr << "  ... and " << (items.size() - shown)
                          << " more matches\n";
            }
            return 1;
        }

        chosen = selection.index;
    } else {
        MenuItem only = items[0];
        ensureLaunchCommands(only);
        std::cout << BOLD << "Found: " << RESET << displayLabel(only) << "\n";
        return launchItem(std::move(only)) ? 0 : 1;
    }

    return launchItem(items[chosen]) ? 0 : 1;
}
