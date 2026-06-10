// zlist.cpp - part of ZPM
// List installed native, Flatpak and Snap packages.

#include "main.h"

#include <cerrno>
#include <cctype>
#include <cstring>

namespace {

constexpr std::size_t kMaxCapturedOutput = 64 * 1024 * 1024;

enum class PackageManager {
    Apt,
    Zypper,
    Dnf,
    Unknown
};

enum class SourceFilter {
    All,
    Native,
    Flatpak,
    Snap
};

struct Options {
    bool showHelp = false;
    bool showVersion = false;
    bool noPager = false;
    SourceFilter filter = SourceFilter::All;
};

struct ParseResult {
    Options options;
    std::string error;
};

struct ProcessResult {
    int exitCode = 127;
    std::string output;
    bool truncated = false;
};

struct PackageRecord {
    std::string name;
    std::string version;
};

struct AppContext {
    PackageManager packageManager = PackageManager::Unknown;
    bool hasFlatpak = false;
    bool hasSnap = false;
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

class SignalGuard {
public:
    SignalGuard(int signalNumber, void (*handler)(int)) : signalNumber_(signalNumber) {
        struct sigaction action {};
        action.sa_handler = handler;
        sigemptyset(&action.sa_mask);

        active_ = sigaction(signalNumber_, &action, &previous_) == 0;
    }

    SignalGuard(const SignalGuard&) = delete;
    SignalGuard& operator=(const SignalGuard&) = delete;

    ~SignalGuard() {
        if (active_) {
            sigaction(signalNumber_, &previous_, nullptr);
        }
    }

private:
    int signalNumber_ = 0;
    bool active_ = false;
    struct sigaction previous_ {};
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

std::vector<std::string> split(const std::string& text, char delimiter) {
    std::vector<std::string> parts;
    std::size_t start = 0;

    for (;;) {
        const std::size_t pos = text.find(delimiter, start);
        if (pos == std::string::npos) {
            parts.push_back(trim(text.substr(start)));
            break;
        }

        parts.push_back(trim(text.substr(start, pos - start)));
        start = pos + 1;
    }

    return parts;
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

bool writeAll(int fd, const char* data, std::size_t size) {
    while (size > 0) {
        const ssize_t written = write(fd, data, size);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            return false;
        }

        data += written;
        size -= static_cast<std::size_t>(written);
    }

    return true;
}

std::size_t lineCount(const std::string& text) {
    if (text.empty()) {
        return 0;
    }

    std::size_t lines = static_cast<std::size_t>(
        std::count(text.begin(), text.end(), '\n'));
    if (text.back() != '\n') {
        ++lines;
    }
    return lines;
}

std::size_t terminalRows() {
    winsize size {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_row > 0) {
        return size.ws_row;
    }

    return 24;
}

std::vector<std::string> pagerCommand() {
    if (commandExists("less")) {
        return {"less", "-R"};
    }
    if (commandExists("more")) {
        return {"more"};
    }
    return {};
}

bool pageOutput(const std::string& output) {
    const std::vector<std::string> args = pagerCommand();
    if (args.empty()) {
        return false;
    }

    int pipeFd[2] = {-1, -1};
    if (pipe(pipeFd) != 0) {
        return false;
    }

    FileDescriptor readEnd(pipeFd[0]);
    FileDescriptor writeEnd(pipeFd[1]);
    setCloseOnExec(readEnd.get());
    setCloseOnExec(writeEnd.get());

    const pid_t pid = fork();
    if (pid < 0) {
        return false;
    }

    if (pid == 0) {
        writeEnd.reset();
        if (dup2(readEnd.get(), STDIN_FILENO) < 0) {
            _exit(127);
        }

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const std::string& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execvp(argv[0], argv.data());
        _exit(errno == ENOENT ? 127 : 126);
    }

    readEnd.reset();

    {
        SignalGuard ignoreSigpipe(SIGPIPE, SIG_IGN);
        writeAll(writeEnd.get(), output.data(), output.size());
    }

    writeEnd.reset();
    return waitForChild(pid) == 0;
}

void emitOutput(const std::string& output, bool noPager) {
    if (output.empty()) {
        return;
    }

    const bool shouldUsePager = !noPager &&
                                isatty(STDOUT_FILENO) == 1 &&
                                lineCount(output) > terminalRows();

    if (shouldUsePager && pageOutput(output)) {
        return;
    }

    std::cout << output;
}

void appendCaptured(ProcessResult& result, const char* data, std::size_t size) {
    if (result.output.size() >= kMaxCapturedOutput) {
        result.truncated = true;
        return;
    }

    const std::size_t remaining = kMaxCapturedOutput - result.output.size();
    const std::size_t accepted = std::min(size, remaining);
    result.output.append(data, accepted);
    result.truncated = result.truncated || accepted < size;
}

ProcessResult captureProcess(const std::vector<std::string>& args) {
    if (args.empty() || args.front().empty()) {
        return {};
    }

    int pipeFd[2] = {-1, -1};
    if (pipe(pipeFd) != 0) {
        return {};
    }

    FileDescriptor readEnd(pipeFd[0]);
    FileDescriptor writeEnd(pipeFd[1]);
    setCloseOnExec(readEnd.get());
    setCloseOnExec(writeEnd.get());

    const pid_t pid = fork();
    if (pid < 0) {
        return {};
    }

    if (pid == 0) {
        readEnd.reset();
        if (dup2(writeEnd.get(), STDOUT_FILENO) < 0) {
            _exit(127);
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

    writeEnd.reset();

    ProcessResult result;
    std::array<char, 4096> buffer {};

    for (;;) {
        const ssize_t count = read(readEnd.get(), buffer.data(), buffer.size());
        if (count > 0) {
            appendCaptured(result, buffer.data(), static_cast<std::size_t>(count));
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

    result.exitCode = waitForChild(pid);
    return result;
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

std::string packageManagerLabel(PackageManager packageManager) {
    switch (packageManager) {
        case PackageManager::Apt:
            return "APT";
        case PackageManager::Zypper:
            return "Zypper";
        case PackageManager::Dnf:
            return "DNF";
        case PackageManager::Unknown:
            return "Native";
    }

    return "Native";
}

std::string dnfCommand() {
    if (commandExists("dnf")) {
        return "dnf";
    }
    return commandExists("dnf5") ? "dnf5" : std::string {};
}

bool nativeListToolAvailable(PackageManager packageManager) {
    switch (packageManager) {
        case PackageManager::Apt:
            return commandExists("dpkg-query") || commandExists("apt");
        case PackageManager::Zypper:
            return commandExists("rpm") || commandExists("zypper");
        case PackageManager::Dnf:
            return commandExists("rpm") || !dnfCommand().empty();
        case PackageManager::Unknown:
            return false;
    }

    return false;
}

void sortPackages(std::vector<PackageRecord>& packages) {
    std::sort(packages.begin(), packages.end(), [](const PackageRecord& lhs,
                                                   const PackageRecord& rhs) {
        const std::string leftName = toLower(lhs.name);
        const std::string rightName = toLower(rhs.name);
        if (leftName != rightName) {
            return leftName < rightName;
        }
        return lhs.version < rhs.version;
    });
}

std::vector<PackageRecord> parseDpkgQueryOutput(const std::string& output) {
    std::vector<PackageRecord> packages;

    for (const std::string& line : splitLines(output)) {
        const std::vector<std::string> columns = split(line, '\t');
        if (columns.size() < 2 || columns[0].empty()) {
            continue;
        }

        packages.push_back({columns[0], columns[1]});
    }

    sortPackages(packages);
    return packages;
}

std::vector<PackageRecord> parseAptListOutput(const std::string& output) {
    std::vector<PackageRecord> packages;

    for (const std::string& rawLine : splitLines(output)) {
        const std::string line = trim(rawLine);
        if (line.empty() || startsWith(line, "Listing...") || startsWith(line, "Listowanie...")) {
            continue;
        }

        std::istringstream stream(line);
        std::string packageWithRepo;
        std::string version;
        if (!(stream >> packageWithRepo >> version)) {
            continue;
        }

        const std::size_t slash = packageWithRepo.find('/');
        const std::string name = slash == std::string::npos
            ? packageWithRepo
            : packageWithRepo.substr(0, slash);
        if (!name.empty()) {
            packages.push_back({name, version});
        }
    }

    sortPackages(packages);
    return packages;
}

std::vector<PackageRecord> parseRpmQueryOutput(const std::string& output) {
    std::vector<PackageRecord> packages;

    for (const std::string& line : splitLines(output)) {
        const std::vector<std::string> columns = split(line, '\t');
        if (columns.size() < 2 || columns[0].empty()) {
            continue;
        }

        packages.push_back({columns[0], columns[1]});
    }

    sortPackages(packages);
    return packages;
}

std::vector<PackageRecord> parseZypperPackagesOutput(const std::string& output) {
    std::vector<PackageRecord> packages;
    bool pastHeader = false;

    for (const std::string& line : splitLines(output)) {
        if (line.find("-+-") != std::string::npos) {
            pastHeader = true;
            continue;
        }
        if (!pastHeader || trim(line).empty()) {
            continue;
        }

        const std::vector<std::string> columns = split(line, '|');
        if (columns.size() < 4 || columns[2].empty()) {
            continue;
        }

        packages.push_back({columns[2], columns[3]});
    }

    sortPackages(packages);
    return packages;
}

bool isDnfMetadataToken(const std::string& token) {
    const std::string lower = toLower(token);
    return lower == "installed" || lower == "zainstalowane" ||
           lower == "aktualizowanie" || lower == "zawadowano" ||
           lower == "załadowano" || lower == "updating" ||
           lower == "repositories" || lower == "loading" ||
           lower == "repozytoria" || lower == "package" ||
           lower == "pakiet" || lower == "name" ||
           lower == "nazwa" || lower == "wersja" ||
           lower == "version" || lower == "dopasowane" ||
           lower == "pola:" || lower == "last";
}

std::vector<PackageRecord> parseDnfListOutput(const std::string& output) {
    std::vector<PackageRecord> packages;

    for (const std::string& rawLine : splitLines(output)) {
        const std::string line = trim(rawLine);
        if (line.empty() || line.find("====") != std::string::npos) {
            continue;
        }

        std::istringstream stream(line);
        std::string packageWithArch;
        std::string version;
        if (!(stream >> packageWithArch) || isDnfMetadataToken(packageWithArch)) {
            continue;
        }
        if (!(stream >> version)) {
            continue;
        }

        std::string name = packageWithArch;
        const std::size_t dot = packageWithArch.rfind('.');
        if (dot != std::string::npos && dot > 0) {
            name = packageWithArch.substr(0, dot);
        }

        if (!name.empty()) {
            packages.push_back({name, version});
        }
    }

    sortPackages(packages);
    return packages;
}

std::vector<std::string> parsePlainListOutput(const std::string& output) {
    std::vector<std::string> lines;

    for (const std::string& rawLine : splitLines(output)) {
        const std::string line = trim(rawLine);
        if (!line.empty()) {
            lines.push_back(line);
        }
    }

    return lines;
}

bool captureAndParse(const std::vector<std::string>& args,
                     std::vector<PackageRecord>& packages,
                     std::vector<PackageRecord> (*parser)(const std::string&)) {
    const ProcessResult result = captureProcess(args);
    if (result.exitCode != 0 || result.truncated) {
        return false;
    }

    packages = parser(result.output);
    return true;
}

bool printPackageSection(const std::string& title,
                         const std::vector<PackageRecord>& packages,
                         bool& printedAnySection,
                         std::ostream& output) {
    if (printedAnySection) {
        output << "\n";
    }
    output << YELLOW << "=== " << title << " packages ===\n" << RESET;
    printedAnySection = true;

    for (const PackageRecord& package : packages) {
        output << package.name << " (" << package.version << ")\n";
    }

    return true;
}

void printRawSection(const std::string& title,
                     const std::vector<std::string>& lines,
                     bool& printedAnySection,
                     std::ostream& output) {
    if (printedAnySection) {
        output << "\n";
    }
    output << YELLOW << "=== " << title << " packages ===\n" << RESET;
    printedAnySection = true;

    for (const std::string& line : lines) {
        output << line << "\n";
    }
}

bool listApt(bool& printedAnySection, std::ostream& output) {
    std::vector<PackageRecord> packages;

    if (commandExists("dpkg-query") &&
        captureAndParse({"dpkg-query", "-W", "-f=${binary:Package}\t${Version}\n"},
                        packages,
                        parseDpkgQueryOutput)) {
        return printPackageSection("APT", packages, printedAnySection, output);
    }

    if (commandExists("apt") &&
        captureAndParse({"apt", "list", "--installed"},
                        packages,
                        parseAptListOutput)) {
        return printPackageSection("APT", packages, printedAnySection, output);
    }

    std::cerr << RED << "Error: Could not list APT packages.\n" << RESET;
    return false;
}

bool listRpmBacked(const std::string& title,
                   const std::vector<std::string>& fallbackCommand,
                   std::vector<PackageRecord> (*fallbackParser)(const std::string&),
                   bool& printedAnySection,
                   std::ostream& output) {
    std::vector<PackageRecord> packages;

    // rpm is the stable source of installed package metadata for DNF and Zypper systems.
    if (commandExists("rpm") &&
        captureAndParse({"rpm",
                         "-qa",
                         "--qf",
                         "%{NAME}\t%|EPOCH?{%{EPOCH}:}:{}|%{VERSION}-%{RELEASE}\n"},
                        packages,
                        parseRpmQueryOutput)) {
        return printPackageSection(title, packages, printedAnySection, output);
    }

    if (!fallbackCommand.empty() &&
        commandExists(fallbackCommand.front()) &&
        captureAndParse(fallbackCommand, packages, fallbackParser)) {
        return printPackageSection(title, packages, printedAnySection, output);
    }

    std::cerr << RED << "Error: Could not list " << title << " packages.\n" << RESET;
    return false;
}

bool listNative(PackageManager packageManager, bool& printedAnySection, std::ostream& output) {
    switch (packageManager) {
        case PackageManager::Apt:
            return listApt(printedAnySection, output);
        case PackageManager::Zypper:
            return listRpmBacked("Zypper",
                                 {"zypper", "--no-refresh", "packages", "-i"},
                                 parseZypperPackagesOutput,
                                 printedAnySection,
                                 output);
        case PackageManager::Dnf: {
            const std::string command = dnfCommand();
            return listRpmBacked("DNF",
                                 command.empty()
                                     ? std::vector<std::string> {}
                                     : std::vector<std::string> {command, "list", "--installed"},
                                 parseDnfListOutput,
                                 printedAnySection,
                                 output);
        }
        case PackageManager::Unknown:
            std::cerr << RED
                      << "Error: Could not detect a supported package manager "
                      << "(apt / zypper / dnf).\n"
                      << RESET;
            return false;
    }

    return false;
}

bool listFlatpak(bool explicitRequest, bool& printedAnySection, std::ostream& output) {
    if (!commandExists("flatpak")) {
        if (explicitRequest) {
            std::cerr << RED << "Error: Flatpak is not installed or not in PATH.\n" << RESET;
        }
        return !explicitRequest;
    }

    const ProcessResult result = captureProcess({
        "flatpak",
        "list",
        "--columns=application,name,version"
    });

    if (result.exitCode != 0 || result.truncated) {
        std::cerr << RED << "Error: Could not list Flatpak packages.\n" << RESET;
        return false;
    }

    printRawSection("Flatpak", parsePlainListOutput(result.output), printedAnySection, output);
    return true;
}

bool listSnap(bool explicitRequest, bool& printedAnySection, std::ostream& output) {
    if (!commandExists("snap")) {
        if (explicitRequest) {
            std::cerr << RED << "Error: Snap is not installed or not in PATH.\n" << RESET;
        }
        return !explicitRequest;
    }

    const ProcessResult result = captureProcess({"snap", "list"});
    if (result.exitCode != 0 || result.truncated) {
        std::cerr << RED << "Error: Could not list Snap packages.\n" << RESET;
        return false;
    }

    printRawSection("Snap", parsePlainListOutput(result.output), printedAnySection, output);
    return true;
}

void printHelp(const char* programName) {
    std::cout << RED << "Usage: " << RESET << programName
              << " [options] or zpm list [options]\n";
    std::cout << RED << "Options:\n" << RESET;
    std::cout << "  --version,  -v  Show version information\n";
    std::cout << "  --help,     -h  Show this help message\n";
    std::cout << "  --native,   -n  List only native PM packages (apt/zypper/dnf)\n";
    std::cout << "  --flatpak,  -f  List only Flatpak packages\n";
    std::cout << "  --snap,     -s  List only Snap packages\n";
    std::cout << "  --no-pager      Print directly even when output is longer than the terminal\n";
}

void printVersion() {
    std::cout << RED << "zlist component version: v"
              << zpm_version::version() << " of ZPM\n"
              << RESET;
    std::cout << "https://github.com/Zielina-Konrad-productions/ZPM\n";
    std::cout << "Copyright (c) 2026 Ignacyyy & Ry3ball\nLicense: MIT\n";
}

ParseResult parseArgs(int argc, char* argv[]) {
    ParseResult result;
    int sourceFilters = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            result.options.showHelp = true;
            continue;
        }
        if (arg == "--version" || arg == "-v") {
            result.options.showVersion = true;
            continue;
        }
        if (arg == "--native" || arg == "-n") {
            result.options.filter = SourceFilter::Native;
            ++sourceFilters;
            continue;
        }
        if (arg == "--flatpak" || arg == "-f") {
            result.options.filter = SourceFilter::Flatpak;
            ++sourceFilters;
            continue;
        }
        if (arg == "--snap" || arg == "-s") {
            result.options.filter = SourceFilter::Snap;
            ++sourceFilters;
            continue;
        }
        if (arg == "--no-pager") {
            result.options.noPager = true;
            continue;
        }

        result.error = startsWith(arg, "-")
            ? "Unknown option: " + arg
            : "Unexpected argument: " + arg;
        return result;
    }

    if (sourceFilters > 1) {
        result.error = "Choose only one package source filter.";
        return result;
    }

    if ((result.options.showHelp || result.options.showVersion) &&
        result.options.filter != SourceFilter::All) {
        result.error = "--help and --version cannot be combined with package source filters.";
    }

    return result;
}

AppContext makeContext() {
    AppContext context;
    context.packageManager = parsePackageManager(get_package_manager());
    context.hasFlatpak = commandExists("flatpak");
    context.hasSnap = commandExists("snap");
    return context;
}

bool runList(const Options& options, const AppContext& context) {
    bool printedAnySection = false;
    std::ostringstream output;
    bool ok = false;

    switch (options.filter) {
        case SourceFilter::Native:
            if (context.packageManager == PackageManager::Unknown) {
                ok = listNative(context.packageManager, printedAnySection, output);
                emitOutput(output.str(), options.noPager);
                return ok;
            }
            if (!nativeListToolAvailable(context.packageManager)) {
                std::cerr << RED << "Error: " << packageManagerLabel(context.packageManager)
                          << " listing tools are not available.\n" << RESET;
                return false;
            }
            ok = listNative(context.packageManager, printedAnySection, output);
            emitOutput(output.str(), options.noPager);
            return ok;
        case SourceFilter::Flatpak:
            ok = listFlatpak(true, printedAnySection, output);
            emitOutput(output.str(), options.noPager);
            return ok;
        case SourceFilter::Snap:
            ok = listSnap(true, printedAnySection, output);
            emitOutput(output.str(), options.noPager);
            return ok;
        case SourceFilter::All:
            break;
    }

    if (!nativeListToolAvailable(context.packageManager)) {
        ok = listNative(context.packageManager, printedAnySection, output);
        emitOutput(output.str(), options.noPager);
        return ok;
    }

    ok = listNative(context.packageManager, printedAnySection, output);
    if (context.hasFlatpak) {
        ok = listFlatpak(false, printedAnySection, output) && ok;
    }
    if (context.hasSnap) {
        ok = listSnap(false, printedAnySection, output) && ok;
    }

    emitOutput(output.str(), options.noPager);
    return ok;
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

    zpm_update::checkForUpdates();

    const AppContext context = makeContext();
    return runList(options, context) ? 0 : 1;
}
