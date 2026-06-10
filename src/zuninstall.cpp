#include "main.h"

#include <cerrno>
#include <cstring>

namespace {

constexpr const char* kInstallDir = "/opt/ZPM";
constexpr const char* kLogPath = "/var/log/zuninstall.log";

void helpmessage(const char* progName) {
    std::cout << RED << "Usage: " << RESET << progName
              << " [options] or zpm uninstall [options]\n";
    std::cout << RED << "Options:\n" << RESET;
    std::cout << "  -h, --help           Show help\n";
    std::cout << "  -v, --version        Show version\n";
}

void versionmessage() {
    std::cout << RED << "zuninstall component version: v"
              << zpm_version::version() << " of ZPM\n" << RESET;
    std::cout << "https://github.com/Zielina-Konrad-productions/ZPM\n";
    std::cout << "Copyright (c) 2026 Ignacyyy & Ry3ball\n";
    std::cout << "License: MIT\n";
}

void logLine(const std::string& line) {
    std::ofstream log(kLogPath, std::ios::app);
    if (log.is_open()) {
        log << line << '\n';
    }
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

bool removeZpmSymlink(const std::filesystem::path& path) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec || !std::filesystem::is_symlink(status)) {
        return true;
    }

    const std::filesystem::path target = std::filesystem::read_symlink(path, ec);
    if (ec) {
        logLine("cannot read symlink " + path.string() + ": " + ec.message());
        return false;
    }

    if (!startsWith(target.string(), std::string(kInstallDir) + "/bin/")) {
        logLine("skipping non-ZPM symlink " + path.string() + " -> " + target.string());
        return true;
    }

    std::filesystem::remove(path, ec);
    if (ec) {
        logLine("cannot remove symlink " + path.string() + ": " + ec.message());
        return false;
    }

    logLine("removed symlink " + path.string());
    return true;
}

bool removeKnownSymlinks() {
    const std::vector<std::string> commands = {
        "zclean",
        "zhome",
        "zinfo",
        "zinst",
        "zlist",
        "zpm",
        "zrm",
        "zrun",
        "zsearch",
        "zuninstall",
        "zupd",
        "zupgr"
    };

    bool ok = true;
    for (const std::filesystem::path directory : {"/usr/bin", "/usr/local/bin"}) {
        for (const std::string& command : commands) {
            ok = removeZpmSymlink(directory / command) && ok;
        }
    }
    return ok;
}

bool removeInstallDir() {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(kInstallDir, ec);
    if (ec) {
        if (ec == std::errc::no_such_file_or_directory) {
            return true;
        }
        logLine("cannot inspect install dir: " + ec.message());
        return false;
    }

    if (std::filesystem::is_symlink(status)) {
        logLine("refusing to remove symlink install dir");
        return false;
    }

    std::filesystem::remove_all(kInstallDir, ec);
    if (ec) {
        logLine("cannot remove install dir: " + ec.message());
        return false;
    }

    logLine("removed install dir");
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    bool showHelp = false;
    bool showVersion = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--version" || arg == "-v") {
            showVersion = true;
        } else if (arg == "--help" || arg == "-h") {
            showHelp = true;
        } else {
            std::cerr << RED << "Error: Unknown option: " << arg << RESET << "\n";
            return 1;
        }
    }

    if (showVersion && showHelp) {
        std::cout << YELLOW << "--version" << RESET << "\n";
        versionmessage();
        std::cout << "\n" << YELLOW << "--help" << RESET << "\n";
        helpmessage(argv[0]);
        return 0;
    }

    if (showVersion) {
        versionmessage();
        return 0;
    }
    if (showHelp) {
        helpmessage(argv[0]);
        return 0;
    }

    if (geteuid() != 0) {
        std::cout << RED << "Run with sudo!\n" << RESET;
        return 1;
    }

    std::ofstream log(kLogPath, std::ios::trunc);
    if (log.is_open()) {
        log << "-------------------uninstalling ZPM----------------------\n";
    }

    std::cout << RED << "ZPM Uninstall program," << RESET;
    std::cout << " continue? [y/n]: ";

    std::string answer;
    std::getline(std::cin, answer);

    if (answer != "y" && answer != "Y") {
        std::cout << YELLOW << "Uninstall canceled.\n" << RESET;
        return 0;
    }

    std::cout << "\n" << RED << "Uninstalling...\n" << RESET;
    progressbar_start(0.0f, "0/3 | Starting...");

    const bool linksOk = removeKnownSymlinks();
    progressbar_update(60.0f, "1/3 | Removing files...");

    const bool installOk = removeInstallDir();
    progressbar_update(90.0f, "2/3 | Cleaning...");

    std::error_code ec;
    std::filesystem::remove("/etc/profile.d/ZPM.sh", ec);
    const bool profileOk = !ec || ec == std::make_error_code(std::errc::no_such_file_or_directory);
    if (!profileOk) {
        logLine("cannot remove /etc/profile.d/ZPM.sh: " + ec.message());
    }

    if (linksOk && installOk && profileOk) {
        progressbar_finish("3/3 | DONE!");
        std::cout << YELLOW << "[RAPORT]" << RESET << " " << kLogPath << "\n";
        return 0;
    }

    progressbar_finish("3/3 | DONE WITH WARNINGS!");
    std::cout << YELLOW << "[RAPORT]" << RESET << " " << kLogPath << "\n";
    return 1;
}
