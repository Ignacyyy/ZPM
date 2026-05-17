// zrun.cpp — part of ZPM
// Launch an installed application from APT, Flatpak, or Snap.
// If found in multiple sources, presents an arrow-key menu to choose.

#include "main.h"
#include <termios.h>

using namespace std;

// ─── ANSI arrow-key menu ──────────────────────────────────────────────────────

struct MenuItem {
    string label;   // e.g. "APT:     firefox"
    string source;  // "apt" | "flatpak" | "snap"
    string command; // exact command to exec
};

// Put terminal in raw mode so we can read arrow keys without Enter
static struct termios g_origTermios;
static void enableRaw() {
    tcgetattr(STDIN_FILENO, &g_origTermios);
    struct termios raw = g_origTermios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}
static void disableRaw() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_origTermios);
}

// Returns index of chosen item, or -1 if user pressed q/Esc/Ctrl-C
static int arrowMenu(const string& pkg, const vector<MenuItem>& items) {
    if (items.empty()) return -1;

    int selected = 0;
    int n = static_cast<int>(items.size());

    // How many lines the menu occupies (header + items + hint)
    int menuLines = n + 2;

    auto draw = [&](bool first) {
        if (!first)
            cout << "\033[" << menuLines << "A"; // move cursor back up

        cout << "\r\033[K" << BOLD << "Run: " << CYAN << pkg << RESET << "\n";
        for (int i = 0; i < n; ++i) {
            if (i == selected)
                cout << "\r\033[K  " << GREEN << "▶ " << BOLD << items[i].label << RESET << "\n";
            else
                cout << "\r\033[K    " << items[i].label << "\n";
        }
        cout << "\r\033[K" << YELLOW << "  ↑↓ move  Enter launch  q quit" << RESET << "\n";
        cout << flush;
    };

    cout << "\n";
    draw(true);
    enableRaw();

    while (true) {
        unsigned char c = 0;
        if (read(STDIN_FILENO, &c, 1) <= 0) { disableRaw(); return -1; }

        if (c == '\r' || c == '\n') {
            disableRaw();
            return selected;
        }

        if (c == 'q' || c == 'Q' || c == 3 /* Ctrl-C */) {
            disableRaw();
            return -1;
        }

        if (c == '\033') {
            // Read the next two bytes of the escape sequence
            unsigned char seq[2] = {0, 0};
            if (read(STDIN_FILENO, &seq[0], 1) <= 0) { disableRaw(); return -1; }
            if (seq[0] != '[') continue; // unknown escape, ignore
            if (read(STDIN_FILENO, &seq[1], 1) <= 0) { disableRaw(); return -1; }

            if      (seq[1] == 'A') selected = (selected - 1 + n) % n; // Up
            else if (seq[1] == 'B') selected = (selected + 1) % n;     // Down
            // anything else: ignore

            draw(false);
        }
    }
}

// ─── detection helpers ────────────────────────────────────────────────────────

static bool aptInstalled(const string& pkg) {
    string cmd = "dpkg-query -W -f='${Status}' " + pkg + " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return false;
    char buf[128]; string status;
    if (fgets(buf, sizeof(buf), p)) status = buf;
    pclose(p);
    return status.find("install ok installed") != string::npos;
}

static bool snapInstalled(const string& pkg) {
    return system(("snap list " + pkg + " >/dev/null 2>&1").c_str()) == 0;
}

// Returns the Flatpak app-id if installed (exact match on app-id or name),
// or empty string if not found.
static string flatpakInstalled(const string& pkg) {
    // Try exact app-id first (e.g. "org.mozilla.firefox")
    string cmd = "flatpak list --columns=application 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return "";
    char buf[256];
    vector<string> ids;
    while (fgets(buf, sizeof(buf), p)) {
        string line = buf;
        line.erase(line.find_last_not_of(" \n\r\t") + 1);
        if (!line.empty()) ids.push_back(line);
    }
    pclose(p);

    string pkgLower = pkg;
    transform(pkgLower.begin(), pkgLower.end(), pkgLower.begin(), ::tolower);

    // 1. exact app-id match
    for (const auto& id : ids)
        if (id == pkg) return id;

    // 2. last component of app-id matches (e.g. "firefox" matches "org.mozilla.firefox")
    for (const auto& id : ids) {
        size_t dot = id.rfind('.');
        string tail = (dot != string::npos) ? id.substr(dot + 1) : id;
        string tailLow = tail;
        transform(tailLow.begin(), tailLow.end(), tailLow.begin(), ::tolower);
        if (tailLow == pkgLower) return id;
    }

    // 3. substring match on app-id
    for (const auto& id : ids) {
        string idLow = id;
        transform(idLow.begin(), idLow.end(), idLow.begin(), ::tolower);
        if (idLow.find(pkgLower) != string::npos) return id;
    }

    return "";
}

// ─── launchers ────────────────────────────────────────────────────────────────

// Try to find the binary for an APT package and exec it detached.
// We use `dpkg -L <pkg>` to list files and pick the first executable in bin/.
static string aptFindBinary(const string& pkg) {
    string cmd = "dpkg -L " + pkg + " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return pkg; // fallback: hope it's on PATH
    char buf[256];
    string best;
    while (fgets(buf, sizeof(buf), p)) {
        string f = buf;
        f.erase(f.find_last_not_of(" \n\r\t") + 1);
        if (f.find("/usr/bin/") == 0 || f.find("/bin/") == 0) {
            if (best.empty()) best = f;
        }
    }
    pclose(p);
    // Fallback: just use the package name (often the binary name)
    return best.empty() ? pkg : best;
}

static void launchAPT(const string& pkg) {
    string binary = aptFindBinary(pkg);
    cout << GREEN << "Launching (APT): " << RESET << binary << "\n";
    pid_t pid = fork();
    if (pid == 0) {
        // Detach from terminal
        setsid();
        // Redirect stdout/stderr to /dev/null so the terminal stays clean
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
        execlp(binary.c_str(), binary.c_str(), nullptr);
        // If binary path failed, try just the package name
        execlp(pkg.c_str(), pkg.c_str(), nullptr);
        _exit(127);
    }
    // Parent returns immediately — app runs in background
}

static void launchFlatpak(const string& appId) {
    cout << GREEN << "Launching (Flatpak): " << RESET << appId << "\n";
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
        execlp("flatpak", "flatpak", "run", appId.c_str(), nullptr);
        _exit(127);
    }
}

static void launchSnap(const string& pkg) {
    cout << GREEN << "Launching (Snap): " << RESET << pkg << "\n";
    // Snaps install a wrapper in /snap/bin/<name>
    string snapBin = "/snap/bin/" + pkg;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
        execlp(snapBin.c_str(), snapBin.c_str(), nullptr);
        // fallback: try name on PATH
        execlp(pkg.c_str(), pkg.c_str(), nullptr);
        _exit(127);
    }
}

// ─── main ─────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    zpm_update::checkForUpdates();

    bool showHelp    = false;
    bool showVersion = false;
    string pkg;

    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        if      (arg == "--help"    || arg == "-h") showHelp = true;
        else if (arg == "--version" || arg == "-v") showVersion = true;
        else if (pkg.empty()) pkg = arg;
    }

    if (showVersion && showHelp) {
        cout << YELLOW << "--version\n" << RESET;
        cout << RED << "zrun component version: " << zpm_version::version() << " of ZPM\n" << RESET;
        cout << "https://github.com/Ignacyyy/ZPM\n";
        cout << "Copyright (c) 2026 Ignacyyy\n";
        cout << "License: MIT\n\n";
        cout << YELLOW << "--help\n" << RESET;
        cout << RED << "Usage: " << RESET << argv[0] << " <package> or zpm run <package>\n\n";
        cout << RED << "Options:\n" << RESET;
        cout << "  Finds the app in APT / Flatpak / Snap and launches it.\n";
        cout << "  If installed from multiple sources, lets you pick with ↑↓.\n\n";
        cout << "  --version, -v   Show version information\n";
        cout << "  --help,    -h   Show this help message\n";
        return 0;
    }

    if (showVersion) {
        cout << RED << "zrun component version: " << zpm_version::version() << " of ZPM\n" << RESET;
        cout << "https://github.com/Ignacyyy/ZPM\n";
        cout << "Copyright (c) 2026 Ignacyyy\nLicense: MIT\n";
        return 0;
    }

    if (showHelp) {
        cout << RED << "Usage: " << RESET << argv[0] << " <package> or zpm run <package>\n\n";
        cout << RED << "Options:\n" << RESET;
        cout << "  Finds the app in APT / Flatpak / Snap and launches it.\n";
        cout << "  If installed from multiple sources, lets you pick with ↑↓.\n\n";
        cout << "  --version, -v   Show version information\n";
        cout << "  --help,    -h   Show this help message\n";
        return showHelp ? 0 : 1;
    }
    if (pkg.empty()) {
        cout << RED << "No package specified\n" << RESET;
        return 0;
    }

    // ── check which package managers are available ────────────────────────────
    bool hasFlatpak = (system("command -v flatpak >/dev/null 2>&1") == 0);
    bool hasSnap    = (system("command -v snap    >/dev/null 2>&1") == 0);

    // ── search each source ────────────────────────────────────────────────────
    cout << YELLOW << "Searching for '" << pkg << "'...\r" << RESET << flush;

    bool   aptFound     = aptInstalled(pkg);
    string flatpakAppId = hasFlatpak ? flatpakInstalled(pkg) : "";
    bool   snapFound    = hasSnap    && snapInstalled(pkg);

    cout << "\033[K"; // clear the searching line

    // ── build menu ────────────────────────────────────────────────────────────
    vector<MenuItem> items;

    if (aptFound) {
        string bin = aptFindBinary(pkg);
        items.push_back({"APT:     " + pkg + "  (" + bin + ")", "apt", bin});
    }
    if (!flatpakAppId.empty()) {
        items.push_back({"Flatpak: " + flatpakAppId, "flatpak", flatpakAppId});
    }
    if (snapFound) {
        items.push_back({"Snap:    " + pkg, "snap", pkg});
    }

    if (items.empty()) {
        cout << RED << "'" << pkg << "' is not installed.\n" << RESET;
        return 1;
    }

    // ── if only one source, skip menu ─────────────────────────────────────────
    int chosen = 0;
    if (items.size() > 1) {
        chosen = arrowMenu(pkg, items);
        cout << "\n";
        if (chosen < 0) {
            cout << YELLOW << "Cancelled.\n" << RESET;
            return 0;
        }
    } else {
        cout << BOLD << "Found: " << RESET << items[0].label << "\n";
    }

    // ── launch ────────────────────────────────────────────────────────────────
    const MenuItem& sel = items[chosen];
    if      (sel.source == "apt")     launchAPT(pkg);
    else if (sel.source == "flatpak") launchFlatpak(sel.command);
    else if (sel.source == "snap")    launchSnap(pkg);

    return 0;
}
