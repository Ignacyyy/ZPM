#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

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

    Action() = default;

    Action(std::string titleValue,
           std::string commandPreviewValue,
           std::vector<std::string> argsValue,
           std::string hintValue,
           bool needsRootValue = false,
           bool exitsValue = false,
           std::string warningValue = {})
        : title(std::move(titleValue)),
          commandPreview(std::move(commandPreviewValue)),
          args(std::move(argsValue)),
          hint(std::move(hintValue)),
          needsRoot(needsRootValue),
          exits(exitsValue),
          warning(std::move(warningValue)) {}
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

std::string shellQuote(std::string_view value) {
    std::string quoted = "'";
    for (const char character : value) {
        if (character == '\'') {
            quoted += "'\\''";
        } else {
            quoted += character;
        }
    }
    quoted += "'";
    return quoted;
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

Element muted(std::string value) {
    return text(std::move(value)) | color(Color::GrayDark);
}

Element titleText(std::string value) {
    return text(std::move(value)) | bold | color(Color::CyanLight);
}

Element statusChip(std::string label, std::string value, Color valueColor) {
    return hbox({
        text(" " + std::move(label) + " ") | dim,
        text(" " + std::move(value) + " ") | bold | color(valueColor),
    }) | borderStyled(Color::GrayDark);
}

Element panel(std::string title, Element body, bool focused = false) {
    const Color borderColor = focused ? Color::CyanLight : Color::GrayDark;
    return vbox({
               hbox({
                   text(focused ? "> " : "  ") | color(Color::CyanLight),
                   text(std::move(title)) | bold,
               }),
               separatorStyled(LIGHT),
               std::move(body) | flex,
           }) |
           borderStyled(ROUNDED, borderColor);
}

Element sectionTitle(std::string value) {
    return text(std::move(value)) | bold | color(Color::GreenLight);
}

Element keyHint(std::string key, std::string label) {
    return hbox({
        text(" " + std::move(key) + " ") | bold | color(Color::Black) | bgcolor(Color::CyanLight),
        text(" " + std::move(label)) | dim,
    });
}

int clampIndex(int index, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    return std::clamp(index, 0, static_cast<int>(size) - 1);
}

Element actionBadge(const Action& action) {
    if (action.exits) {
        return text(" EXIT ") | bold | color(Color::Black) | bgcolor(Color::RedLight);
    }
    if (action.needsRoot) {
        return text(" ROOT ") | bold | color(Color::Black) | bgcolor(Color::Yellow);
    }
    if (action.args.empty()) {
        return text(" INFO ") | bold | color(Color::Black) | bgcolor(Color::CyanLight);
    }
    return text(" SAFE ") | bold | color(Color::Black) | bgcolor(Color::GreenLight);
}

Element commandPreviewBox(const std::string& command) {
    if (command.empty()) {
        return vbox({
                   muted("No command is attached to this entry."),
               }) |
               borderStyled(Color::GrayDark);
    }

    return vbox({
               muted("Command preview"),
               paragraphAlignLeft("$ " + command) | color(Color::Yellow),
           }) |
           borderStyled(Color::Yellow);
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
        "printf '%s: ' " + shellQuote(prompt) + "; "
        "IFS= read -r value; "
        "if [ -z \"$value\" ]; then echo 'No input provided.'; exit 1; fi; "
        "set -f; "
        "IFS=' \t'; "
        "set -- $value; "
        "if [ \"$#\" -eq 0 ]; then echo 'No input provided.'; exit 1; fi; "
        "printf '%s' " + shellQuote("$ " + command) + "; "
        "for arg do printf ' %s' \"$arg\"; done; "
        "printf '\\n'; "
        "exec " + command + " \"$@\"";

    return {"sh", "-lc", script};
}

std::vector<std::string> shellArgs(const std::string& script) {
    return {"sh", "-lc", script};
}

std::vector<std::string> editConfigArgs() {
    return shellArgs(
        "choose_editor() { "
        "for candidate in \"$SUDO_EDITOR\" \"$VISUAL\" \"$EDITOR\" sensible-editor editor vim vi; do "
        "[ -n \"$candidate\" ] || continue; "
        "set -f; set -- $candidate; "
        "[ \"$#\" -gt 0 ] || continue; "
        "if command -v \"$1\" >/dev/null 2>&1; then printf '%s\\n' \"$candidate\"; return 0; fi; "
        "done; "
        "return 1; "
        "}; "
        "editor=$(choose_editor) || { echo 'Error: no terminal text editor found!'; exit 1; }; "
        "set -f; "
        "exec $editor /opt/ZPM/zielina.conf"
    );
}

std::vector<Category> buildCategories(const SystemStatus& status) {
    std::vector<Action> packageChanges {
        {
            "Install package",
            "zpm install <packages...>",
            promptCommandArgs("Packages to install", "zpm install"),
            "Closes the TUI, asks for package names, and runs the real "
            "ZPM install command in the terminal.",
            true,
        },
        {
            "Remove package",
            "zpm remove <packages...>",
            promptCommandArgs("Packages to remove", "zpm remove"),
            "Closes the TUI, asks for package names, and runs the real "
            "ZPM remove command in the terminal.",
            true,
        },
    };

    if (status.backend == "apt") {
        packageChanges.push_back({
            "Remove package purge",
            "zpm remove --purge <packages...>",
            promptCommandArgs("Packages to purge", "zpm remove --purge"),
            "Closes the TUI, asks for package names, and runs APT purge "
            "through ZPM.",
            true,
            false,
            "APT purge can remove package configuration files.",
        });
    }

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
            "Dry runs",
            "Preview package, update, cleanup, and upgrade operations without changing the system.",
            {
                {
                    "Update dry-run",
                    "zpm update --dry-run",
                    {"zpm", "update", "--dry-run"},
                    "Simulates the standard update flow and shows the plan without "
                    "modifying packages.",
                    false,
                },
                {
                    "Full update dry-run",
                    "zpm update --full --dry-run",
                    {"zpm", "update", "--full", "--dry-run"},
                    "Simulates the fuller update flow, such as dist-upgrade or the "
                    "matching backend equivalent when supported.",
                    false,
                },
                {
                    "Install package dry-run",
                    "zpm install --dry-run <packages...>",
                    promptCommandArgs("Packages to simulate installing", "zpm install --dry-run"),
                    "Closes the TUI, asks for package names, and simulates "
                    "the ZPM install command without changing packages.",
                    false,
                },
                {
                    "Remove package dry-run",
                    "zpm remove --dry-run <packages...>",
                    promptCommandArgs("Packages to simulate removing", "zpm remove --dry-run"),
                    "Closes the TUI, asks for package names, and simulates "
                    "the ZPM remove command without changing packages.",
                    false,
                },
                {
                    "Clean dry-run",
                    "zpm clean --dry-run",
                    {"zpm", "clean", "--dry-run"},
                    "Simulates cache cleanup without deleting files.",
                    false,
                },
                {
                    "ZPM upgrade dry-run",
                    "zpm upgrade --dry-run",
                    {"zpm", "upgrade", "--dry-run"},
                    "Simulates the ZPM upgrade without changing files.",
                    false,
                },
                {
                    "Experimental upgrade dry-run",
                    "zpm upgrade --experimental --dry-run",
                    {"zpm", "upgrade", "--experimental", "--dry-run"},
                    "Simulates the experimental ZPM upgrade flow without changing files.",
                    false,
                },
                {
                    "Force experimental dry-run",
                    "zpm upgrade --experimental --force --dry-run",
                    {"zpm", "upgrade", "--experimental", "--force", "--dry-run"},
                    "Simulates the forced experimental ZPM upgrade flow without changing files.",
                    false,
                },
            },
        },
        {
            "Updates",
            "Real system update tasks for packages handled by the detected backend.",
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
            },
        },
        {
            "Power actions",
            "Update flows that may reboot or shut down the machine.",
            {
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
            },
        },
        {
            "Package changes",
            "Real install and remove operations.",
            std::move(packageChanges),
        },
        {
            "Search and info",
            "Read-only package search and package details.",
            {
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
            },
        },
        {
            "Lists",
            "Installed package lists, with direct output or the configured pager.",
            {
                {
                    "Package list (no pager)",
                    "zpm list --no-pager",
                    {"zpm", "list", "--no-pager"},
                    "Prints all installed packages directly in the terminal.",
                    false,
                },
                {
                    "Native package list (no pager)",
                    "zpm list --native --no-pager",
                    {"zpm", "list", "--native", "--no-pager"},
                    "Prints installed native packages directly in the terminal.",
                    false,
                },
                {
                    "Flatpak package list (no pager)",
                    "zpm list --flatpak --no-pager",
                    {"zpm", "list", "--flatpak", "--no-pager"},
                    "Prints installed Flatpak packages directly in the terminal.",
                    false,
                },
                {
                    "Snap package list (no pager)",
                    "zpm list --snap --no-pager",
                    {"zpm", "list", "--snap", "--no-pager"},
                    "Prints installed Snap packages directly in the terminal.",
                    false,
                },
                {
                    "Package list (pager)",
                    "zpm list",
                    {"zpm", "list"},
                    "Shows all installed packages with the configured pager when needed.",
                    false,
                },
                {
                    "Native package list (pager)",
                    "zpm list --native",
                    {"zpm", "list", "--native"},
                    "Shows installed native packages with the configured pager when needed.",
                    false,
                },
                {
                    "Flatpak package list (pager)",
                    "zpm list --flatpak",
                    {"zpm", "list", "--flatpak"},
                    "Shows installed Flatpak packages with the configured pager when needed.",
                    false,
                },
                {
                    "Snap package list (pager)",
                    "zpm list --snap",
                    {"zpm", "list", "--snap"},
                    "Shows installed Snap packages with the configured pager when needed.",
                    false,
                },
            },
        },
        {
            "ZPM upgrade",
            "Real upgrade operations for Zielina Package Manager itself.",
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
            },
        },
        {
            "Help and config",
            "Help pages, version information, configuration editing, and exit.",
            {
                {
                    "Homepage",
                    "zhome",
                    {"zhome"},
                    "Opens the ZPM homepage/help interface through zhome.",
                    false,
                },
                {
                    "Homepage page 1",
                    "zhome -p1",
                    {"zhome", "-p1"},
                    "Opens PAGE 1 of the ZPM homepage.",
                    false,
                },
                {
                    "Homepage page 2",
                    "zhome -p2",
                    {"zhome", "-p2"},
                    "Opens PAGE 2 of the ZPM homepage.",
                    false,
                },
                {
                    "Homepage page 3",
                    "zhome -p3",
                    {"zhome", "-p3"},
                    "Opens PAGE 3 of the ZPM homepage.",
                    false,
                },
                {
                    "Homepage all pages",
                    "zhome -p1 -p2 -p3",
                    {"zhome", "-p1", "-p2", "-p3"},
                    "Opens all ZPM homepage pages in order.",
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
                    "Edit config",
                    "$SUDO_EDITOR/$VISUAL/$EDITOR /opt/ZPM/zielina.conf",
                    editConfigArgs(),
                    "Opens the active ZPM configuration file in the default "
                    "terminal editor.",
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

    return categories;
}

Element renderMenu(Component categoryMenu,
                   Component actionMenu,
                   const SystemStatus& status,
                   const std::vector<Category>& categories,
                   int selectedCategory,
                   int selectedAction,
                   int focusedPane) {
    if (categories.empty()) {
        return appTheme(vbox({
                   titleText("ZPM TUI"),
                   separator(),
                   paragraphAlignLeft("No categories are available. Rebuild the TUI data and try again.")
                       | color(Color::RedLight),
               }) |
               borderStyled(ROUNDED, Color::RedLight));
    }

    const int categoryIndex = clampIndex(selectedCategory, categories.size());
    const Category& category = categories[static_cast<std::size_t>(categoryIndex)];
    if (category.actions.empty()) {
        return appTheme(vbox({
                   titleText("ZPM TUI"),
                   separator(),
                   paragraphAlignLeft("The selected category has no actions.")
                       | color(Color::RedLight),
               }) |
               borderStyled(ROUNDED, Color::RedLight));
    }

    const int actionIndex = clampIndex(selectedAction, category.actions.size());
    const Action& action = category.actions[static_cast<std::size_t>(actionIndex)];
    const Dimensions terminalSize = Terminal::Size();
    const bool compact = terminalSize.dimx > 0 && terminalSize.dimx < 112;

    std::vector<Element> details {
        hbox({
            titleText(category.title),
            filler(),
            actionBadge(action),
        }),
        paragraphAlignLeft(category.hint) | dim,
        separator(),
    };

    if (category.title == "Welcome") {
        details.push_back(sectionTitle("Navigation"));
        details.push_back(paragraphAlignLeft(
            "Use Left/Right or Tab to switch between Categories and Actions. "
            "Use Up/Down to move inside the focused panel. Enter selects the "
            "highlighted category or action."));
        details.push_back(separator());
        details.push_back(sectionTitle("Command flow"));
        details.push_back(paragraphAlignLeft(
            "Actions with a command preview open a confirmation screen first. "
            "Press Enter or r to run, Esc or c to cancel, and q to quit the TUI."));
        details.push_back(separator());
        details.push_back(sectionTitle("After a command"));
        details.push_back(paragraphAlignLeft(
            "The interface closes before launching the real command, so output, "
            "prompts, passwords, and errors are shown directly by the terminal. "
            "When the command finishes, press b to return to ZPM TUI or q to exit."));
        details.push_back(separator());
        details.push_back(sectionTitle("Safe workflow"));
        details.push_back(paragraphAlignLeft(
            "For risky operations, start with a dry-run action when available. "
            "Check the command preview before confirming updates, removals, "
            "purges, forced upgrades, reboot, or shutdown actions."));
        details.push_back(separator());
    }

    details.push_back(hbox({
        text(action.title) | bold,
        filler(),
    }));
    details.push_back(commandPreviewBox(action.commandPreview));
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
    details.push_back(sectionTitle("System status"));
    details.push_back(hflow({
        statusChip("Distro", status.distro, Color::White),
        statusChip("Backend", status.backend, status.backend == "unknown"
                                            ? Color::RedLight
                                            : Color::GreenLight),
        statusChip("ZPM", status.zpmVersion, Color::CyanLight),
        statusChip("User", status.privileges, status.privileges == "root"
                                                 ? Color::GreenLight
                                                 : Color::RedLight),
    }));

    Element categoriesPanel = panel("Categories",
                                    categoryMenu->Render() | vscroll_indicator | frame,
                                    focusedPane == 0);
    Element actionsPanel = panel("Actions",
                                 actionMenu->Render() | vscroll_indicator | frame,
                                 focusedPane == 1);
    Element detailsPanel = panel("Details", vbox(details) | yframe);

    Element navigation = compact
                             ? vbox({
                                   categoriesPanel | size(HEIGHT, LESS_THAN, 10),
                                   actionsPanel | size(HEIGHT, LESS_THAN, 12),
                               })
                             : hbox({
                                   categoriesPanel | size(WIDTH, EQUAL, 24),
                                   actionsPanel | size(WIDTH, EQUAL, 32),
                               });

    Element body = compact
                       ? vbox({
                             std::move(navigation),
                             detailsPanel | flex,
                         })
                       : hbox({
                             std::move(navigation),
                             detailsPanel | flex,
                         });

    return appTheme(vbox({
               hbox({
                   text(" ZPM TUI ") | bold | color(Color::Black) | bgcolor(Color::CyanLight),
                   text(" Zielina Package Manager ") | dim,
                   filler(),
                   text(compact ? "Tab: panel  Enter: select  q: exit"
                                : "Left/Right or Tab: panel  Enter: select  q/Esc: exit")
                       | dim,
               }),
               hbox({
                   keyHint("Up/Down", "move"),
                   text("  "),
                   keyHint("Enter", "select"),
                   text("  "),
                   keyHint("Esc", "exit/cancel"),
               }) | size(HEIGHT, EQUAL, 1),
               separatorStyled(LIGHT),
               std::move(body) | flex,
           }) |
           borderStyled(ROUNDED, Color::CyanLight));
}

Element renderConfirmation(const Action& action) {
    std::vector<Element> content {
        hbox({
            titleText("Confirm command"),
            filler(),
            actionBadge(action),
            text("  "),
            text("Enter/r: run  Esc/c: cancel  q: exit") | dim,
        }),
        separatorStyled(LIGHT),
        text(action.title) | bold,
    };

    content.push_back(commandPreviewBox(action.commandPreview));
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
        text(" Run ") | bold | color(Color::Black) | bgcolor(Color::GreenLight),
        text("  "),
        text(" Cancel ") | bold | color(Color::Black) | bgcolor(Color::Yellow),
    }));

    return appTheme(vbox(content) | borderStyled(ROUNDED, Color::CyanLight));
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

    while (true) {
        Action selectedLaunch;
        Action pendingAction;
        bool shouldLaunch = false;
        int selectedCategory = 0;
        int selectedAction = 0;
        int focusedPane = 0;

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
                if (categories.empty()) {
                    selectedCategory = 0;
                    selectedAction = 0;
                    focusedPane = 0;
                    actionEntries.clear();
                    return;
                }
                selectedCategory = clampIndex(selectedCategory, categories.size());
                focusedPane = clampIndex(focusedPane, 2);
                actionEntries.clear();
                const Category& category = categories[static_cast<std::size_t>(selectedCategory)];
                actionEntries.reserve(category.actions.size());
                for (const Action& action : category.actions) {
                    actionEntries.push_back(action.title);
                }

                selectedAction = clampIndex(selectedAction, actionEntries.size());
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
                syncActionEntries();
                if (categories.empty()) {
                    return;
                }
                const Category& category = categories[static_cast<std::size_t>(selectedCategory)];
                if (category.actions.empty()) {
                    return;
                }
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
                if (event == Event::Tab) {
                    focusedPane = focusedPane == 0 ? 1 : 0;
                    return true;
                }
                if (event == Event::ArrowRight) {
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
