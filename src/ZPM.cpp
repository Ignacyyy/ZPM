
#include "main.h"

#include <cerrno>
#include <cstring>
#include <string_view>

namespace {

    struct CommandAlias {
        std::string_view name;
        std::string_view target;
    };

    struct CommandHelp {
        std::string_view names;
        std::string_view description;
    };

    struct ParsedArgs {
        int commandIndex = -1;
        int separatorIndex = -1;
        std::string error;
    };

    constexpr std::array<CommandAlias, 18> kCommandAliases {{
        {"install", "zinst"},
        {"inst", "zinst"},
        {"remove", "zrm"},
        {"rm", "zrm"},
        {"update", "zupd"},
        {"upd", "zupd"},
        {"upgrade", "zupgr"},
        {"upgr", "zupgr"},
        {"list", "zlist"},
        {"ls", "zlist"},
        {"search", "zsearch"},
        {"clean", "zclean"},
        {"info", "zinfo"},
        {"uninstall", "zuninstall"},
        {"home", "zhome"},
        {"run", "zrun"},
        {"tui", "ztui"},
        {"ztui", "ztui"},
    }};

    constexpr std::array<CommandHelp, 12> kCommandHelp {{
        {"home", "Home page of ZPM"},
        {"tui", "Run ZPM terminal UI"},
        {"update, upd", "Perform a system upgrade"},
        {"upgrade, upgr", "Upgrade ZPM itself"},
        {"install, inst", "Install package"},
        {"remove, rm", "Remove package"},
        {"list, ls", "List installed packages"},
        {"search", "Search for package"},
        {"clean", "Clean system cache"},
        {"info", "Package information"},
        {"uninstall", "Uninstall ZPM"},
        {"run", "Run programs using ZPM"},
    }};

    void showVersion() {
        std::cout << RED << "zpm component version: v" << zpm_version::version()
        << "\n" << RESET
        << "https://github.com/Zielina-Konrad-productions/ZPM\n"
        << "Copyright (c) 2026 Ignacyyy & Ry3ball\n"
        << "License: MIT\n";
    }

    void showHelp() {
        std::cout << RED << "Usage: " << RESET << "zpm <command> [options]\n";
        std::cout << RED << "Commands:" << RESET << "\n";

        for (const CommandHelp& command : kCommandHelp) {
            std::cout << "  " << std::left << std::setw(15) << command.names
            << " " << command.description << "\n";
        }

        std::cout << "\n" << RED << "Options:" << RESET << "\n"
        << "  --help, -h      Show this help message\n"
        << "  --version, -v   Show version information\n";
    }

    bool isGlobalOption(std::string_view arg) {
        return arg == "--help" || arg == "-h" || arg == "--version" || arg == "-v";
    }

    bool isOptionLike(std::string_view arg) {
        return arg.size() > 1 && arg.front() == '-';
    }

    std::string commandTarget(std::string_view command) {
        for (const CommandAlias& alias : kCommandAliases) {
            if (alias.name == command) {
                return std::string(alias.target);
            }
        }

        return {};
    }

    ParsedArgs parseArgs(int argc, char* argv[]) {
        ParsedArgs parsed;
        bool optionsEnded = false;

        for (int i = 1; i < argc; ++i) {
            const std::string_view arg(argv[i]);

            if (!optionsEnded && arg == "--") {
                parsed.separatorIndex = i;
                optionsEnded = true;
                continue;
            }

            if (!optionsEnded && isGlobalOption(arg)) {
                continue;
            }

            if (!optionsEnded && isOptionLike(arg)) {
                parsed.error = "Unknown option: " + std::string(arg);
                return parsed;
            }

            parsed.commandIndex = i;
            return parsed;
        }

        return parsed;
    }

    std::string parentDirectory(const std::string& path) {
        const std::size_t slash = path.find_last_of('/');
        if (slash == std::string::npos) {
            return {};
        }
        if (slash == 0) {
            return "/";
        }
        return path.substr(0, slash);
    }

    std::string joinPath(const std::string& directory, std::string_view filename) {
        if (directory.empty()) {
            return std::string(filename);
        }
        if (directory.back() == '/') {
            return directory + std::string(filename);
        }
        return directory + "/" + std::string(filename);
    }

    bool executableAt(const std::string& path) {
        return !path.empty() && access(path.c_str(), X_OK) == 0;
    }

    std::string executableDirectoryFromProc() {
        std::array<char, 4096> path {};
        const ssize_t length = readlink("/proc/self/exe", path.data(), path.size() - 1);
        if (length <= 0) {
            return {};
        }

        return parentDirectory(std::string(path.data(), static_cast<std::size_t>(length)));
    }

    std::string executableDirectoryFromArgv(const char* argv0) {
        if (argv0 == nullptr) {
            return {};
        }

        const std::string path(argv0);
        return path.find('/') == std::string::npos ? std::string {} : parentDirectory(path);
    }

    std::string resolveComponentExecutable(std::string_view target, const char* argv0) {
        const std::array<std::string, 3> candidateDirs {
            executableDirectoryFromProc(),
            executableDirectoryFromArgv(argv0),
            "/opt/ZPM/bin",
        };

        for (const std::string& directory : candidateDirs) {
            if (directory.empty()) {
                continue;
            }

            const std::string candidate = joinPath(directory, target);
            if (executableAt(candidate)) {
                return candidate;
            }
        }

        return std::string(target);
    }

    std::vector<std::string> buildComponentArgs(int argc,
                                                char* argv[],
                                                int commandIndex,
                                                int separatorIndex,
                                                std::string_view target) {
        std::vector<std::string> args;
        args.reserve(static_cast<std::size_t>(argc));
        args.emplace_back(target);

        for (int i = 1; i < argc; ++i) {
            if (i == commandIndex || i == separatorIndex) {
                continue;
            }
            args.emplace_back(argv[i]);
        }

        return args;
                                                }

                                                int execComponent(const std::string& executable, const std::vector<std::string>& args) {
                                                    if (executable.empty() || args.empty()) {
                                                        std::cerr << RED << "Error: Missing component executable.\n" << RESET;
                                                        return 127;
                                                    }

                                                    std::vector<char*> argv;
                                                    argv.reserve(args.size() + 1);
                                                    for (const std::string& arg : args) {
                                                        argv.push_back(const_cast<char*>(arg.c_str()));
                                                    }
                                                    argv.push_back(nullptr);

                                                    if (executable.find('/') == std::string::npos) {
                                                        execvp(executable.c_str(), argv.data());
                                                    } else {
                                                        execv(executable.c_str(), argv.data());
                                                    }

                                                    const int errorCode = errno;
                                                    std::cerr << RED << "Error: Could not start component '" << args.front() << "'";
                                                    if (executable != args.front()) {
                                                        std::cerr << " (" << executable << ")";
                                                    }
                                                    std::cerr << ": " << std::strerror(errorCode) << "\n" << RESET;

                                                    return errorCode == ENOENT ? 127 : 126;
                                                }

                                                bool hasFlag(int argc, char* argv[], std::string_view shortName, std::string_view longName) {
                                                    for (int i = 1; i < argc; ++i) {
                                                        const std::string_view arg(argv[i]);
                                                        if (arg == shortName || arg == longName) {
                                                            return true;
                                                        }
                                                    }
                                                    return false;
                                                }

} // namespace

int main(int argc, char* argv[]) {
    const ParsedArgs parsed = parseArgs(argc, argv);
    if (!parsed.error.empty()) {
        std::cerr << RED << "Error: " << parsed.error << "\n" << RESET;
        return 1;
    }

    if (parsed.commandIndex != -1) {
        const std::string_view command(argv[parsed.commandIndex]);
        const std::string target = commandTarget(command);
        if (target.empty()) {
            std::cerr << RED << "Error: Unknown command: " << command << "\n" << RESET;
            std::cerr << "Run 'zpm --help' to list all commands.\n";
            return 1;
        }

        const std::string executable = resolveComponentExecutable(target, argv[0]);
        const std::vector<std::string> componentArgs =
        buildComponentArgs(argc, argv, parsed.commandIndex, parsed.separatorIndex, target);
        return execComponent(executable, componentArgs);
    }

    const bool version = hasFlag(argc, argv, "-v", "--version");
    const bool help = hasFlag(argc, argv, "-h", "--help");

    if (version && help) {
        std::cout << YELLOW << "--version" << RESET << "\n";
        showVersion();
        std::cout << "\n" << YELLOW << "--help" << RESET << "\n";
        showHelp();
        return 0;
    }

    if (version) {
        showVersion();
        return 0;
    }

    if (help || argc == 1 || parsed.separatorIndex != -1) {
        showHelp();
        return 0;
    }

    return 0;
}
