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

enum class ActionOptionsKind {
    None,
    Update,
    SearchPackages,
    ListPackages,
    UpgradeZpm,
    HomePages,
};

enum class PackageSourceFilter {
    All,
    Native,
    Flatpak,
    Snap,
};

enum class PagerMode {
    NoPager,
    Pager,
};

struct Action {
    std::string title;
    std::string commandPreview;
    std::vector<std::string> args;
    std::string hint;
    bool needsRoot = false;
    bool exits = false;
    std::string warning;
    ActionOptionsKind optionsKind = ActionOptionsKind::None;

    Action() = default;

    Action(std::string titleValue,
           std::string commandPreviewValue,
           std::vector<std::string> argsValue,
           std::string hintValue,
           bool needsRootValue = false,
           bool exitsValue = false,
           std::string warningValue = {},
           ActionOptionsKind optionsKindValue = ActionOptionsKind::None)
        : title(std::move(titleValue)),
          commandPreview(std::move(commandPreviewValue)),
          args(std::move(argsValue)),
          hint(std::move(hintValue)),
          needsRoot(needsRootValue),
          exits(exitsValue),
          warning(std::move(warningValue)),
          optionsKind(optionsKindValue) {}
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

struct UpdateOptions {
    bool full = false;
    bool yes = false;
    bool reboot = false;
    bool shutdown = false;
};

struct BrowseOptions {
    PackageSourceFilter source = PackageSourceFilter::All;
    PagerMode pager = PagerMode::NoPager;
};

struct UpgradeOptions {
    bool experimental = false;
    bool force = false;
};

struct HomePageOptions {
    bool defaultPage = true;
    std::array<bool, 3> pages {};
};

struct ConfigurableOptions {
    UpdateOptions update;
    BrowseOptions browse;
    UpgradeOptions upgrade;
    HomePageOptions home;
};

constexpr int kPaneCategories = 0;
constexpr int kPaneActions = 1;
constexpr int kPaneOptions = 2;
constexpr int kMaxOptionCount = 6;

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
    std::string lookupName = name;
    if (name.rfind("./", 0) == 0 && name.find('/', 2) == std::string::npos) {
        lookupName = name.substr(2);
    } else if (name.find('/') != std::string::npos) {
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
        const std::string candidate = joinPath(directory, lookupName);
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

void toggleUpdateOption(UpdateOptions& options, int option) {
    switch (option) {
    case 0:
        options.full = !options.full;
        break;
    case 1:
        options.yes = !options.yes;
        break;
    case 2:
        options.reboot = !options.reboot;
        if (options.reboot) {
            options.shutdown = false;
        }
        break;
    case 3:
        options.shutdown = !options.shutdown;
        if (options.shutdown) {
            options.reboot = false;
        }
        break;
    default:
        break;
    }
}

std::string sourceFlag(PackageSourceFilter source) {
    switch (source) {
    case PackageSourceFilter::Native:
        return "--native";
    case PackageSourceFilter::Flatpak:
        return "--flatpak";
    case PackageSourceFilter::Snap:
        return "--snap";
    case PackageSourceFilter::All:
        return {};
    }
    return {};
}

std::string sourceLabel(PackageSourceFilter source) {
    switch (source) {
    case PackageSourceFilter::Native:
        return "native packages";
    case PackageSourceFilter::Flatpak:
        return "Flatpak packages";
    case PackageSourceFilter::Snap:
        return "Snap packages";
    case PackageSourceFilter::All:
        return "all package sources";
    }
    return "all package sources";
}

void appendArg(std::vector<std::string>& args, std::string& preview, const std::string& arg) {
    if (arg.empty()) {
        return;
    }
    args.push_back(arg);
    preview += " " + arg;
}

void toggleHomePageOption(HomePageOptions& options, int option) {
    if (option == 0) {
        options.defaultPage = true;
        options.pages.fill(false);
        return;
    }

    const int pageIndex = option - 1;
    if (pageIndex < 0 || pageIndex >= static_cast<int>(options.pages.size())) {
        return;
    }

    options.pages[static_cast<std::size_t>(pageIndex)] =
        !options.pages[static_cast<std::size_t>(pageIndex)];
    options.defaultPage = !std::any_of(options.pages.begin(), options.pages.end(),
                                       [](bool selected) { return selected; });
}

void setPackageSource(BrowseOptions& options, int option) {
    switch (option) {
    case 0:
        options.source = PackageSourceFilter::All;
        break;
    case 1:
        options.source = PackageSourceFilter::Native;
        break;
    case 2:
        options.source = PackageSourceFilter::Flatpak;
        break;
    case 3:
        options.source = PackageSourceFilter::Snap;
        break;
    default:
        break;
    }
}

void toggleActionOption(ConfigurableOptions& options, ActionOptionsKind kind, int option) {
    switch (kind) {
    case ActionOptionsKind::Update:
        toggleUpdateOption(options.update, option);
        break;
    case ActionOptionsKind::SearchPackages:
        setPackageSource(options.browse, option);
        break;
    case ActionOptionsKind::ListPackages:
        if (option <= 3) {
            setPackageSource(options.browse, option);
        } else if (option == 4) {
            options.browse.pager = PagerMode::NoPager;
        } else if (option == 5) {
            options.browse.pager = PagerMode::Pager;
        }
        break;
    case ActionOptionsKind::UpgradeZpm:
        if (option == 0) {
            options.upgrade.experimental = !options.upgrade.experimental;
        } else if (option == 1) {
            options.upgrade.force = !options.upgrade.force;
        }
        break;
    case ActionOptionsKind::HomePages:
        toggleHomePageOption(options.home, option);
        break;
    case ActionOptionsKind::None:
        break;
    }
}

Action applyActionOptions(Action action, const ConfigurableOptions& options) {
    switch (action.optionsKind) {
    case ActionOptionsKind::Update: {
        const UpdateOptions& update = options.update;
        action.args = {"zpm", "update"};
        action.commandPreview = "zpm update";

        if (update.full) {
            action.args.push_back("--full");
            action.commandPreview += " --full";
        }
        if (update.yes) {
            action.args.push_back("--yes");
            action.commandPreview += " --yes";
        }
        if (update.reboot) {
            action.args.push_back("--reboot");
            action.commandPreview += " --reboot";
        } else if (update.shutdown) {
            action.args.push_back("--shutdown");
            action.commandPreview += " --shutdown";
        }

        action.hint = update.full ? "Runs the full update flow" : "Runs the standard update flow";
        action.hint += update.yes ? " with automatic confirmations" : " and leaves native prompts visible";
        if (update.reboot) {
            action.hint += ", then requests a reboot";
        } else if (update.shutdown) {
            action.hint += ", then requests a shutdown";
        }
        action.hint += ".";

        if (update.reboot) {
            action.warning = "This can reboot the machine after updates finish.";
        } else if (update.shutdown) {
            action.warning = "This can shut down the machine after updates finish.";
        } else {
            action.warning.clear();
        }
        return action;
    }
    case ActionOptionsKind::SearchPackages: {
        std::string command = "zpm search";
        const std::string flag = sourceFlag(options.browse.source);
        if (!flag.empty()) {
            command += " " + flag;
        }
        action.commandPreview = command + " <query>";
        action.args = promptCommandArgs("Search query", command);
        action.hint = "Searches " + sourceLabel(options.browse.source) + ".";
        return action;
    }
    case ActionOptionsKind::ListPackages: {
        action.args = {"zpm", "list"};
        action.commandPreview = "zpm list";
        appendArg(action.args, action.commandPreview, sourceFlag(options.browse.source));
        appendArg(action.args,
                  action.commandPreview,
                  options.browse.pager == PagerMode::Pager ? "--pager" : "--no-pager");
        action.hint = "Lists " + sourceLabel(options.browse.source);
        action.hint += options.browse.pager == PagerMode::Pager
            ? " with the configured pager."
            : " directly in the terminal.";
        return action;
    }
    case ActionOptionsKind::UpgradeZpm: {
        action.args = {"zpm", "upgrade"};
        action.commandPreview = "zpm upgrade";
        if (options.upgrade.experimental) {
            appendArg(action.args, action.commandPreview, "--experimental");
        }
        if (options.upgrade.force) {
            appendArg(action.args, action.commandPreview, "--force");
        }
        action.hint = options.upgrade.experimental
            ? "Checks prerelease ZPM updates"
            : "Checks and installs stable ZPM updates when available";
        if (options.upgrade.force) {
            action.hint += " and forces reinstall when needed";
        }
        action.hint += ".";
        if (options.upgrade.experimental || options.upgrade.force) {
            action.warning = "Review upgrade options before running; this can replace the current ZPM build.";
        } else {
            action.warning.clear();
        }
        return action;
    }
    case ActionOptionsKind::HomePages: {
        action.args = {"zhome"};
        action.commandPreview = "zhome";
        bool anyPage = false;
        for (std::size_t index = 0; index < options.home.pages.size(); ++index) {
            if (!options.home.pages[index]) {
                continue;
            }
            anyPage = true;
            const std::string page = "-p" + std::to_string(index + 1);
            appendArg(action.args, action.commandPreview, page);
        }
        action.hint = anyPage
            ? "Opens the selected ZPM home pages in order."
            : "Opens the ZPM homepage/help interface.";
        return action;
    }
    case ActionOptionsKind::None:
        return action;
    }
    return action;
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
                    "Search packages",
                    "zpm search <query>",
                    promptCommandArgs("Search query", "zpm search"),
                    "Searches packages from the selected source.",
                    false,
                    false,
                    {},
                    ActionOptionsKind::SearchPackages,
                },
                {
                    "Package info",
                    "zpm info <package>",
                    promptCommandArgs("Package to inspect", "zpm info"),
                    "Shows package metadata from native PM, Flatpak, or Snap.",
                    false,
                },
                {
                    "List packages",
                    "zpm list --no-pager",
                    {"zpm", "list", "--no-pager"},
                    "Lists installed packages from the selected source.",
                    false,
                    false,
                    {},
                    ActionOptionsKind::ListPackages,
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
            "Update flow with selectable full, automatic, reboot, and shutdown options.",
            {
                {
                    "Update",
                    "zpm update",
                    {"zpm", "update"},
                    "Runs the standard update flow and leaves native prompts visible.",
                    true,
                    false,
                    {},
                    ActionOptionsKind::Update,
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
                    "./zhome -ed",
                    {"./zhome", "-ed"},
                    "Opens the active ZPM configuration file through zhome.",
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
                    false,
                    {},
                    ActionOptionsKind::UpgradeZpm,
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
                    "zhome",
                    {"zhome"},
                    "Opens the selected ZPM home pages.",
                    false,
                    false,
                    {},
                    ActionOptionsKind::HomePages,
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

Element updateOptionRow(const std::string& label,
                        const std::string& hint,
                        bool checked,
                        bool focused,
                        Color activeColor,
                        Box& box) {
    Element marker = text(checked ? "[x]" : "[ ]") |
                     bold |
                     color(checked ? activeColor : Color::GrayLight);
    Element row = hbox({
        text(focused ? "> " : "  ") | color(Color::CyanLight),
        marker,
        text(" " + label + " ") | bold,
        filler(),
        muted(hint),
    });

    if (focused) {
        row = row | color(Color::CyanLight);
    }

    return row | reflect(box);
}

int optionCountFor(ActionOptionsKind kind) {
    switch (kind) {
    case ActionOptionsKind::Update:
        return 4;
    case ActionOptionsKind::SearchPackages:
    case ActionOptionsKind::HomePages:
        return 4;
    case ActionOptionsKind::ListPackages:
        return 6;
    case ActionOptionsKind::UpgradeZpm:
        return 2;
    case ActionOptionsKind::None:
        return 0;
    }
    return 0;
}

bool hasOptions(const Action& action) {
    return optionCountFor(action.optionsKind) > 0;
}

Element renderOptionsPanel(const Action& action,
                           const ConfigurableOptions& options,
                           int selectedOption,
                           bool focused,
                           std::array<Box, kMaxOptionCount>& boxes) {
    const int optionCount = optionCountFor(action.optionsKind);
    selectedOption = clampIndex(selectedOption, static_cast<std::size_t>(optionCount));

    std::vector<Element> rows {
        sectionTitle("Options"),
    };

    switch (action.optionsKind) {
    case ActionOptionsKind::Update:
        rows.push_back(updateOptionRow("Full update", "--full", options.update.full,
                                       focused && selectedOption == 0,
                                       Color::GreenLight, boxes[0]));
        rows.push_back(updateOptionRow("Automatic confirmations", "--yes", options.update.yes,
                                       focused && selectedOption == 1,
                                       Color::GreenLight, boxes[1]));
        rows.push_back(updateOptionRow("Reboot after update", "--reboot", options.update.reboot,
                                       focused && selectedOption == 2,
                                       Color::RedLight, boxes[2]));
        rows.push_back(updateOptionRow("Shutdown after update", "--shutdown", options.update.shutdown,
                                       focused && selectedOption == 3,
                                       Color::RedLight, boxes[3]));
        rows.push_back(separatorStyled(LIGHT));
        rows.push_back(paragraphAlignLeft("Reboot and shutdown are mutually exclusive.") | dim);
        break;
    case ActionOptionsKind::SearchPackages:
        rows.push_back(updateOptionRow("All sources", "native + Flatpak + Snap",
                                       options.browse.source == PackageSourceFilter::All,
                                       focused && selectedOption == 0,
                                       Color::GreenLight, boxes[0]));
        rows.push_back(updateOptionRow("Native only", "--native",
                                       options.browse.source == PackageSourceFilter::Native,
                                       focused && selectedOption == 1,
                                       Color::GreenLight, boxes[1]));
        rows.push_back(updateOptionRow("Flatpak only", "--flatpak",
                                       options.browse.source == PackageSourceFilter::Flatpak,
                                       focused && selectedOption == 2,
                                       Color::GreenLight, boxes[2]));
        rows.push_back(updateOptionRow("Snap only", "--snap",
                                       options.browse.source == PackageSourceFilter::Snap,
                                       focused && selectedOption == 3,
                                       Color::GreenLight, boxes[3]));
        break;
    case ActionOptionsKind::ListPackages:
        rows.push_back(updateOptionRow("All sources", "native + Flatpak + Snap",
                                       options.browse.source == PackageSourceFilter::All,
                                       focused && selectedOption == 0,
                                       Color::GreenLight, boxes[0]));
        rows.push_back(updateOptionRow("Native only", "--native",
                                       options.browse.source == PackageSourceFilter::Native,
                                       focused && selectedOption == 1,
                                       Color::GreenLight, boxes[1]));
        rows.push_back(updateOptionRow("Flatpak only", "--flatpak",
                                       options.browse.source == PackageSourceFilter::Flatpak,
                                       focused && selectedOption == 2,
                                       Color::GreenLight, boxes[2]));
        rows.push_back(updateOptionRow("Snap only", "--snap",
                                       options.browse.source == PackageSourceFilter::Snap,
                                       focused && selectedOption == 3,
                                       Color::GreenLight, boxes[3]));
        rows.push_back(separatorStyled(LIGHT));
        rows.push_back(updateOptionRow("Print directly", "--no-pager",
                                       options.browse.pager == PagerMode::NoPager,
                                       focused && selectedOption == 4,
                                       Color::CyanLight, boxes[4]));
        rows.push_back(updateOptionRow("Use pager", "--pager",
                                       options.browse.pager == PagerMode::Pager,
                                       focused && selectedOption == 5,
                                       Color::CyanLight, boxes[5]));
        break;
    case ActionOptionsKind::UpgradeZpm:
        rows.push_back(updateOptionRow("Experimental prerelease", "--experimental",
                                       options.upgrade.experimental,
                                       focused && selectedOption == 0,
                                       Color::YellowLight, boxes[0]));
        rows.push_back(updateOptionRow("Force reinstall", "--force",
                                       options.upgrade.force,
                                       focused && selectedOption == 1,
                                       Color::RedLight, boxes[1]));
        break;
    case ActionOptionsKind::HomePages:
        rows.push_back(updateOptionRow("Home", "zhome",
                                       options.home.defaultPage,
                                       focused && selectedOption == 0,
                                       Color::GreenLight, boxes[0]));
        rows.push_back(updateOptionRow("PAGE 1", "ARM + config",
                                       options.home.pages[0],
                                       focused && selectedOption == 1,
                                       Color::GreenLight, boxes[1]));
        rows.push_back(updateOptionRow("PAGE 2", "wrapper commands",
                                       options.home.pages[1],
                                       focused && selectedOption == 2,
                                       Color::GreenLight, boxes[2]));
        rows.push_back(updateOptionRow("PAGE 3", "aliases",
                                       options.home.pages[2],
                                       focused && selectedOption == 3,
                                       Color::GreenLight, boxes[3]));
        break;
    case ActionOptionsKind::None:
        break;
    }

    return vbox(std::move(rows)) |
           borderStyled(ROUNDED, focused ? Color::CyanLight : Color::GrayDark);
}

Element renderMenu(Component categoryMenu,
                   Component actionMenu,
                   const SystemStatus& status,
                   const std::vector<Category>& categories,
                   int selectedCategory,
                   int selectedAction,
                   const ConfigurableOptions& configurableOptions,
                   int selectedOption,
                   int focusedPane,
                   Box& actionPanelBox,
                   std::array<Box, kMaxOptionCount>& optionBoxes,
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
    const Action& selected = category.actions[static_cast<std::size_t>(actionIndex)];
    const Action action = applyActionOptions(selected, configurableOptions);
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
            "Use Left/Right, h/l, or Tab to switch between Categories, Actions, "
            "and Options when they are available. Enter selects the highlighted action. "
            "Press ? to jump to Help."));
        details.push_back(separator());
    }

    details.push_back(hbox({
        text(action.title) | bold,
        filler(),
    }));
    details.push_back(commandPreviewBox(action.commandPreview));

    if (hasOptions(selected)) {
        details.push_back(separator());
        details.push_back(renderOptionsPanel(selected,
                                             configurableOptions,
                                             selectedOption,
                                             focusedPane == kPaneOptions,
                                             optionBoxes));
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
                                    focusedPane == kPaneCategories);
    Element actionsPanel = panel("Actions (" + std::to_string(category.actions.size()) + ")",
                                 actionMenu->Render() | vscroll_indicator | frame,
                                 focusedPane == kPaneActions);
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

    std::vector<Element> shortcutHints {
        keyHint("Up/Down", "move"),
        text("  "),
        keyHint("Wheel", "scroll"),
        text("  "),
        keyHint("Enter", "select"),
        text("  "),
        keyHint("?", "help"),
        text("  "),
        keyHint("q", "back/confirm exit"),
        text("  "),
        keyHint("Esc", "exit/cancel"),
    };
    if (hasOptions(selected)) {
        shortcutHints.push_back(text("  "));
        shortcutHints.push_back(keyHint("Space", "toggle option"));
    }

    return appTheme(vbox({
               hbox({
                   text(" ZPM ") | bold | color(Color::Black) | bgcolor(Color::CyanLight),
                   text(" TUI ") | bold | color(Color::Black) | bgcolor(Color::GreenLight),
                   text(" " + category.title + " / " + action.title + " ") | bold,
                   filler(),
                   text(compact ? "?: help  q: back/confirm exit  Enter: select"
                                : "Mouse + wheel  Left/Right/Tab: panel  ?: help  q/Esc: confirm exit")
                       | dim,
               }),
               hbox(std::move(shortcutHints)) | size(HEIGHT, EQUAL, 1),
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

Element renderExitConfirmation(const UiAnimationState& animation,
                               Box& exitButtonBox,
                               Box& backButtonBox) {
    animation::RequestAnimationFrame();

    std::vector<Element> content {
        hbox({
            titleText("Exit ZPM TUI"),
            filler(),
            text("Enter/y/click: exit  Esc/n/q/right click: back") | dim,
        }),
        animatedAccent(animation, Terminal::Size().dimx),
        separatorStyled(LIGHT),
        paragraphAlignLeft("Do you want to close the terminal interface and return to the shell?"),
        separator(),
        hbox({
            confirmButton("EXIT", "Enter / y / click", Color::RedLight,
                          Color::RedLight, exitButtonBox) | flex,
            text("  "),
            confirmButton("BACK", "Esc / n / q / click", Color::GreenLight,
                          Color::GreenLight, backButtonBox) | flex,
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
        int selectedOption = 0;
        int focusedPane = kPaneCategories;
        ConfigurableOptions configurableOptions;

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
            std::array<Box, kMaxOptionCount> optionBoxes;
            Box confirmRunButtonBox;
            Box confirmCancelButtonBox;
            Box exitConfirmButtonBox;
            Box exitBackButtonBox;
            auto lastActionClickTime = std::chrono::steady_clock::time_point {};
            int lastActionClickX = -1;
            int lastActionClickY = -1;
            constexpr auto doubleClickWindow = std::chrono::milliseconds(420);

            auto currentOptionsKind = [&]() -> ActionOptionsKind {
                if (categories.empty()) {
                    return ActionOptionsKind::None;
                }
                const int categoryIndex = clampIndex(selectedCategory, categories.size());
                const Category& category = categories[static_cast<std::size_t>(categoryIndex)];
                if (category.actions.empty()) {
                    return ActionOptionsKind::None;
                }
                const int actionIndex = clampIndex(selectedAction, category.actions.size());
                return category.actions[static_cast<std::size_t>(actionIndex)].optionsKind;
            };

            auto currentActionHasOptions = [&]() -> bool {
                return optionCountFor(currentOptionsKind()) > 0;
            };

            auto availablePaneCount = [&]() {
                return currentActionHasOptions() ? 3 : 2;
            };

            auto syncActionEntries = [&] {
                if (categories.empty()) {
                    selectedCategory = 0;
                    selectedAction = 0;
                    selectedOption = 0;
                    focusedPane = kPaneCategories;
                    actionEntries.clear();
                    return;
                }
                selectedCategory = clampIndex(selectedCategory, categories.size());
                actionEntries.clear();
                const Category& category = categories[static_cast<std::size_t>(selectedCategory)];
                actionEntries.reserve(category.actions.size());
                for (const Action& action : category.actions) {
                    actionEntries.push_back(action.title);
                }

                selectedAction = clampIndex(selectedAction, actionEntries.size());
                selectedOption = clampIndex(selectedOption,
                                            static_cast<std::size_t>(optionCountFor(currentOptionsKind())));
                focusedPane = clampIndex(focusedPane, availablePaneCount());
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
                Action action = category.actions[static_cast<std::size_t>(selectedAction)];
                action = applyActionOptions(std::move(action), configurableOptions);
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
                if (focusedPane == kPaneCategories) {
                    const int before = selectedCategory;
                    selectedCategory = clampIndex(selectedCategory + delta, categories.size());
                    if (selectedCategory != before) {
                        selectedAction = 0;
                    }
                    syncActionEntries();
                    return;
                }

                if (focusedPane == kPaneOptions && currentActionHasOptions()) {
                    selectedOption = clampIndex(
                        selectedOption + delta,
                        static_cast<std::size_t>(optionCountFor(currentOptionsKind())));
                    return;
                }

                if (categories.empty()) {
                    return;
                }
                const Category& category = categories[static_cast<std::size_t>(selectedCategory)];
                selectedAction = clampIndex(selectedAction + delta, category.actions.size());
            };

            auto focusNextPane = [&] {
                syncActionEntries();
                const int panes = availablePaneCount();
                focusedPane = (focusedPane + 1) % panes;
            };

            auto focusPreviousPane = [&] {
                syncActionEntries();
                const int panes = availablePaneCount();
                focusedPane = (focusedPane + panes - 1) % panes;
            };

            auto toggleSelectedOption = [&] {
                syncActionEntries();
                if (!currentActionHasOptions()) {
                    return false;
                }
                selectedOption = clampIndex(
                    selectedOption,
                    static_cast<std::size_t>(optionCountFor(currentOptionsKind())));
                toggleActionOption(configurableOptions, currentOptionsKind(), selectedOption);
                return true;
            };

            MenuOption categoryOption = zpmMenuOption();
            categoryOption.on_change = [&] {
                selectedAction = 0;
                syncActionEntries();
            };
            categoryOption.on_enter = [&] {
                focusedPane = kPaneActions;
            };

            MenuOption actionOption = zpmMenuOption();
            actionOption.on_enter = [&] {
                selectCurrentAction();
            };

            Component categoryMenu = Menu(&categoryEntries, &selectedCategory, categoryOption);
            Component actionMenu = Menu(&actionEntries, &selectedAction, actionOption);
            Component optionsFocus = Renderer([] { return text(""); });
            Component menuControls = Container::Horizontal(
                {categoryMenu, actionMenu, optionsFocus},
                &focusedPane);
            Component menuView = Renderer(menuControls, [&] {
                syncActionEntries();
                return renderMenu(categoryMenu,
                                  actionMenu,
                                  systemStatus,
                                  categories,
                                  selectedCategory,
                                  selectedAction,
                                  configurableOptions,
                                  selectedOption,
                                  focusedPane,
                                  actionPanelBox,
                                  optionBoxes,
                                  animation);
            });

            Component rootRenderer = Renderer(menuView, [&] {
                if (showExitConfirm) {
                    return renderExitConfirmation(animation,
                                                  exitConfirmButtonBox,
                                                  exitBackButtonBox);
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
                        if (mouse.button == Mouse::Left && mouse.motion == Mouse::Pressed) {
                            if (exitConfirmButtonBox.Contain(mouse.x, mouse.y)) {
                                screen.Exit();
                                return true;
                            }
                            if (exitBackButtonBox.Contain(mouse.x, mouse.y)) {
                                showExitConfirm = false;
                                return true;
                            }
                        }
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
                        focusedPane = kPaneCategories;
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
                        if (currentActionHasOptions()) {
                            const int optionCount = optionCountFor(currentOptionsKind());
                            for (int index = 0; index < optionCount; ++index) {
                                if (optionBoxes[static_cast<std::size_t>(index)].Contain(mouse.x, mouse.y)) {
                                    selectedOption = index;
                                    focusedPane = kPaneOptions;
                                    toggleActionOption(configurableOptions, currentOptionsKind(), selectedOption);
                                    lastActionClickTime = {};
                                    lastActionClickX = -1;
                                    lastActionClickY = -1;
                                    return true;
                                }
                            }
                        }

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

                        if (focusedPane == kPaneActions && sameSpot && fastEnough) {
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
                    if (focusedPane == kPaneOptions) {
                        focusedPane = kPaneActions;
                        return true;
                    }
                    if (focusedPane == kPaneActions) {
                        focusedPane = kPaneCategories;
                        return true;
                    }
                    showExitConfirm = true;
                    return true;
                }
                if (event == Event::Character('?')) {
                    for (std::size_t index = 0; index < categories.size(); ++index) {
                        if (categories[index].title == "Help") {
                            selectedCategory = static_cast<int>(index);
                            selectedAction = 0;
                            focusedPane = kPaneActions;
                            syncActionEntries();
                            break;
                        }
                    }
                    return true;
                }
                if (focusedPane == kPaneOptions && event == Event::ArrowDown) {
                    moveFocusedSelection(1);
                    return true;
                }
                if (focusedPane == kPaneOptions && event == Event::ArrowUp) {
                    moveFocusedSelection(-1);
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
                    focusNextPane();
                    return true;
                }
                if (event == Event::ArrowRight || event == Event::Character('l')) {
                    focusNextPane();
                    return true;
                }
                if (event == Event::ArrowLeft || event == Event::Character('h')) {
                    focusPreviousPane();
                    return true;
                }
                if (focusedPane == kPaneOptions && event == Event::Return) {
                    selectCurrentAction();
                    return true;
                }
                if (event == Event::Character(' ')) {
                    if (focusedPane == kPaneCategories) {
                        focusedPane = kPaneActions;
                    } else if (focusedPane == kPaneOptions) {
                        toggleSelectedOption();
                    } else if (currentActionHasOptions()) {
                        focusedPane = kPaneOptions;
                        toggleSelectedOption();
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
