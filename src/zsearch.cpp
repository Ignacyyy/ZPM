// zsearch.cpp - part of ZPM
// Search native, Flatpak and Snap package catalogs.

#include "main.h"

#include <cerrno>
#include <cctype>
#include <exception>

namespace {

constexpr std::size_t kMaxQueryLength = 1024;
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
    SourceFilter filter = SourceFilter::All;
    std::vector<std::string> queryTerms;
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

struct SearchResult {
    std::string title;
    std::string summary;
};

struct SearchSection {
    std::string title;
    std::string label;
    std::vector<SearchResult> results;
    bool ok = true;
    std::string error;
};

struct AppContext {
    PackageManager packageManager = PackageManager::Unknown;
    std::string dnfCommand;
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

std::vector<std::string> splitAndTrim(const std::string& text, char delimiter) {
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

std::string joinTerms(const std::vector<std::string>& terms) {
    std::string joined;

    for (const std::string& term : terms) {
        if (!joined.empty()) {
            joined += ' ';
        }
        joined += term;
    }

    return joined;
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

std::vector<std::string> withTerms(std::vector<std::string> args,
                                   const std::vector<std::string>& terms) {
    args.insert(args.end(), terms.begin(), terms.end());
    return args;
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

std::string dnfCommand() {
    if (commandExists("dnf")) {
        return "dnf";
    }
    return commandExists("dnf5") ? "dnf5" : std::string {};
}

bool nativeSearchToolAvailable(const AppContext& context) {
    switch (context.packageManager) {
        case PackageManager::Apt:
            return commandExists("apt-cache");
        case PackageManager::Zypper:
            return commandExists("zypper");
        case PackageManager::Dnf:
            return !context.dnfCommand.empty();
        case PackageManager::Unknown:
            return false;
    }

    return false;
}

std::string nativeToolLabel(PackageManager packageManager) {
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

bool validQueryTerm(const std::string& term) {
    return !term.empty() &&
           std::none_of(term.begin(), term.end(), [](unsigned char c) {
               return std::iscntrl(c);
           });
}

bool validQuery(const std::vector<std::string>& terms) {
    if (terms.empty()) {
        return false;
    }

    std::size_t length = 0;
    for (const std::string& term : terms) {
        if (!validQueryTerm(term)) {
            return false;
        }
        length += term.size();
        if (length > kMaxQueryLength) {
            return false;
        }
        ++length;
    }

    return true;
}

std::vector<std::string> makeHighlightNeedles(const std::vector<std::string>& queryTerms) {
    std::vector<std::string> needles;

    for (const std::string& term : queryTerms) {
        const std::string cleaned = toLower(trim(term));
        if (!cleaned.empty() &&
            std::find(needles.begin(), needles.end(), cleaned) == needles.end()) {
            needles.push_back(cleaned);
        }
    }

    std::sort(needles.begin(), needles.end(), [](const std::string& lhs,
                                                 const std::string& rhs) {
        return lhs.size() > rhs.size();
    });

    return needles;
}

std::string highlight(const std::string& text, const std::vector<std::string>& needles) {
    if (text.empty() || needles.empty()) {
        return text;
    }

    const std::string lowerText = toLower(text);
    std::vector<bool> highlighted(text.size(), false);

    for (const std::string& needle : needles) {
        std::size_t pos = lowerText.find(needle);
        while (pos != std::string::npos) {
            for (std::size_t index = pos; index < pos + needle.size() &&
                                             index < highlighted.size(); ++index) {
                highlighted[index] = true;
            }
            pos = lowerText.find(needle, pos + needle.size());
        }
    }

    std::string output;
    output.reserve(text.size() + 16);
    bool active = false;

    for (std::size_t index = 0; index < text.size(); ++index) {
        if (highlighted[index] && !active) {
            output += YELLOW;
            active = true;
        } else if (!highlighted[index] && active) {
            output += RESET;
            active = false;
        }
        output += text[index];
    }

    if (active) {
        output += RESET;
    }

    return output;
}

bool commandFailedToExecute(const ProcessResult& result) {
    return result.exitCode == 126 || result.exitCode == 127;
}

SearchSection failedSection(std::string title, std::string label, std::string error) {
    SearchSection section;
    section.title = std::move(title);
    section.label = std::move(label);
    section.ok = false;
    section.error = std::move(error);
    return section;
}

SearchSection parseAptResults(const ProcessResult& result) {
    SearchSection section;
    section.title = "APT";
    section.label = "APT";

    for (const std::string& line : splitLines(result.output)) {
        const std::size_t dash = line.find(" - ");
        if (dash == std::string::npos) {
            continue;
        }

        const std::string name = trim(line.substr(0, dash));
        if (name.empty()) {
            continue;
        }

        section.results.push_back({name, trim(line.substr(dash + 3))});
    }

    return section;
}

SearchSection parseZypperResults(const ProcessResult& result) {
    SearchSection section;
    section.title = "Zypper";
    section.label = "ZYPPER";

    bool pastHeader = false;
    for (const std::string& line : splitLines(result.output)) {
        if (line.find("-+-") != std::string::npos) {
            pastHeader = true;
            continue;
        }
        if (!pastHeader || trim(line).empty()) {
            continue;
        }

        const std::vector<std::string> columns = splitAndTrim(line, '|');
        if (columns.size() < 3 || columns[1].empty()) {
            continue;
        }

        section.results.push_back({columns[1], columns[2]});
    }

    return section;
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

SearchSection parseDnfResults(const ProcessResult& result) {
    SearchSection section;
    section.title = "DNF";
    section.label = "DNF";

    for (const std::string& rawLine : splitLines(result.output)) {
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

        const std::string name = stripRpmArch(package);
        if (!name.empty()) {
            section.results.push_back({name, summary});
        }
    }

    return section;
}

SearchSection parseFlatpakResults(const ProcessResult& result) {
    SearchSection section;
    section.title = "Flatpak";
    section.label = "FLATPAK";

    for (const std::string& rawLine : splitLines(result.output)) {
        const std::string line = trim(rawLine);
        if (line.empty()) {
            continue;
        }

        const std::vector<std::string> columns = splitAndTrim(line, '\t');
        if (columns.size() < 2 || columns[0].empty()) {
            continue;
        }

        if (toLower(columns[0]) == "application") {
            continue;
        }

        std::string title = columns[0];
        if (!columns[1].empty()) {
            title += " - " + columns[1];
        }

        const std::string summary = columns.size() >= 3 ? columns[2] : std::string {};
        section.results.push_back({title, summary});
    }

    return section;
}

SearchSection parseSnapResults(const ProcessResult& result) {
    SearchSection section;
    section.title = "Snap";
    section.label = "SNAP";

    for (const std::string& rawLine : splitLines(result.output)) {
        const std::string line = trim(rawLine);
        if (line.empty()) {
            continue;
        }

        const std::string lower = toLower(line);
        if (startsWith(lower, "name ") || startsWith(lower, "no matching snaps")) {
            continue;
        }

        std::istringstream stream(line);
        std::string name;
        std::string version;
        std::string publisher;
        std::string notes;
        std::string summary;

        if (!(stream >> name)) {
            continue;
        }
        stream >> version >> publisher >> notes;
        std::getline(stream, summary);

        section.results.push_back({name, trim(summary)});
    }

    return section;
}

SearchSection runSearchCommand(const std::vector<std::string>& args,
                               SearchSection (*parser)(const ProcessResult&),
                               const std::string& title,
                               const std::string& label) {
    const ProcessResult result = captureProcess(args);
    if (result.truncated) {
        return failedSection(title, label, title + " search output was too large.");
    }
    if (commandFailedToExecute(result)) {
        return failedSection(title, label, "Could not run " + title + " search command.");
    }

    return parser(result);
}

SearchSection searchApt(const std::vector<std::string>& queryTerms) {
    if (!commandExists("apt-cache")) {
        return failedSection("APT", "APT", "apt-cache is not installed or not in PATH.");
    }

    return runSearchCommand(withTerms({"apt-cache", "search"}, queryTerms),
                            parseAptResults,
                            "APT",
                            "APT");
}

SearchSection searchZypper(const std::vector<std::string>& queryTerms) {
    if (!commandExists("zypper")) {
        return failedSection("Zypper", "ZYPPER", "zypper is not installed or not in PATH.");
    }

    return runSearchCommand(withTerms({"zypper", "--no-refresh", "search"}, queryTerms),
                            parseZypperResults,
                            "Zypper",
                            "ZYPPER");
}

SearchSection searchDnf(const AppContext& context,
                        const std::vector<std::string>& queryTerms) {
    if (context.dnfCommand.empty()) {
        return failedSection("DNF", "DNF", "dnf or dnf5 is not installed or not in PATH.");
    }

    return runSearchCommand(withTerms({context.dnfCommand, "search"}, queryTerms),
                            parseDnfResults,
                            "DNF",
                            "DNF");
}

SearchSection searchFlatpak(const std::vector<std::string>& queryTerms) {
    return runSearchCommand(withTerms({"flatpak",
                                       "search",
                                       "--columns=application,name,description"},
                                      queryTerms),
                            parseFlatpakResults,
                            "Flatpak",
                            "FLATPAK");
}

SearchSection searchSnap(const std::vector<std::string>& queryTerms) {
    return runSearchCommand(withTerms({"snap", "find"}, queryTerms),
                            parseSnapResults,
                            "Snap",
                            "SNAP");
}

SearchSection searchNative(const AppContext& context,
                           const std::vector<std::string>& queryTerms) {
    switch (context.packageManager) {
        case PackageManager::Apt:
            return searchApt(queryTerms);
        case PackageManager::Zypper:
            return searchZypper(queryTerms);
        case PackageManager::Dnf:
            return searchDnf(context, queryTerms);
        case PackageManager::Unknown:
            return failedSection("Native",
                                 "NATIVE",
                                 "Could not detect a supported package manager (apt / zypper / dnf).");
    }

    return failedSection("Native", "NATIVE", "Could not search native packages.");
}

void printSection(const SearchSection& section,
                  const std::vector<std::string>& highlightTerms,
                  bool leadingBlank,
                  std::ostream& output) {
    if (leadingBlank) {
        output << "\n";
    }

    if (!section.ok) {
        output << RED << "Error: " << section.error << "\n" << RESET;
        return;
    }

    if (section.results.empty()) {
        output << YELLOW << "=== " << section.title << ": no results ===\n" << RESET;
        return;
    }

    output << YELLOW << "=== " << section.title << " results ===\n" << RESET;
    for (const SearchResult& result : section.results) {
        output << GREEN << "[" << section.label << "]" << RESET << " "
               << highlight(result.title, highlightTerms) << "\n";
        if (!result.summary.empty()) {
            output << "    " << highlight(result.summary, highlightTerms) << "\n";
        }
    }

    output << "\n" << GREEN << " " << section.title
           << " found: " << section.results.size() << " packages\n" << RESET;
}

void printHelp(const char* programName) {
    std::cout << RED << "Usage: " << RESET << programName
              << " <query> [options] or zpm search <query> [options]\n";
    std::cout << RED << "Options:\n" << RESET;
    std::cout << "  --version,  -v  Show version information\n";
    std::cout << "  --help,     -h  Show this help message\n";
    std::cout << "  --native,   -n  Search only native PM packages (apt/zypper/dnf)\n";
    std::cout << "  --flatpak,  -f  Search only Flatpak packages\n";
    std::cout << "  --snap,     -s  Search only Snap packages\n";
}

void printVersion() {
    std::cout << RED << "zsearch component version: v"
              << zpm_version::version() << " of ZPM\n"
              << RESET;
    std::cout << "https://github.com/Zielina-Konrad-productions/ZPM\n";
    std::cout << "Copyright (c) 2026 Ignacyyy & Ry3ball\nLicense: MIT\n";
}

ParseResult parseArgs(int argc, char* argv[]) {
    ParseResult result;
    bool optionsEnded = false;
    int sourceFilters = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (!optionsEnded && arg == "--") {
            optionsEnded = true;
            continue;
        }
        if (!optionsEnded && (arg == "--help" || arg == "-h")) {
            result.options.showHelp = true;
            continue;
        }
        if (!optionsEnded && (arg == "--version" || arg == "-v")) {
            result.options.showVersion = true;
            continue;
        }
        if (!optionsEnded && (arg == "--native" || arg == "-n")) {
            result.options.filter = SourceFilter::Native;
            ++sourceFilters;
            continue;
        }
        if (!optionsEnded && (arg == "--flatpak" || arg == "-f")) {
            result.options.filter = SourceFilter::Flatpak;
            ++sourceFilters;
            continue;
        }
        if (!optionsEnded && (arg == "--snap" || arg == "-s")) {
            result.options.filter = SourceFilter::Snap;
            ++sourceFilters;
            continue;
        }
        if (!optionsEnded && startsWith(arg, "-")) {
            result.error = "Unknown option: " + arg;
            return result;
        }

        result.options.queryTerms.push_back(arg);
    }

    if (sourceFilters > 1) {
        result.error = "Choose only one package source filter.";
        return result;
    }

    if ((result.options.showHelp || result.options.showVersion) &&
        result.options.filter != SourceFilter::All) {
        result.error = "--help and --version cannot be combined with package source filters.";
        return result;
    }

    if (!result.options.queryTerms.empty() && !validQuery(result.options.queryTerms)) {
        result.error = "Search query is empty, too long, or contains control characters.";
    }

    return result;
}

AppContext makeContext() {
    AppContext context;
    context.packageManager = parsePackageManager(get_package_manager());
    context.dnfCommand = dnfCommand();
    context.hasFlatpak = commandExists("flatpak");
    context.hasSnap = commandExists("snap");
    return context;
}

bool runSearch(const Options& options, const AppContext& context) {
    const std::string queryText = joinTerms(options.queryTerms);
    const std::vector<std::string> highlightTerms = makeHighlightNeedles(options.queryTerms);
    std::cout << GREEN << " Searching: " << RESET << queryText << "\n\n";

    switch (options.filter) {
        case SourceFilter::Native: {
            if (context.packageManager == PackageManager::Unknown) {
                const SearchSection section = searchNative(context, options.queryTerms);
                printSection(section, highlightTerms, false, std::cout);
                return false;
            }

            if (!nativeSearchToolAvailable(context)) {
                std::cerr << RED << "Error: " << nativeToolLabel(context.packageManager)
                          << " search tools are not available.\n" << RESET;
                return false;
            }

            const SearchSection section = searchNative(context, options.queryTerms);
            printSection(section, highlightTerms, false, std::cout);
            return section.ok;
        }
        case SourceFilter::Flatpak: {
            if (!context.hasFlatpak) {
                std::cerr << RED << "Error: Flatpak is not installed or not in PATH.\n" << RESET;
                return false;
            }

            const SearchSection section = searchFlatpak(options.queryTerms);
            printSection(section, highlightTerms, false, std::cout);
            return section.ok;
        }
        case SourceFilter::Snap: {
            if (!context.hasSnap) {
                std::cerr << RED << "Error: Snap is not installed or not in PATH.\n" << RESET;
                return false;
            }

            const SearchSection section = searchSnap(options.queryTerms);
            printSection(section, highlightTerms, false, std::cout);
            return section.ok;
        }
        case SourceFilter::All:
            break;
    }

    if (context.packageManager == PackageManager::Unknown) {
        const SearchSection section = searchNative(context, options.queryTerms);
        printSection(section, highlightTerms, false, std::cout);
        return false;
    }

    if (!nativeSearchToolAvailable(context)) {
        std::cerr << RED << "Error: " << nativeToolLabel(context.packageManager)
                  << " search tools are not available.\n" << RESET;
        return false;
    }

    bool ok = true;
    const SearchSection nativeSection = searchNative(context, options.queryTerms);
    printSection(nativeSection, highlightTerms, false, std::cout);
    ok = nativeSection.ok && ok;

    if (context.hasFlatpak) {
        const SearchSection flatpakSection = searchFlatpak(options.queryTerms);
        printSection(flatpakSection, highlightTerms, true, std::cout);
        ok = flatpakSection.ok && ok;
    }

    if (context.hasSnap) {
        const SearchSection snapSection = searchSnap(options.queryTerms);
        printSection(snapSection, highlightTerms, true, std::cout);
        ok = snapSection.ok && ok;
    }

    return ok;
}

} // namespace

int main(int argc, char* argv[]) {
    try {
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

        if (options.queryTerms.empty()) {
            std::cerr << YELLOW << "No search query specified!\n" << RESET;
            return 1;
        }

        zpm_update::checkForUpdates();

        const AppContext context = makeContext();
        return runSearch(options, context) ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << RED << "Error: " << error.what() << "\n" << RESET;
        return 1;
    } catch (...) {
        std::cerr << RED << "Error: Unexpected failure.\n" << RESET;
        return 1;
    }
}
