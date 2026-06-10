#pragma once
#include "main.h"

#include <cerrno>
#include <chrono>
#include <cctype>
#include <fcntl.h>
#include <fstream>
#include <optional>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace zpm_update {

constexpr const char* kLatestReleaseUrl =
    "https://api.github.com/repos/Zielina-Konrad-productions/ZPM/releases/latest";
constexpr std::chrono::seconds kCheckInterval{900};

struct CommandResult {
    int exitCode = 127;
    std::string output;
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

static inline std::string trim(std::string value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

static inline bool writeAll(int fd, const char* data, size_t size) {
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

static inline bool isSafeOwnedRegularFile(int fd) {
    struct stat st {};
    if (fstat(fd, &st) != 0) {
        return false;
    }

    return S_ISREG(st.st_mode) &&
           st.st_nlink == 1 &&
           st.st_uid == geteuid();
}

static inline int decodeExitStatus(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 127;
}

static inline CommandResult captureCommand(const std::vector<std::string>& args) {
    if (args.empty() || args.front().empty()) {
        return {};
    }

    int pipeFd[2] = {-1, -1};
    if (pipe(pipeFd) != 0) {
        return {};
    }

    fcntl(pipeFd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipeFd[1], F_SETFD, FD_CLOEXEC);

    FileDescriptor readEnd(pipeFd[0]);
    FileDescriptor writeEnd(pipeFd[1]);

    const pid_t pid = fork();
    if (pid < 0) {
        return {};
    }

    if (pid == 0) {
        readEnd.reset();
        dup2(writeEnd.get(), STDOUT_FILENO);

        const int nullFd = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (nullFd >= 0) {
            dup2(nullFd, STDERR_FILENO);
            close(nullFd);
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

    writeEnd.reset();

    CommandResult result;
    std::array<char, 4096> buffer {};
    for (;;) {
        const ssize_t count = read(readEnd.get(), buffer.data(), buffer.size());
        if (count > 0) {
            result.output.append(buffer.data(), static_cast<size_t>(count));
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

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            result.exitCode = 127;
            return result;
        }
    }

    result.exitCode = decodeExitStatus(status);
    return result;
}

static inline std::filesystem::path cacheDirectory() {
    if (geteuid() == 0) {
        return "/var/cache/zpm";
    }

    if (const char* xdg = getenv("XDG_CACHE_HOME");
        xdg != nullptr && *xdg != '\0' && std::filesystem::path(xdg).is_absolute()) {
        return std::filesystem::path(xdg) / "zpm";
    }

    if (const char* home = getenv("HOME");
        home != nullptr && *home != '\0' && std::filesystem::path(home).is_absolute()) {
        return std::filesystem::path(home) / ".cache" / "zpm";
    }

    return {};
}

static inline std::filesystem::path cacheFilePath() {
    const std::filesystem::path directory = cacheDirectory();
    if (directory.empty()) {
        return {};
    }
    return directory / "last_update_check";
}

static inline bool ensureCacheDirectory(const std::filesystem::path& directory) {
    if (directory.empty()) {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        return false;
    }

    std::filesystem::permissions(
        directory,
        std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace,
        ec
    );
    return !ec;
}

static inline bool readLastCheck(long long& value) {
    const std::filesystem::path path = cacheFilePath();
    if (path.empty()) {
        return false;
    }

    FileDescriptor fd(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!fd.valid() || !isSafeOwnedRegularFile(fd.get())) {
        return false;
    }

    std::string text;
    std::array<char, 64> buffer {};
    for (;;) {
        const ssize_t count = read(fd.get(), buffer.data(), buffer.size());
        if (count > 0) {
            text.append(buffer.data(), static_cast<size_t>(count));
            if (text.size() > 32) {
                return false;
            }
            continue;
        }
        if (count == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        return false;
    }

    text = trim(text);
    if (text.empty() ||
        !std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); })) {
        return false;
    }

    try {
        value = std::stoll(text);
    } catch (...) {
        return false;
    }
    return true;
}

static inline bool updateInfoEnabled() {
    std::ifstream conf("/opt/ZPM/zielina.conf");
    if (!conf.is_open()) {
        return true;
    }

    std::string line;
    while (std::getline(conf, line)) {
        line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
        line.erase(std::remove(line.begin(), line.end(), '\n'), line.end());
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (line.rfind("update-info=", 0) == 0) {
            std::string value = line.substr(12);
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value == "true" || value == "1" || value == "yes";
        }
    }
    return true;
}

static inline bool shouldCheckForUpdates() {
    long long lastCheck = 0;
    if (!readLastCheck(lastCheck)) {
        return true;
    }

    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto currentTime = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    return currentTime - lastCheck >= kCheckInterval.count();
}

static inline void saveCheckTime() {
    const std::filesystem::path path = cacheFilePath();
    if (path.empty() || !ensureCacheDirectory(path.parent_path())) {
        return;
    }

    FileDescriptor fd(open(path.c_str(),
                           O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                           0600));
    if (!fd.valid() || !isSafeOwnedRegularFile(fd.get())) {
        return;
    }

    fchmod(fd.get(), 0600);

    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto currentTime = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    const std::string text = std::to_string(currentTime);
    writeAll(fd.get(), text.data(), text.size());
}

static inline std::optional<std::string> extractJsonString(const std::string& json,
                                                           const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    const auto keyPos = json.find(needle);
    if (keyPos == std::string::npos) {
        return std::nullopt;
    }

    const auto colon = json.find(':', keyPos + needle.size());
    if (colon == std::string::npos) {
        return std::nullopt;
    }

    auto pos = json.find_first_not_of(" \t\r\n", colon + 1);
    if (pos == std::string::npos || json[pos] != '"') {
        return std::nullopt;
    }
    ++pos;

    std::string value;
    bool escaping = false;
    for (; pos < json.size(); ++pos) {
        const char c = json[pos];
        if (escaping) {
            value += c;
            escaping = false;
            continue;
        }
        if (c == '\\') {
            escaping = true;
            continue;
        }
        if (c == '"') {
            return value;
        }
        value += c;
    }

    return std::nullopt;
}

static inline std::string normalizeVersion(std::string value) {
    value = trim(value);
    if (!value.empty() && value.front() == 'v') {
        value.erase(value.begin());
    }

    value.erase(std::remove_if(value.begin(),
                               value.end(),
                               [](unsigned char c) {
                                   return std::isspace(c) || std::iscntrl(c);
                               }),
                value.end());

    if (!std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '.' || c == '-' || c == '_';
        })) {
        return {};
    }

    return value;
}

static inline std::string get_latest_version() {
    const CommandResult result = captureCommand({
        "curl",
        "-fsSL",
        "--connect-timeout", "5",
        "--max-time", "15",
        "-H", "User-Agent: ZPM",
        kLatestReleaseUrl
    });

    if (result.exitCode != 0 || result.output.empty()) {
        return {};
    }

    const auto tag = extractJsonString(result.output, "tag_name");
    return tag ? normalizeVersion(*tag) : std::string {};
}

static inline std::string get_installed_version() {
    FILE* f = fopen("/opt/ZPM/VERSION.txt", "r");
    if (!f) {
        return "none";
    }

    char line[64];
    std::string v;
    if (fgets(line, sizeof(line), f)) {
        v = line;
    }
    fclose(f);

    v = normalizeVersion(v);
    return v.empty() ? "none" : v;
}

static inline std::vector<int> parse_version(const std::string& ver) {
    std::string v = normalizeVersion(ver);
    const size_t dash = v.find('-');
    if (dash != std::string::npos) {
        v = v.substr(0, dash);
    }

    std::vector<int> out;
    std::stringstream ss(v);
    std::string seg;
    while (std::getline(ss, seg, '.')) {
        try {
            out.push_back(std::stoi(seg));
        } catch (...) {
            out.push_back(0);
        }
    }
    while (out.size() < 3) {
        out.push_back(0);
    }
    return out;
}

static inline bool is_newer(const std::string& latest, const std::string& current) {
    if (current == "none") {
        return true;
    }

    auto a = parse_version(latest);
    auto b = parse_version(current);

    for (size_t i = 0; i < a.size(); i++) {
        if (a[i] > b[i]) {
            return true;
        }
        if (a[i] < b[i]) {
            return false;
        }
    }
    return false;
}

static inline void checkForUpdates() {
    if (!updateInfoEnabled() || !shouldCheckForUpdates()) {
        return;
    }

    const std::string current = get_installed_version();
    const std::string latest = get_latest_version();
    saveCheckTime();

    if (latest.empty() || current == "none" || !is_newer(latest, current)) {
        return;
    }

    std::cout << CYAN << "\n====================================\n" << RESET;
    std::cout << YELLOW << "      ZPM UPDATE AVAILABLE\n" << RESET;
    std::cout << CYAN << "====================================\n\n" << RESET;
}

} // namespace zpm_update
