#include <ftxui/component/animation.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cmath>
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
    return clear_under(std::move(element) | color(Color::White) | bgcolor(Color::Black)) |
           bgcolor(Color::Black) |
           flex;
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

Element metric(std::string label, std::string value, Color valueColor) {
    return hbox({
        text(" " + std::move(label) + " ") | dim,
        text(std::move(value)) | bold | color(valueColor),
        text(" "),
    });
}

struct UiAnimationState {
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
};

float animationPhase(const UiAnimationState& animation) {
    using namespace std::chrono;
    const auto elapsed = steady_clock::now() - animation.started;
    const double seconds = duration_cast<milliseconds>(elapsed).count() / 1000.0;
    return static_cast<float>(std::fmod(seconds / 1.8, 1.0));
}

Color pulseColor(float phase) {
    if (phase < 0.25F) {
        return Color::CyanLight;
    }
    if (phase < 0.50F) {
        return Color::GreenLight;
    }
    if (phase < 0.75F) {
        return Color::YellowLight;
    }
    return Color::BlueLight;
}

std::string shimmerLine(int width, float phase) {
    width = std::clamp(width, 12, 96);
    std::string line(static_cast<std::size_t>(width), '-');
    const int highlight = std::max(4, width / 8);
    const int start = static_cast<int>(phase * static_cast<float>(width + highlight)) - highlight;

    for (int offset = 0; offset < highlight; ++offset) {
        const int index = start + offset;
        if (index >= 0 && index < width) {
            line[static_cast<std::size_t>(index)] = '=';
        }
    }

    return line;
}

Element animatedAccent(const UiAnimationState& animation, int terminalWidth) {
    const float phase = animationPhase(animation);
    return text(shimmerLine(terminalWidth - 8, phase)) | color(pulseColor(phase));
}

Element menuEntry(const EntryState& state) {
    Element label = text((state.active ? ">> " : "   ") + state.label);
    if (state.active) {
        label = label | bold | color(Color::Black) | bgcolor(state.focused ? Color::CyanLight
                                                                           : Color::GrayLight);
    } else if (state.focused) {
        label = label | color(Color::CyanLight);
    } else {
        label = label | color(Color::White);
    }
    return label;
}

MenuOption zpmMenuOption() {
    MenuOption option = MenuOption::VerticalAnimated();
    option.entries_option.transform = menuEntry;
    option.underline.enabled = true;
    option.underline.SetAnimation(std::chrono::milliseconds(180), animation::easing::CubicOut);
    return option;
}

std::size_t countActions(const std::vector<Category>& categories) {
    std::size_t total = 0;
    for (const Category& category : categories) {
        total += category.actions.size();
    }
    return total;
}

std::size_t countCommandActions(const std::vector<Category>& categories) {
    std::size_t total = 0;
    for (const Category& category : categories) {
        for (const Action& action : category.actions) {
            if (!action.args.empty()) {
                ++total;
            }
        }
    }
    return total;
}

std::size_t countRootActions(const std::vector<Category>& categories) {
    std::size_t total = 0;
    for (const Category& category : categories) {
        for (const Action& action : category.actions) {
            if (action.needsRoot) {
                ++total;
            }
        }
    }
    return total;
}

Element dashboardLine(std::string key, std::string value, Color keyColor) {
    return hbox({
        text(" " + std::move(key) + " ") | bold | color(Color::Black) | bgcolor(keyColor),
        text(" " + std::move(value)),
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

Element welcomeDashboard(const SystemStatus& status,
                         const std::vector<Category>& categories,
                         bool compact) {
    const std::size_t allActions = countActions(categories);
    const std::size_t commandActions = countCommandActions(categories);
    const std::size_t rootActions = countRootActions(categories);

    Element stats = compact
        ? vbox({
              metric("Categories", std::to_string(categories.size()), Color::CyanLight),
              metric("Actions", std::to_string(allActions), Color::GreenLight),
              metric("Commands", std::to_string(commandActions), Color::YellowLight),
              metric("Root tasks", std::to_string(rootActions), Color::RedLight),
          })
        : hbox({
              metric("Categories", std::to_string(categories.size()), Color::CyanLight),
              separatorEmpty(),
              metric("Actions", std::to_string(allActions), Color::GreenLight),
              separatorEmpty(),
              metric("Commands", std::to_string(commandActions), Color::YellowLight),
              separatorEmpty(),
              metric("Root tasks", std::to_string(rootActions), Color::RedLight),
          });

    return vbox({
        hbox({
            text(" ZPM COMMAND CENTER ") | bold | color(Color::Black) | bgcolor(Color::CyanLight),
            text("  Terminal UI for Zielina Package Manager") | dim,
        }),
        separator(),
        stats,
        separator(),
        hflow({
            statusChip("Backend", status.backend, status.backend == "unknown"
                                           ? Color::RedLight
                                           : Color::GreenLight),
            statusChip("ZPM", status.zpmVersion, Color::CyanLight),
            statusChip("User", status.privileges, status.privileges == "root"
                                                ? Color::GreenLight
                                                : Color::RedLight),
        }),
        separator(),
        sectionTitle("Recommended flow"),
        dashboardLine("1", "Dry runs: preview install, remove, update, clean, and upgrade tasks.",
                      Color::CyanLight),
        dashboardLine("2", "Browse packages: search, inspect, and list before changing the system.",
                      Color::GreenLight),
        dashboardLine("3", "Logs: open recent ZPM logs when a command needs diagnosis.",
                      Color::YellowLight),
        separator(),
        sectionTitle("Launch behavior"),
        paragraphAlignLeft(
            "Every command leaves the fullscreen interface first, clears the "
            "terminal, then runs the real ZPM program in a clean screen. "
            "Command output, prompts, passwords, and errors stay visible in "
            "the terminal until you choose whether to return."),
    });
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

void clearTerminalForCommand() {
    if (isatty(STDOUT_FILENO) != 1) {
        return;
    }

    std::cout << "\033[0m\033[?25h\033[H\033[2J\033[3J";
    std::cout.flush();
}

void prepareTerminalForTui() {
    if (isatty(STDOUT_FILENO) != 1) {
        return;
    }

    std::cout << "\033[0m\033[37m\033[40m\033[H\033[2J";
    std::cout.flush();
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

    clearTerminalForCommand();
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

std::vector<std::string> viewLogArgs(const std::string& path) {
    const std::string quotedPath = shellQuote(path);
    return shellArgs(
        "path=" + quotedPath + "; "
        "if [ ! -e \"$path\" ]; then "
        "echo \"Log file does not exist yet: $path\"; exit 0; "
        "fi; "
        "if [ ! -s \"$path\" ]; then "
        "echo \"Log file is empty: $path\"; exit 0; "
        "fi; "
        "pager=${PAGER:-}; "
        "if [ -z \"$pager\" ]; then "
        "for candidate in less more pager; do "
        "if command -v \"$candidate\" >/dev/null 2>&1; then pager=$candidate; break; fi; "
        "done; "
        "fi; "
        "if [ -n \"$pager\" ]; then exec $pager \"$path\"; fi; "
        "exec tail -n 200 \"$path\""
    );
}

std::vector<std::string> logSummaryArgs() {
    return shellArgs(
        "for path in "
        "/tmp/zinst.log /tmp/zrm.log /tmp/zupd.log /tmp/zupd_patchcheck.log "
        "/tmp/zclean.log /tmp/zupgr.log /var/log/zuninstall.log; do "
        "if [ -e \"$path\" ]; then "
        "printf '%-30s %10s  %s\\n' \"$path\" \"$(wc -c < \"$path\") bytes\" \"$(stat -c '%y' \"$path\" 2>/dev/null | cut -d. -f1)\"; "
        "else "
        "printf '%-30s missing\\n' \"$path\"; "
        "fi; "
        "done"
    );
}

std::vector<Category> buildCategories(const SystemStatus& status) {
    const std::string purgeWarning =
        status.backend == "apt"
            ? "APT purge can remove package configuration files."
            : "APT purge is available only on apt. Current backend: " +
                  status.backend + ". ZPM may ignore purge on this backend.";
    const std::string uninstallWarning =
        "This can remove ZPM files and command symlinks from the system.";

    return {
        {
            "Welcome",
            "Keyboard, mouse, and launch rules for the refreshed ZPM interface.",
            {
                {
                    "Navigation guide",
                    "",
                    {},
                    "Use the category list first, then choose the exact task. "
                    "Mouse click and wheel scrolling work in the menu panels; "
                    "keyboard navigation is still available everywhere.",
                    false,
                },
            },
        },
        {
            "Dry runs",
            "All ZPM simulations grouped together; these actions should not change packages or files.",
            {
                {
                    "Install packages dry-run",
                    "zpm install --dry-run <packages...>",
                    promptCommandArgs("Packages to simulate installing", "zpm install --dry-run"),
                    "Simulates install flow without changing packages.",
                    false,
                },
                {
                    "Remove packages dry-run",
                    "zpm remove --dry-run <packages...>",
                    promptCommandArgs("Packages to simulate removing", "zpm remove --dry-run"),
                    "Simulates remove flow without changing packages.",
                    false,
                },
                {
                    "APT purge dry-run",
                    "zpm remove --purge --dry-run <packages...>",
                    promptCommandArgs("Packages to simulate purging", "zpm remove --purge --dry-run"),
                    "Simulates APT purge through ZPM.",
                    false,
                    false,
                    purgeWarning,
                },
                {
                    "System update dry-run",
                    "zpm update --dry-run",
                    {"zpm", "update", "--dry-run"},
                    "Simulates the standard system update flow.",
                    false,
                },
                {
                    "Full system update dry-run",
                    "zpm update --full --dry-run",
                    {"zpm", "update", "--full", "--dry-run"},
                    "Simulates the full system update flow.",
                    false,
                },
                {
                    "Clean cache dry-run",
                    "zpm clean --dry-run",
                    {"zpm", "clean", "--dry-run"},
                    "Simulates cache cleanup without deleting files.",
                    false,
                },
                {
                    "ZPM upgrade dry-run",
                    "zpm upgrade --dry-run",
                    {"zpm", "upgrade", "--dry-run"},
                    "Simulates a stable ZPM upgrade without changing files.",
                    false,
                },
                {
                    "Force upgrade dry-run",
                    "zpm upgrade --force --dry-run",
                    {"zpm", "upgrade", "--force", "--dry-run"},
                    "Simulates a forced stable ZPM upgrade.",
                    false,
                },
                {
                    "Experimental upgrade dry-run",
                    "zpm upgrade --experimental --dry-run",
                    {"zpm", "upgrade", "--experimental", "--dry-run"},
                    "Simulates a prerelease ZPM upgrade.",
                    false,
                },
                {
                    "Force experimental dry-run",
                    "zpm upgrade --experimental --force --dry-run",
                    {"zpm", "upgrade", "--experimental", "--force", "--dry-run"},
                    "Simulates a forced prerelease ZPM upgrade.",
                    false,
                },
            },
        },
        {
            "Browse packages",
            "Read-only package search, package details, and installed package lists.",
            {
                {
                    "Search all sources",
                    "zpm search <query>",
                    promptCommandArgs("Search query", "zpm search"),
                    "Searches native packages, Flatpak, and Snap when available.",
                    false,
                },
                {
                    "Search native packages",
                    "zpm search --native <query>",
                    promptCommandArgs("Native search query", "zpm search --native"),
                    "Searches only packages from the detected native package manager.",
                    false,
                },
                {
                    "Search Flatpak packages",
                    "zpm search --flatpak <query>",
                    promptCommandArgs("Flatpak search query", "zpm search --flatpak"),
                    "Searches only Flatpak applications.",
                    false,
                },
                {
                    "Search Snap packages",
                    "zpm search --snap <query>",
                    promptCommandArgs("Snap search query", "zpm search --snap"),
                    "Searches only Snap packages.",
                    false,
                },
                {
                    "Package info",
                    "zpm info <package>",
                    promptCommandArgs("Package to inspect", "zpm info"),
                    "Shows package metadata from native PM, Flatpak, or Snap.",
                    false,
                },
                {
                    "List all packages",
                    "zpm list --no-pager",
                    {"zpm", "list", "--no-pager"},
                    "Prints installed packages directly in the terminal.",
                    false,
                },
                {
                    "List native packages",
                    "zpm list --native --no-pager",
                    {"zpm", "list", "--native", "--no-pager"},
                    "Prints installed native packages directly in the terminal.",
                    false,
                },
                {
                    "List Flatpak packages",
                    "zpm list --flatpak --no-pager",
                    {"zpm", "list", "--flatpak", "--no-pager"},
                    "Prints installed Flatpak packages directly in the terminal.",
                    false,
                },
                {
                    "List Snap packages",
                    "zpm list --snap --no-pager",
                    {"zpm", "list", "--snap", "--no-pager"},
                    "Prints installed Snap packages directly in the terminal.",
                    false,
                },
                {
                    "List all with pager",
                    "zpm list --pager",
                    {"zpm", "list", "--pager"},
                    "Shows installed packages with the configured pager.",
                    false,
                },
                {
                    "List native with pager",
                    "zpm list --native --pager",
                    {"zpm", "list", "--native", "--pager"},
                    "Shows installed native packages with the configured pager.",
                    false,
                },
                {
                    "List Flatpak with pager",
                    "zpm list --flatpak --pager",
                    {"zpm", "list", "--flatpak", "--pager"},
                    "Shows installed Flatpak packages with the configured pager.",
                    false,
                },
                {
                    "List Snap with pager",
                    "zpm list --snap --pager",
                    {"zpm", "list", "--snap", "--pager"},
                    "Shows installed Snap packages with the configured pager.",
                    false,
                },
            },
        },
        {
            "Install and remove",
            "Real package install and remove operations.",
            {
                {
                    "Install packages",
                    "zpm install <packages...>",
                    promptCommandArgs("Packages to install", "zpm install"),
                    "Installs packages using native PM, Flatpak, or Snap detection.",
                    true,
                },
                {
                    "Remove packages",
                    "zpm remove <packages...>",
                    promptCommandArgs("Packages to remove", "zpm remove"),
                    "Removes packages using native PM, Flatpak, or Snap detection.",
                    true,
                },
                {
                    "APT purge packages",
                    "zpm remove --purge <packages...>",
                    promptCommandArgs("Packages to purge", "zpm remove --purge"),
                    "Removes packages with APT purge when the backend supports it.",
                    true,
                    false,
                    purgeWarning,
                },
            },
        },
        {
            "System update",
            "Normal and full update flows that can change the system.",
            {
                {
                    "Update with prompts",
                    "zpm update",
                    {"zpm", "update"},
                    "Runs the standard update flow and leaves native prompts visible.",
                    true,
                },
                {
                    "Full update with prompts",
                    "zpm update --full",
                    {"zpm", "update", "--full"},
                    "Runs the full update flow and leaves native prompts visible.",
                    true,
                },
                {
                    "Automatic update",
                    "zpm update --yes",
                    {"zpm", "update", "--yes"},
                    "Runs the standard update flow with automatic confirmations.",
                    true,
                },
                {
                    "Automatic full update",
                    "zpm update --full --yes",
                    {"zpm", "update", "--full", "--yes"},
                    "Runs the full update flow with automatic confirmations.",
                    true,
                },
            },
        },
        {
            "Power update",
            "Update flows that can reboot or shut down the machine afterwards.",
            {
                {
                    "Update then reboot",
                    "zpm update --reboot",
                    {"zpm", "update", "--reboot"},
                    "Runs a standard update and requests a reboot afterwards.",
                    true,
                    false,
                    "This can reboot the machine after updates finish.",
                },
                {
                    "Update then shutdown",
                    "zpm update --shutdown",
                    {"zpm", "update", "--shutdown"},
                    "Runs a standard update and requests a shutdown afterwards.",
                    true,
                    false,
                    "This can shut down the machine after updates finish.",
                },
                {
                    "Full update then reboot",
                    "zpm update --full --reboot",
                    {"zpm", "update", "--full", "--reboot"},
                    "Runs a full update and requests a reboot afterwards.",
                    true,
                    false,
                    "This can perform a full update and reboot the machine.",
                },
                {
                    "Full update then shutdown",
                    "zpm update --full --shutdown",
                    {"zpm", "update", "--full", "--shutdown"},
                    "Runs a full update and requests a shutdown afterwards.",
                    true,
                    false,
                    "This can perform a full update and shut down the machine.",
                },
                {
                    "Auto update then reboot",
                    "zpm update --yes --reboot",
                    {"zpm", "update", "--yes", "--reboot"},
                    "Runs automatic update and requests a reboot afterwards.",
                    true,
                    false,
                    "This answers prompts automatically and can reboot the machine.",
                },
                {
                    "Auto update then shutdown",
                    "zpm update --yes --shutdown",
                    {"zpm", "update", "--yes", "--shutdown"},
                    "Runs automatic update and requests a shutdown afterwards.",
                    true,
                    false,
                    "This answers prompts automatically and can shut down the machine.",
                },
                {
                    "Auto full update then reboot",
                    "zpm update --full --yes --reboot",
                    {"zpm", "update", "--full", "--yes", "--reboot"},
                    "Runs automatic full update and requests a reboot afterwards.",
                    true,
                    false,
                    "This answers prompts automatically, performs a full update, and can reboot.",
                },
                {
                    "Auto full update then shutdown",
                    "zpm update --full --yes --shutdown",
                    {"zpm", "update", "--full", "--yes", "--shutdown"},
                    "Runs automatic full update and requests a shutdown afterwards.",
                    true,
                    false,
                    "This answers prompts automatically, performs a full update, and can shut down.",
                },
            },
        },
        {
            "Run programs",
            "Find and launch installed desktop or terminal applications through ZPM.",
            {
                {
                    "Run application",
                    "zpm run <application>",
                    promptCommandArgs("Application to run", "zpm run"),
                    "Finds the app in native PM, Flatpak, or Snap and launches it.",
                    false,
                },
            },
        },
        {
            "Maintenance",
            "Cleanup and local housekeeping.",
            {
                {
                    "Clean cache",
                    "zpm clean",
                    {"zpm", "clean"},
                    "Cleans package caches and leftovers.",
                    true,
                    false,
                    "Clean can delete cached package data and leftover files.",
                },
            },
        },
        {
            "Logs",
            "Open ZPM program logs from /tmp and /var/log.",
            {
                {
                    "Log summary",
                    "show known ZPM logs",
                    logSummaryArgs(),
                    "Shows which known log files exist, their size, and last modification time.",
                    false,
                },
                {
                    "Install log",
                    "view /tmp/zinst.log",
                    viewLogArgs("/tmp/zinst.log"),
                    "Opens the zpm install log.",
                    false,
                },
                {
                    "Remove log",
                    "view /tmp/zrm.log",
                    viewLogArgs("/tmp/zrm.log"),
                    "Opens the zpm remove log.",
                    false,
                },
                {
                    "Update log",
                    "view /tmp/zupd.log",
                    viewLogArgs("/tmp/zupd.log"),
                    "Opens the zpm update log.",
                    false,
                },
                {
                    "Update patch-check log",
                    "view /tmp/zupd_patchcheck.log",
                    viewLogArgs("/tmp/zupd_patchcheck.log"),
                    "Opens the update patch-check log.",
                    false,
                },
                {
                    "Clean log",
                    "view /tmp/zclean.log",
                    viewLogArgs("/tmp/zclean.log"),
                    "Opens the zpm clean log.",
                    false,
                },
                {
                    "ZPM upgrade log",
                    "view /tmp/zupgr.log",
                    viewLogArgs("/tmp/zupgr.log"),
                    "Opens the zpm upgrade log.",
                    false,
                },
                {
                    "Uninstall log",
                    "view /var/log/zuninstall.log",
                    viewLogArgs("/var/log/zuninstall.log"),
                    "Opens the zpm uninstall log.",
                    false,
                },
            },
        },
        {
            "ZPM Admin",
            "Configuration, upgrade, restart, and uninstall actions for Zielina Package Manager.",
            {
                {
                    "Edit config",
                    "$SUDO_EDITOR/$VISUAL/$EDITOR /opt/ZPM/zielina.conf",
                    editConfigArgs(),
                    "Opens the active ZPM configuration file in a terminal editor.",
                    true,
                    false,
                    "Editing config can change ZPM behavior.",
                },
                {
                    "Edit config via zhome",
                    "zpm home --edit-config",
                    {"zpm", "home", "--edit-config"},
                    "Runs the zhome configuration editor path.",
                    true,
                    false,
                    "Editing config can change ZPM behavior.",
                },
                {
                    "Upgrade ZPM",
                    "zpm upgrade",
                    {"zpm", "upgrade"},
                    "Checks and installs stable ZPM updates when available.",
                    true,
                },
                {
                    "Force ZPM upgrade",
                    "zpm upgrade --force",
                    {"zpm", "upgrade", "--force"},
                    "Forces reinstall even when ZPM appears up to date.",
                    true,
                    false,
                    "Force upgrade can reinstall or replace the current ZPM build.",
                },
                {
                    "Experimental ZPM upgrade",
                    "zpm upgrade --experimental",
                    {"zpm", "upgrade", "--experimental"},
                    "Checks prerelease ZPM updates.",
                    true,
                },
                {
                    "Force experimental upgrade",
                    "zpm upgrade --experimental --force",
                    {"zpm", "upgrade", "--experimental", "--force"},
                    "Forces the prerelease upgrade path.",
                    true,
                    false,
                    "This forces a prerelease upgrade path. Review before running.",
                },
                {
                    "Restart ZPM TUI",
                    "zpm tui",
                    {"zpm", "tui"},
                    "Closes this session and starts a fresh ZPM terminal UI.",
                    true,
                },
                {
                    "Uninstall ZPM",
                    "zpm uninstall",
                    {"zpm", "uninstall"},
                    "Runs the real ZPM uninstaller.",
                    true,
                    false,
                    uninstallWarning,
                },
            },
        },
        {
            "Home pages",
            "ZPM home pages and command index.",
            {
                {
                    "Homepage",
                    "zpm home",
                    {"zpm", "home"},
                    "Opens the ZPM homepage/help interface.",
                    false,
                },
                {
                    "Homepage page 1",
                    "zpm home -p1",
                    {"zpm", "home", "-p1"},
                    "Opens PAGE 1: ARM support and config information.",
                    false,
                },
                {
                    "Homepage page 2",
                    "zpm home -p2",
                    {"zpm", "home", "-p2"},
                    "Opens PAGE 2: zpm wrapper commands.",
                    false,
                },
                {
                    "Homepage page 3",
                    "zpm home -p3",
                    {"zpm", "home", "-p3"},
                    "Opens PAGE 3: ZPM command aliases.",
                    false,
                },
                {
                    "Homepage all pages",
                    "zpm home -p1 -p2 -p3",
                    {"zpm", "home", "-p1", "-p2", "-p3"},
                    "Opens all ZPM homepage pages in order.",
                    false,
                },
            },
        },
        {
            "Help",
            "Help screens for every ZPM command that exposes one.",
            {
                {"Wrapper help", "zpm --help", {"zpm", "--help"}, "Shows help for the zpm wrapper.", false},
                {"Install help", "zpm install --help", {"zpm", "install", "--help"}, "Shows install command help.", false},
                {"Remove help", "zpm remove --help", {"zpm", "remove", "--help"}, "Shows remove command help.", false},
                {"Update help", "zpm update --help", {"zpm", "update", "--help"}, "Shows update command help.", false},
                {"Upgrade help", "zpm upgrade --help", {"zpm", "upgrade", "--help"}, "Shows upgrade command help.", false},
                {"List help", "zpm list --help", {"zpm", "list", "--help"}, "Shows list command help.", false},
                {"Search help", "zpm search --help", {"zpm", "search", "--help"}, "Shows search command help.", false},
                {"Info help", "zpm info --help", {"zpm", "info", "--help"}, "Shows info command help.", false},
                {"Clean help", "zpm clean --help", {"zpm", "clean", "--help"}, "Shows clean command help.", false},
                {"Run help", "zpm run --help", {"zpm", "run", "--help"}, "Shows run command help.", false},
                {"Uninstall help", "zpm uninstall --help", {"zpm", "uninstall", "--help"}, "Shows uninstall command help.", false},
            },
        },
        {
            "Versions",
            "Version screens for wrapper commands.",
            {
                {"Wrapper version", "zpm --version", {"zpm", "--version"}, "Shows zpm wrapper version.", false},
                {"Install version", "zpm install --version", {"zpm", "install", "--version"}, "Shows install command version.", false},
                {"Remove version", "zpm remove --version", {"zpm", "remove", "--version"}, "Shows remove command version.", false},
                {"Update version", "zpm update --version", {"zpm", "update", "--version"}, "Shows update command version.", false},
                {"Upgrade version", "zpm upgrade --version", {"zpm", "upgrade", "--version"}, "Shows upgrade command version.", false},
                {"List version", "zpm list --version", {"zpm", "list", "--version"}, "Shows list command version.", false},
                {"Search version", "zpm search --version", {"zpm", "search", "--version"}, "Shows search command version.", false},
                {"Info version", "zpm info --version", {"zpm", "info", "--version"}, "Shows info command version.", false},
                {"Clean version", "zpm clean --version", {"zpm", "clean", "--version"}, "Shows clean command version.", false},
                {"Run version", "zpm run --version", {"zpm", "run", "--version"}, "Shows run command version.", false},
                {"Uninstall version", "zpm uninstall --version", {"zpm", "uninstall", "--version"}, "Shows uninstall command version.", false},
            },
        },
        {
            "Exit",
            "Leave the terminal UI.",
            {
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
}

Element renderMenu(Component categoryMenu,
                   Component actionMenu,
                   const SystemStatus& status,
                   const std::vector<Category>& categories,
                   int selectedCategory,
                   int selectedAction,
                   int focusedPane,
                   Box& actionPanelBox,
                   const UiAnimationState& animation) {
    animation::RequestAnimationFrame();

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
        details.push_back(welcomeDashboard(status, categories, compact));
        details.push_back(separator());
        details.push_back(sectionTitle("Navigation"));
        details.push_back(paragraphAlignLeft(
            "Use mouse click, wheel scrolling, Up/Down, or j/k to move. "
            "Use Left/Right, h/l, or Tab to switch between Categories and "
            "Actions. Enter selects the highlighted category or action."));
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

    if (!action.warning.empty()) {
        details.push_back(separator());
        details.push_back(paragraphAlignLeft(action.warning) | color(Color::RedLight) | bold);
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

    Element categoriesPanel = panel("Categories (" + std::to_string(categories.size()) + ")",
                                    categoryMenu->Render() | vscroll_indicator | frame,
                                    focusedPane == 0);
    Element actionsPanel = panel("Actions (" + std::to_string(category.actions.size()) + ")",
                                 actionMenu->Render() | vscroll_indicator | frame,
                                 focusedPane == 1);
    Element detailsPanel = panel("Details", vbox(details) | yframe);

    Element navigation = compact
                             ? vbox({
                                   categoriesPanel | size(HEIGHT, LESS_THAN, 10),
                                   actionsPanel | size(HEIGHT, LESS_THAN, 12) | reflect(actionPanelBox),
                               })
                             : hbox({
                                   categoriesPanel | size(WIDTH, EQUAL, 24),
                                   actionsPanel | size(WIDTH, EQUAL, 32) | reflect(actionPanelBox),
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
                   text(" ZPM ") | bold | color(Color::Black) | bgcolor(Color::CyanLight),
                   text(" TUI ") | bold | color(Color::Black) | bgcolor(Color::GreenLight),
                   text(" " + category.title + " / " + action.title + " ") | bold,
                   filler(),
                   text(compact ? "q: back/confirm exit  Enter: select"
                                : "Mouse + wheel  Left/Right/Tab: panel  q/Esc: confirm exit")
                       | dim,
               }),
               hbox({
                   keyHint("Up/Down", "move"),
                   text("  "),
                   keyHint("Wheel", "scroll"),
                   text("  "),
                   keyHint("Enter", "select"),
                   text("  "),
                   keyHint("q", "back/confirm exit"),
                   text("  "),
                   keyHint("Esc", "exit/cancel"),
               }) | size(HEIGHT, EQUAL, 1),
               animatedAccent(animation, terminalSize.dimx),
               separatorStyled(LIGHT),
               std::move(body) | flex,
           }) |
           borderStyled(ROUNDED, Color::CyanLight));
}

Element confirmButton(std::string label,
                      std::string hint,
                      Color borderColor,
                      Color fillColor,
                      Box& box) {
    return vbox({
               filler(),
               hbox({
                   filler(),
                   text("  " + std::move(label) + "  ") |
                       bold |
                       color(Color::Black) |
                       bgcolor(fillColor),
                   filler(),
               }),
               hbox({
                   filler(),
                   text(std::move(hint)) | dim,
                   filler(),
               }),
               filler(),
           }) |
           size(HEIGHT, EQUAL, 5) |
           size(WIDTH, GREATER_THAN, 26) |
           borderStyled(ROUNDED, borderColor) |
           reflect(box);
}

Element renderConfirmation(const Action& action,
                           const UiAnimationState& animation,
                           Box& runButtonBox,
                           Box& cancelButtonBox) {
    animation::RequestAnimationFrame();
    const float phase = animationPhase(animation);

    std::vector<Element> content {
        hbox({
            titleText("Confirm command"),
            filler(),
            actionBadge(action),
            text("  "),
            text("Enter/r: run  Esc/c/right click: cancel  q: back") | dim,
        }),
        animatedAccent(animation, Terminal::Size().dimx) | color(pulseColor(phase)),
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
        confirmButton("RUN COMMAND", "Enter / r / click", Color::GreenLight,
                      Color::GreenLight, runButtonBox) | flex,
        text("  "),
        confirmButton("CANCEL", "Esc / c / q / click", Color::Yellow,
                      Color::Yellow, cancelButtonBox) | flex,
    }));

    return appTheme(vbox(content) | borderStyled(ROUNDED, Color::CyanLight));
}

Element renderExitConfirmation(const UiAnimationState& animation) {
    animation::RequestAnimationFrame();

    std::vector<Element> content {
        hbox({
            titleText("Exit ZPM TUI"),
            filler(),
            text("Enter/y: exit  Esc/n/q: back") | dim,
        }),
        animatedAccent(animation, Terminal::Size().dimx),
        separatorStyled(LIGHT),
        paragraphAlignLeft("Do you want to close the terminal interface and return to the shell?"),
        separator(),
        hbox({
            text(" Exit ") | bold | color(Color::Black) | bgcolor(Color::RedLight),
            text("  "),
            text(" Back ") | bold | color(Color::Black) | bgcolor(Color::GreenLight),
        }),
    };

    return appTheme(vbox(content) | borderStyled(ROUNDED, Color::RedLight));
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
            prepareTerminalForTui();
            auto screen = ScreenInteractive::Fullscreen();
            screen.TrackMouse(true);
            UiAnimationState animation;

            std::vector<std::string> categoryEntries;
            categoryEntries.reserve(categories.size());
            for (const Category& category : categories) {
                categoryEntries.push_back(category.title);
            }

            std::vector<std::string> actionEntries;
            bool showConfirm = false;
            bool showExitConfirm = false;
            Box actionPanelBox;
            Box confirmRunButtonBox;
            Box confirmCancelButtonBox;
            auto lastActionClickTime = std::chrono::steady_clock::time_point {};
            int lastActionClickX = -1;
            int lastActionClickY = -1;
            constexpr auto doubleClickWindow = std::chrono::milliseconds(420);

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

            auto selectCurrentAction = [&] {
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
                    showExitConfirm = true;
                    return;
                }
                if (action.args.empty()) {
                    return;
                }
                pendingAction = action;
                showConfirm = true;
            };

            auto moveFocusedSelection = [&](int delta) {
                syncActionEntries();
                if (focusedPane == 0) {
                    const int before = selectedCategory;
                    selectedCategory = clampIndex(selectedCategory + delta, categories.size());
                    if (selectedCategory != before) {
                        selectedAction = 0;
                    }
                    syncActionEntries();
                    return;
                }

                if (categories.empty()) {
                    return;
                }
                const Category& category = categories[static_cast<std::size_t>(selectedCategory)];
                selectedAction = clampIndex(selectedAction + delta, category.actions.size());
            };

            MenuOption categoryOption = zpmMenuOption();
            categoryOption.on_change = [&] {
                selectedAction = 0;
                syncActionEntries();
            };
            categoryOption.on_enter = [&] {
                focusedPane = 1;
            };

            MenuOption actionOption = zpmMenuOption();
            actionOption.on_enter = [&] {
                selectCurrentAction();
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
                                  focusedPane,
                                  actionPanelBox,
                                  animation);
            });

            Component rootRenderer = Renderer(menuView, [&] {
                if (showExitConfirm) {
                    return renderExitConfirmation(animation);
                }
                return showConfirm
                    ? renderConfirmation(pendingAction, animation,
                                         confirmRunButtonBox, confirmCancelButtonBox)
                    : menuView->Render();
            });

            Component root = CatchEvent(rootRenderer, [&](Event event) {
                if (showExitConfirm) {
                    if (event.is_mouse()) {
                        const Mouse mouse = event.mouse();
                        if (mouse.button == Mouse::Right && mouse.motion == Mouse::Pressed) {
                            showExitConfirm = false;
                            return true;
                        }
                        return true;
                    }
                    if (event == Event::Return || event == Event::Character('y')) {
                        screen.Exit();
                        return true;
                    }
                    if (event == Event::Escape || event == Event::Character('n') ||
                        event == Event::Character('q') || event == Event::Character('c')) {
                        showExitConfirm = false;
                        return true;
                    }
                    return true;
                }

                if (showConfirm) {
                    if (event.is_mouse()) {
                        const Mouse mouse = event.mouse();
                        if (mouse.button == Mouse::Left && mouse.motion == Mouse::Pressed) {
                            if (confirmRunButtonBox.Contain(mouse.x, mouse.y)) {
                                selectedLaunch = pendingAction;
                                shouldLaunch = true;
                                screen.Exit();
                                return true;
                            }
                            if (confirmCancelButtonBox.Contain(mouse.x, mouse.y)) {
                                showConfirm = false;
                                return true;
                            }
                        }
                        if (mouse.button == Mouse::Right && mouse.motion == Mouse::Pressed) {
                            showConfirm = false;
                            return true;
                        }
                        return true;
                    }
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
                        showConfirm = false;
                        focusedPane = 0;
                        return true;
                    }
                    return true;
                }

                if (event.is_mouse()) {
                    const Mouse mouse = event.mouse();
                    if (mouse.button == Mouse::WheelUp) {
                        moveFocusedSelection(-1);
                        return true;
                    }
                    if (mouse.button == Mouse::WheelDown) {
                        moveFocusedSelection(1);
                        return true;
                    }
                    if (mouse.button == Mouse::Left && mouse.motion == Mouse::Pressed) {
                        if (!actionPanelBox.Contain(mouse.x, mouse.y)) {
                            lastActionClickTime = {};
                            lastActionClickX = -1;
                            lastActionClickY = -1;
                            return false;
                        }

                        const auto now = std::chrono::steady_clock::now();
                        const bool sameSpot = mouse.x == lastActionClickX &&
                                              mouse.y == lastActionClickY;
                        const bool fastEnough =
                            now - lastActionClickTime <= doubleClickWindow;

                        if (focusedPane == 1 && sameSpot && fastEnough) {
                            selectCurrentAction();
                            lastActionClickTime = {};
                            lastActionClickX = -1;
                            lastActionClickY = -1;
                            return true;
                        }

                        lastActionClickTime = now;
                        lastActionClickX = mouse.x;
                        lastActionClickY = mouse.y;
                    }
                    return false;
                }

                if (event == Event::Escape) {
                    showExitConfirm = true;
                    return true;
                }
                if (event == Event::Character('q')) {
                    if (focusedPane == 1) {
                        focusedPane = 0;
                        return true;
                    }
                    showExitConfirm = true;
                    return true;
                }
                if (event == Event::Character('j')) {
                    moveFocusedSelection(1);
                    return true;
                }
                if (event == Event::Character('k')) {
                    moveFocusedSelection(-1);
                    return true;
                }
                if (event == Event::Tab) {
                    focusedPane = focusedPane == 0 ? 1 : 0;
                    return true;
                }
                if (event == Event::ArrowRight || event == Event::Character('l')) {
                    focusedPane = 1;
                    return true;
                }
                if (event == Event::ArrowLeft || event == Event::Character('h')) {
                    focusedPane = 0;
                    return true;
                }
                if (event == Event::Character(' ')) {
                    if (focusedPane == 0) {
                        focusedPane = 1;
                    } else {
                        selectCurrentAction();
                    }
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
