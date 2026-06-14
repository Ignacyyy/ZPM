#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <utility>
#include <unistd.h>
#include <vector>

namespace {

using namespace ftxui;

struct Action {
    std::string title;
    std::string commandPreview;
    std::vector<std::string> args;
    std::string hint;
    bool needsRoot = false;
    bool exits = false;
    std::string warning;
};

struct Category {
    std::string title;
    std::string hint;
    std::vector<Action> actions;
};

struct SystemStatus {
    std::string distro = "unknown";
    std::string backend = "unknown";
    std::string zpmVersion = "unknown";
    std::string privileges = "root";
};

bool executableAt(const std::string& path) {
    return !path.empty() && access(path.c_str(), X_OK) == 0;
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

std::string executableDirectoryFromProc() {
    std::vector<char> path(4096);
    const ssize_t length = readlink("/proc/self/exe", path.data(), path.size() - 1);
    if (length <= 0) {
        return {};
    }
    return parentDirectory(std::string(path.data(), static_cast<std::size_t>(length)));
}

std::string resolveExecutable(const std::string& name) {
    if (name.find('/') != std::string::npos) {
        return name;
    }

    const std::string exeDir = executableDirectoryFromProc();
    const std::string projectDir = parentDirectory(exeDir);
    const std::vector<std::string> candidateDirs {
        exeDir,
        joinPath(projectDir, "bin"),
        "/opt/ZPM/bin",
        "/usr/bin",
        "/bin",
    };

    for (const std::string& directory : candidateDirs) {
        const std::string candidate = joinPath(directory, name);
        if (executableAt(candidate)) {
            return candidate;
        }
    }

    return name;
}

std::string trim(std::string value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string stripQuotes(std::string value) {
    value = trim(std::move(value));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

std::optional<std::string> osReleaseValue(std::string_view key) {
    std::ifstream file("/etc/os-release");
    std::string line;
    while (std::getline(file, line)) {
        const std::size_t equal = line.find('=');
        if (equal == std::string::npos) {
            continue;
        }
        if (line.substr(0, equal) == key) {
            return stripQuotes(line.substr(equal + 1));
        }
    }
    return std::nullopt;
}

std::string readVersionFile(const std::string& path) {
    std::ifstream file(path);
    std::string version;
    file >> version;
    return version;
}

std::string detectPackageBackend() {
    if (executableAt("/usr/bin/apt-get") || executableAt("/bin/apt-get")) {
        return "apt";
    }
    if (executableAt("/usr/bin/dnf5")) {
        return "dnf5";
    }
    if (executableAt("/usr/bin/dnf")) {
        return "dnf";
    }
    if (executableAt("/usr/bin/zypper")) {
        return "zypper";
    }
    return "unknown";
}

SystemStatus collectSystemStatus() {
    SystemStatus status;
    if (const std::optional<std::string> prettyName = osReleaseValue("PRETTY_NAME")) {
        status.distro = *prettyName;
    } else if (const std::optional<std::string> name = osReleaseValue("NAME")) {
        status.distro = *name;
    }
    status.backend = detectPackageBackend();

    const std::string exeDir = executableDirectoryFromProc();
    const std::string installedVersion = readVersionFile("/opt/ZPM/VERSION.txt");
    const std::string localVersion = readVersionFile("VERSION.txt");
    const std::string relativeVersion =
        exeDir.empty() ? std::string {} : readVersionFile(joinPath(parentDirectory(exeDir), "VERSION.txt"));

    if (!installedVersion.empty()) {
        status.zpmVersion = installedVersion;
    } else if (!localVersion.empty()) {
        status.zpmVersion = localVersion;
    } else if (!relativeVersion.empty()) {
        status.zpmVersion = relativeVersion;
    }

    status.privileges = geteuid() == 0 ? "root" : "not root";
    return status;
}

Element appTheme(Element element) {
    return element | color(Color::White) | bgcolor(Color::Black);
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

int runRealCommand(Action action) {
    std::vector<std::string> args = std::move(action.args);
    if (args.empty()) {
        return 0;
    }

    args.front() = resolveExecutable(args.front());

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (std::string& arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    std::cout << "$ " << action.commandPreview << "\n";
    std::cout.flush();

    const pid_t pid = fork();
    if (pid < 0) {
        const int errorCode = errno;
        std::cerr << "Could not start command '" << action.commandPreview
                  << "': " << std::strerror(errorCode) << "\n";
        return 127;
    }

    if (pid == 0) {
        if (args.front().find('/') != std::string::npos) {
            execv(args.front().c_str(), argv.data());
        } else {
            execvp(args.front().c_str(), argv.data());
        }

        const int errorCode = errno;
        std::cerr << "Could not start command '" << action.commandPreview
                  << "': " << std::strerror(errorCode) << "\n";
        _exit(errorCode == ENOENT ? 127 : 126);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        std::cerr << "Could not wait for command '" << action.commandPreview
                  << "': " << std::strerror(errno) << "\n";
        return 127;
    }

    return decodeExitStatus(status);
}

bool askReturnToTui(int exitCode) {
    while (true) {
        std::cout << "\nCommand exited with code " << exitCode << ".\n"
                  << "[b] back to ZPM TUI, [q] quit: ";
        std::cout.flush();

        std::string answer;
        if (!std::getline(std::cin, answer)) {
            return false;
        }
        if (answer.empty()) {
            continue;
        }

        const char choice = static_cast<char>(
            std::tolower(static_cast<unsigned char>(answer.front())));
        if (choice == 'b') {
            return true;
        }
        if (choice == 'q') {
            return false;
        }
    }
}

std::vector<std::string> promptCommandArgs(const std::string& prompt,
                                           const std::string& command) {
    const std::string script =
        "printf '" + prompt + ": '; "
        "IFS= read -r value; "
        "if [ -z \"$value\" ]; then echo 'No input provided.'; exit 1; fi; "
        "printf '$ " + command + " %s\\n' \"$value\"; "
        "exec " + command + " \"$value\"";

    return {"sh", "-lc", script};
}

std::vector<std::string> shellArgs(const std::string& script) {
    return {"sh", "-lc", script};
}

std::vector<std::string> logViewerArgs(const std::string& path) {
    return shellArgs(
        "if [ ! -r '" + path + "' ]; then "
        "echo 'Log file not found or not readable: " + path + "'; exit 1; "
        "fi; "
        "pager=${PAGER:-less}; "
        "if command -v \"$pager\" >/dev/null 2>&1; then exec \"$pager\" '" + path + "'; fi; "
        "exec cat '" + path + "'"
    );
}

std::vector<std::string> editConfigArgs() {
    return shellArgs(
        "editor=${EDITOR:-nano}; "
        "if ! command -v \"$editor\" >/dev/null 2>&1; then editor=vi; fi; "
        "exec \"$editor\" /opt/ZPM/zielina.conf"
    );
}

std::vector<Category> buildCategories(const SystemStatus& status) {
    std::vector<Category> categories {
        {
            "Welcome",
            "Advanced guide for navigating and safely launching ZPM tasks.",
            {
                {
                    "Navigation guide",
                    "",
                    {},
                    "This page explains the recommended workflow for using ZPM "
                    "TUI without hiding what happens in the terminal.",
                    false,
                },
            },
        },
        {
            "System update",
            "ZPM system update tasks for packages handled by the detected backend.",
            {
                {
                    "Normal update",
                    "zpm update --yes",
                    {"zpm", "update", "--yes"},
                    "Runs the standard ZPM system update flow. ZPM detects apt, "
                    "dnf, or zypper and starts the normal package update.",
                    true,
                },
                {
                    "Full system update",
                    "zpm update --full --yes",
                    {"zpm", "update", "--full", "--yes"},
                    "Runs the fuller ZPM system update flow, such as dist-upgrade "
                    "or the matching backend equivalent when supported.",
                    true,
                },
                {
                    "Full update dry-run",
                    "zpm update --full --dry-run",
                    {"zpm", "update", "--full", "--dry-run"},
                    "Simulates the full ZPM system update flow without "
                    "modifying packages.",
                    false,
                },
                {
                    "Update and reboot",
                    "zpm update --yes --reboot",
                    {"zpm", "update", "--yes", "--reboot"},
                    "Runs the standard ZPM system update flow and requests a "
                    "reboot after the update completes.",
                    true,
                    false,
                    "This action can reboot the machine after updates finish.",
                },
                {
                    "Update and shutdown",
                    "zpm update --yes --shutdown",
                    {"zpm", "update", "--yes", "--shutdown"},
                    "Runs the standard ZPM system update flow and requests a "
                    "shutdown after the update completes.",
                    true,
                    false,
                    "This action can shut down the machine after updates finish.",
                },
                {
                    "Dry-run update",
                    "zpm update --dry-run",
                    {"zpm", "update", "--dry-run"},
                    "Simulates the update flow and shows the plan without "
                    "modifying packages.",
                    false,
                },
            },
        },
        {
            "Package management",
            "Install, remove, search, and inspect packages through ZPM.",
            {
                {
                    "Install package",
                    "zpm install <package>",
                    promptCommandArgs("Package to install", "zpm install"),
                    "Closes the TUI, asks for a package name, and runs the real "
                    "ZPM install command in the terminal.",
                    true,
                },
                {
                    "Install package dry-run",
                    "zpm install --dry-run <package>",
                    promptCommandArgs("Package to simulate installing", "zpm install --dry-run"),
                    "Closes the TUI, asks for a package name, and simulates "
                    "the ZPM install command without changing packages.",
                    false,
                },
                {
                    "Remove package",
                    "zpm remove <package>",
                    promptCommandArgs("Package to remove", "zpm remove"),
                    "Closes the TUI, asks for a package name, and runs the real "
                    "ZPM remove command in the terminal.",
                    true,
                },
                {
                    "Remove package purge (APT only)",
                    "zpm remove --purge <package>",
                    promptCommandArgs("Package to purge", "zpm remove --purge"),
                    "Closes the TUI, asks for a package name, and runs APT purge "
                    "through ZPM. This option only applies to APT-based systems.",
                    true,
                    false,
                    "APT purge can remove package configuration files.",
                },
                {
                    "Remove package dry-run",
                    "zpm remove --dry-run <package>",
                    promptCommandArgs("Package to simulate removing", "zpm remove --dry-run"),
                    "Closes the TUI, asks for a package name, and simulates "
                    "the ZPM remove command without changing packages.",
                    false,
                },
                {
                    "Search package",
                    "zpm search <query>",
                    promptCommandArgs("Search query", "zpm search"),
                    "Closes the TUI, asks for a search query, and runs the real "
                    "ZPM search command in the terminal.",
                    false,
                },
                {
                    "Package info",
                    "zpm info <package>",
                    promptCommandArgs("Package to inspect", "zpm info"),
                    "Closes the TUI, asks for a package name, and runs the real "
                    "ZPM info command in the terminal.",
                    false,
                },
                {
                    "Package list",
                    "zpm list --no-pager",
                    {"zpm", "list", "--no-pager"},
                    "Shows installed packages without a pager, so the command "
                    "prints directly in the terminal after launch.",
                    false,
                },
                {
                    "Native package list",
                    "zpm list --native --no-pager",
                    {"zpm", "list", "--native", "--no-pager"},
                    "Shows installed native packages without a pager.",
                    false,
                },
                {
                    "Flatpak package list",
                    "zpm list --flatpak --no-pager",
                    {"zpm", "list", "--flatpak", "--no-pager"},
                    "Shows installed Flatpak packages without a pager.",
                    false,
                },
                {
                    "Snap package list",
                    "zpm list --snap --no-pager",
                    {"zpm", "list", "--snap", "--no-pager"},
                    "Shows installed Snap packages without a pager.",
                    false,
                },
            },
        },
        {
            "ZPM upgrade",
            "Upgrade and test the installed Zielina Package Manager.",
            {
                {
                    "Upgrade ZPM",
                    "zpm upgrade",
                    {"zpm", "upgrade"},
                    "Checks for Zielina Package Manager updates. After selection, "
                    "the TUI exits so any zupgr prompts appear directly in the "
                    "terminal.",
                    true,
                },
                {
                    "Force ZPM upgrade",
                    "zpm upgrade --force",
                    {"zpm", "upgrade", "--force"},
                    "Forces the ZPM upgrade flow even when the installed version "
                    "appears to be current.",
                    true,
                    false,
                    "Force upgrade can reinstall or replace the current ZPM build.",
                },
                {
                    "Experimental ZPM upgrade",
                    "zpm upgrade --experimental",
                    {"zpm", "upgrade", "--experimental"},
                    "Runs the experimental ZPM upgrade flow for prerelease "
                    "versions when available.",
                    true,
                },
                {
                    "Force experimental ZPM upgrade",
                    "zpm upgrade --experimental --force",
                    {"zpm", "upgrade", "--experimental", "--force"},
                    "Forces the experimental ZPM upgrade flow for prerelease "
                    "versions when available.",
                    true,
                    false,
                    "This forces a prerelease upgrade path. Review before running.",
                },
                {
                    "Experimental dry-run upgrade",
                    "zpm upgrade --experimental --dry-run",
                    {"zpm", "upgrade", "--experimental", "--dry-run"},
                    "Simulates the experimental ZPM upgrade flow without changing files.",
                    false,
                },
                {
                    "Force experimental dry-run upgrade",
                    "zpm upgrade --experimental --force --dry-run",
                    {"zpm", "upgrade", "--experimental", "--force", "--dry-run"},
                    "Simulates the forced experimental ZPM upgrade flow without changing files.",
                    false,
                },
                {
                    "Dry-run upgrade",
                    "zpm upgrade --dry-run",
                    {"zpm", "upgrade", "--dry-run"},
                    "Simulates the ZPM upgrade without changing files.",
                    false,
                },
            },
        },
        {
            "Maintenance",
            "Cache cleanup and system housekeeping.",
            {
                {
                    "Clean cache",
                    "zpm clean",
                    {"zpm", "clean"},
                    "Cleans caches and leftovers from package operations.",
                    true,
                    false,
                    "Clean can delete cached package data and leftover files.",
                },
                {
                    "Dry-run clean",
                    "zpm clean --dry-run",
                    {"zpm", "clean", "--dry-run"},
                    "Simulates cache cleanup without deleting files.",
                    false,
                },
            },
        },
        {
            "Logs",
            "Open recent ZPM operation logs with the configured pager.",
            {
                {
                    "Package install log",
                    "less /tmp/zinst.log",
                    logViewerArgs("/tmp/zinst.log"),
                    "Opens the latest package installation log.",
                    false,
                },
                {
                    "Package removal log",
                    "less /tmp/zrm.log",
                    logViewerArgs("/tmp/zrm.log"),
                    "Opens the latest package removal log.",
                    false,
                },
                {
                    "System update log",
                    "less /tmp/zupd.log",
                    logViewerArgs("/tmp/zupd.log"),
                    "Opens the latest system update log.",
                    false,
                },
                {
                    "System patch-check log",
                    "less /tmp/zupd_patchcheck.log",
                    logViewerArgs("/tmp/zupd_patchcheck.log"),
                    "Opens the latest system update patch-check log.",
                    false,
                },
                {
                    "ZPM upgrade log",
                    "less /tmp/zupgr.log",
                    logViewerArgs("/tmp/zupgr.log"),
                    "Opens the latest ZPM upgrade log.",
                    false,
                },
                {
                    "Clean log",
                    "less /tmp/zclean.log",
                    logViewerArgs("/tmp/zclean.log"),
                    "Opens the latest cleanup log.",
                    false,
                },
                {
                    "ZPM uninstall log",
                    "less /var/log/zuninstall.log",
                    logViewerArgs("/var/log/zuninstall.log"),
                    "Opens the latest ZPM uninstall log.",
                    false,
                },
                {
                    "Manual installer log",
                    "less /var/log/ZPM_INSTALL.log",
                    logViewerArgs("/var/log/ZPM_INSTALL.log"),
                    "Opens the manual ZPM installer log.",
                    false,
                },
                {
                    "Internet installer log",
                    "less /var/log/ZPM_INETINSTALL.log",
                    logViewerArgs("/var/log/ZPM_INETINSTALL.log"),
                    "Opens the internet ZPM installer log.",
                    false,
                },
            },
        },
        {
            "Information",
            "Read-only views that do not change system configuration.",
            {
                {
                    "Homepage",
                    "zhome",
                    {"zhome"},
                    "Opens the ZPM homepage/help interface through zhome.",
                    false,
                },
                {
                    "Main help",
                    "zpm --help",
                    {"zpm", "--help"},
                    "Shows quick help for the main zpm wrapper.",
                    false,
                },
                {
                    "ZPM version",
                    "zpm --version",
                    {"zpm", "--version"},
                    "Shows the installed ZPM version information.",
                    false,
                },
                {
                    "Show config",
                    "cat /opt/ZPM/zielina.conf",
                    {"cat", "/opt/ZPM/zielina.conf"},
                    "Prints the active ZPM configuration file.",
                    false,
                },
                {
                    "Edit config",
                    "$EDITOR /opt/ZPM/zielina.conf",
                    editConfigArgs(),
                    "Opens the active ZPM configuration file in $EDITOR, "
                    "falling back to nano or vi.",
                    true,
                    false,
                    "Editing config can change ZPM behavior.",
                },
                {
                    "Exit",
                    "",
                    {},
                    "Closes the interface.",
                    false,
                    true,
                },
            },
        },
    };

    if (status.backend != "apt") {
        for (Category& category : categories) {
            if (category.title != "Package management") {
                continue;
            }
            category.actions.erase(
                std::remove_if(category.actions.begin(),
                               category.actions.end(),
                               [](const Action& action) {
                                   return action.title == "Remove package purge (APT only)";
                               }),
                category.actions.end());
        }
    }

    return categories;
}

Element renderMenu(Component categoryMenu,
                   Component actionMenu,
                   const SystemStatus& status,
                   const std::vector<Category>& categories,
                   int selectedCategory,
                   int selectedAction,
                   int focusedPane) {
    const Category& category = categories[static_cast<std::size_t>(selectedCategory)];
    const Action& action = category.actions[static_cast<std::size_t>(selectedAction)];

    std::vector<Element> details {
        text(category.title) | bold | color(Color::CyanLight),
        paragraphAlignLeft(category.hint),
        separator(),
    };

    if (category.title == "Welcome") {
        details.push_back(text("Navigation") | bold | color(Color::GreenLight));
        details.push_back(paragraphAlignLeft(
            "Use Left/Right or Tab to switch between Categories and Actions. "
            "Use Up/Down to move inside the focused panel. Enter selects the "
            "highlighted category or action."));
        details.push_back(separator());
        details.push_back(text("Command flow") | bold | color(Color::GreenLight));
        details.push_back(paragraphAlignLeft(
            "Actions with a command preview open a confirmation screen first. "
            "Press Enter or r to run, Esc or c to cancel, and q to quit the TUI."));
        details.push_back(separator());
        details.push_back(text("After a command") | bold | color(Color::GreenLight));
        details.push_back(paragraphAlignLeft(
            "The interface closes before launching the real command, so output, "
            "prompts, passwords, and errors are shown directly by the terminal. "
            "When the command finishes, press b to return to ZPM TUI or q to exit."));
        details.push_back(separator());
        details.push_back(text("Safe workflow") | bold | color(Color::GreenLight));
        details.push_back(paragraphAlignLeft(
            "For risky operations, start with a dry-run action when available. "
            "Check the command preview before confirming updates, removals, "
            "purges, forced upgrades, reboot, or shutdown actions."));
        details.push_back(separator());
    }

    details.push_back(text(action.title) | bold);

    if (!action.commandPreview.empty()) {
        details.push_back(text(action.commandPreview) | color(Color::Yellow));
        details.push_back(separator());
    }

    details.push_back(paragraphAlignLeft(action.hint));
    if (!action.args.empty()) {
        details.push_back(separator());
        details.push_back(
            paragraphAlignLeft("After confirmation, this interface closes and runs "
                               "the real command in this terminal.")
            | color(Color::Yellow));
    } else {
        details.push_back(separator());
        details.push_back(
            paragraphAlignLeft("This is an information page. Select another "
                               "category to run ZPM tasks.")
            | color(Color::Yellow));
    }

    if (action.needsRoot) {
        details.push_back(separator());
        details.push_back(
            paragraphAlignLeft("This action performs system operations. The TUI "
                               "is running as root, so the command will receive "
                               "the required privileges.")
            | color(Color::GreenLight));
    }

    details.push_back(separator());
    details.push_back(text("System status") | bold);
    details.push_back(text("Distro") | dim);
    details.push_back(paragraphAlignLeft(status.distro));
    details.push_back(text("Package backend") | dim);
    details.push_back(text(status.backend));
    details.push_back(text("ZPM version") | dim);
    details.push_back(text(status.zpmVersion));
    details.push_back(text("Privileges") | dim);
    details.push_back(text(status.privileges) | color(status.privileges == "root"
                                                          ? Color::GreenLight
                                                          : Color::RedLight));

    return appTheme(vbox({
               hbox({
                   text("ZPM TUI") | bold | color(Color::CyanLight),
                   filler(),
                   text("Left/Right or Tab: panel  Enter: select  q/Esc: exit") | dim,
               }),
               separator(),
               hbox({
                   vbox({
                       text(focusedPane == 0 ? "> Categories" : "Categories") | bold,
                       separator(),
                       categoryMenu->Render() | vscroll_indicator | frame | flex,
                   }) | size(WIDTH, EQUAL, 24),
                   separator(),
                   vbox({
                       text(focusedPane == 1 ? "> Actions" : "Actions") | bold,
                       separator(),
                       actionMenu->Render() | vscroll_indicator | frame | flex,
                   }) | size(WIDTH, EQUAL, 28),
                   separator(),
                   vbox(details) | flex,
               }) | flex,
           }) |
           border);
}

Element renderConfirmation(const Action& action) {
    std::vector<Element> content {
        hbox({
            text("Confirm command") | bold | color(Color::CyanLight),
            filler(),
            text("Enter/r: run  Esc/c: cancel  q: exit") | dim,
        }),
        separator(),
        text(action.title) | bold,
    };

    if (!action.commandPreview.empty()) {
        content.push_back(text("$ " + action.commandPreview) | color(Color::Yellow));
    }

    content.push_back(separator());
    content.push_back(paragraphAlignLeft(action.hint));

    if (action.needsRoot) {
        content.push_back(separator());
        content.push_back(paragraphAlignLeft("This command may change the system. "
                                             "Review it before running.")
                          | color(Color::RedLight));
    }

    if (!action.warning.empty()) {
        content.push_back(separator());
        content.push_back(paragraphAlignLeft(action.warning) | color(Color::RedLight) | bold);
    }

    content.push_back(separator());
    content.push_back(hbox({
        text("[ Run ]") | bold | color(Color::GreenLight),
        text("  "),
        text("[ Cancel ]") | color(Color::Yellow),
    }));

    return appTheme(vbox(content) | border);
}

} // namespace

int main() {
    if (geteuid() != 0) {
        std::cerr << "ZPM TUI requires administrator privileges.\n"
                  << "Run it again with sudo.\n";
        return 1;
    }

    const SystemStatus systemStatus = collectSystemStatus();
    const std::vector<Category> categories = buildCategories(systemStatus);
    int selectedCategory = 0;
    int selectedAction = 0;
    int focusedPane = 0;

    while (true) {
        Action selectedLaunch;
        Action pendingAction;
        bool shouldLaunch = false;

        {
            auto screen = ScreenInteractive::Fullscreen();

            std::vector<std::string> categoryEntries;
            categoryEntries.reserve(categories.size());
            for (const Category& category : categories) {
                categoryEntries.push_back(category.title);
            }

            std::vector<std::string> actionEntries;
            bool showConfirm = false;

            auto syncActionEntries = [&] {
                actionEntries.clear();
                const Category& category = categories[static_cast<std::size_t>(selectedCategory)];
                actionEntries.reserve(category.actions.size());
                for (const Action& action : category.actions) {
                    actionEntries.push_back(action.title);
                }

                if (selectedAction >= static_cast<int>(actionEntries.size())) {
                    selectedAction = actionEntries.empty()
                                         ? 0
                                         : static_cast<int>(actionEntries.size()) - 1;
                }
            };
            syncActionEntries();

            MenuOption categoryOption = MenuOption::VerticalAnimated();
            categoryOption.on_change = [&] {
                selectedAction = 0;
                syncActionEntries();
            };
            categoryOption.on_enter = [&] {
                focusedPane = 1;
            };

            MenuOption actionOption = MenuOption::VerticalAnimated();
            actionOption.on_enter = [&] {
                const Category& category = categories[static_cast<std::size_t>(selectedCategory)];
                const Action& action = category.actions[static_cast<std::size_t>(selectedAction)];
                if (action.exits) {
                    screen.Exit();
                    return;
                }
                if (action.args.empty()) {
                    return;
                }
                pendingAction = action;
                showConfirm = true;
            };

            Component categoryMenu = Menu(&categoryEntries, &selectedCategory, categoryOption);
            Component actionMenu = Menu(&actionEntries, &selectedAction, actionOption);
            Component menuControls = Container::Horizontal({categoryMenu, actionMenu}, &focusedPane);
            Component menuView = Renderer(menuControls, [&] {
                syncActionEntries();
                return renderMenu(categoryMenu,
                                  actionMenu,
                                  systemStatus,
                                  categories,
                                  selectedCategory,
                                  selectedAction,
                                  focusedPane);
            });

            Component rootRenderer = Renderer(menuView, [&] {
                return showConfirm ? renderConfirmation(pendingAction) : menuView->Render();
            });

            Component root = CatchEvent(rootRenderer, [&](Event event) {
                if (showConfirm) {
                    if (event == Event::Return || event == Event::Character('r')) {
                        selectedLaunch = pendingAction;
                        shouldLaunch = true;
                        screen.Exit();
                        return true;
                    }
                    if (event == Event::Escape || event == Event::Character('c')) {
                        showConfirm = false;
                        return true;
                    }
                    if (event == Event::Character('q')) {
                        screen.Exit();
                        return true;
                    }
                    return true;
                }

                if (event == Event::Escape || event == Event::Character('q')) {
                    screen.Exit();
                    return true;
                }
                if (event == Event::ArrowRight || event == Event::Tab) {
                    focusedPane = 1;
                    return true;
                }
                if (event == Event::ArrowLeft) {
                    focusedPane = 0;
                    return true;
                }
                return false;
            });

            screen.Loop(root);
        }

        if (!shouldLaunch) {
            return 0;
        }

        const int exitCode = runRealCommand(std::move(selectedLaunch));
        if (!askReturnToTui(exitCode)) {
            return exitCode;
        }
    }
}
