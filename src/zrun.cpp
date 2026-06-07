// zrun.cpp — part of ZPM
// Launch an installed application from native PM, Flatpak, or Snap.

#include "main.h"
#include <termios.h>

using namespace std;

// ─── ANSI arrow-key menu ──────────────────────────────────────────────────────

struct MenuItem {
    string label;
    string source;  // "native" | "flatpak" | "snap"
    string command; // exact command to exec
    string pkg;     // rzeczywista nazwa pakietu
};

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

static int arrowMenu(const string& pkg, const vector<MenuItem>& items) {
    if (items.empty()) return -1;

    int selected  = 0;
    int n         = static_cast<int>(items.size());
    int menuLines = n + 2;

    auto draw = [&](bool first) {
        if (!first)
            cout << "\033[" << menuLines << "A";
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
        if (c == '\r' || c == '\n')          { disableRaw(); return selected; }
        if (c == 'q' || c == 'Q' || c == 3) { disableRaw(); return -1; }

        if (c == '\033') {
            unsigned char seq[2] = {0, 0};
            if (read(STDIN_FILENO, &seq[0], 1) <= 0) { disableRaw(); return -1; }
            if (seq[0] != '[') continue;
            if (read(STDIN_FILENO, &seq[1], 1) <= 0) { disableRaw(); return -1; }
            if      (seq[1] == 'A') selected = (selected - 1 + n) % n;
            else if (seq[1] == 'B') selected = (selected + 1) % n;
            draw(false);
        }
    }
}

// ─── helpers ──────────────────────────────────────────────────────────────────

static string toLower(const string& s) {
    string r = s;
    transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

// ─── APT helpers ──────────────────────────────────────────────────────────────

static vector<string> aptFindMatches(const string& query) {
    string q = toLower(query);
    FILE* p = popen("dpkg-query -W -f='${Package} ${Status}\\n' 2>/dev/null", "r");
    if (!p) return {};

    char buf[256];
    vector<string> exact, prefix, contains;

    while (fgets(buf, sizeof(buf), p)) {
        string line = buf;
        size_t sp = line.find(' ');
        if (sp == string::npos) continue;
        string name   = line.substr(0, sp);
        string status = line.substr(sp + 1);
        if (status.find("install ok installed") == string::npos) continue;

        string nl = toLower(name);
        if      (nl == q)                          exact.push_back(name);
        else if (nl.find(q) == 0)                  prefix.push_back(name);
        else if (nl.find(q) != string::npos)       contains.push_back(name);
    }
    pclose(p);

    vector<string> result;
    for (auto& v : {exact, prefix, contains})
        for (auto& n : v)
            result.push_back(n);
    return result;
}

static string aptFindBinary(const string& pkg) {
    string cmd = "dpkg -L " + pkg + " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return pkg;
    char buf[256];
    string best;
    while (fgets(buf, sizeof(buf), p)) {
        string f = buf;
        f.erase(f.find_last_not_of(" \n\r\t") + 1);
        if ((f.find("/usr/bin/") == 0 || f.find("/bin/") == 0) && best.empty())
            best = f;
    }
    pclose(p);
    return best.empty() ? pkg : best;
}

// ─── RPM helpers (zypper + dnf) ───────────────────────────────────────────────

static vector<string> rpmFindMatches(const string& query) {
    string q = toLower(query);

    // rpm -qa zwraca "name-version-release.arch" — wyciągamy tylko nazwę
    FILE* p = popen("rpm -qa --qf '%{NAME}\\n' 2>/dev/null", "r");
    if (!p) return {};

    char buf[256];
    vector<string> exact, prefix, contains;

    while (fgets(buf, sizeof(buf), p)) {
        string name = buf;
        name.erase(name.find_last_not_of(" \n\r\t") + 1);
        if (name.empty()) continue;

        string nl = toLower(name);
        if      (nl == q)                    exact.push_back(name);
        else if (nl.find(q) == 0)            prefix.push_back(name);
        else if (nl.find(q) != string::npos) contains.push_back(name);
    }
    pclose(p);

    vector<string> result;
    for (auto& v : {exact, prefix, contains})
        for (auto& n : v)
            result.push_back(n);
    return result;
}

static string rpmFindBinary(const string& pkg) {
    string cmd = "rpm -ql " + pkg + " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return pkg;
    char buf[256];
    string best;
    while (fgets(buf, sizeof(buf), p)) {
        string f = buf;
        f.erase(f.find_last_not_of(" \n\r\t") + 1);
        if ((f.find("/usr/bin/") == 0 || f.find("/bin/") == 0) && best.empty())
            best = f;
    }
    pclose(p);
    return best.empty() ? pkg : best;
}

// ─── Flatpak / Snap helpers ───────────────────────────────────────────────────

static string flatpakInstalled(const string& pkg) {
    FILE* p = popen("flatpak list --columns=application 2>/dev/null", "r");
    if (!p) return "";
    char buf[256];
    vector<string> ids;
    while (fgets(buf, sizeof(buf), p)) {
        string line = buf;
        line.erase(line.find_last_not_of(" \n\r\t") + 1);
        if (!line.empty()) ids.push_back(line);
    }
    pclose(p);

    string q = toLower(pkg);
    for (const auto& id : ids)
        if (id == pkg) return id;
    for (const auto& id : ids) {
        size_t dot  = id.rfind('.');
        string tail = (dot != string::npos) ? id.substr(dot + 1) : id;
        if (toLower(tail) == q) return id;
    }
    for (const auto& id : ids)
        if (toLower(id).find(q) != string::npos) return id;
    return "";
}

static bool snapInstalled(const string& pkg) {
    return system(("snap list " + pkg + " >/dev/null 2>&1").c_str()) == 0;
}

// ─── launchers ────────────────────────────────────────────────────────────────

static void launchNative(const string& pkg, const string& binary, const string& pmLabel) {
    cout << GREEN << "Launching (" << pmLabel << "): " << RESET << binary
         << YELLOW << "  [" << pkg << "]" << RESET << "\n";
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); close(dn); }
        execlp(binary.c_str(), binary.c_str(), nullptr);
        execlp(pkg.c_str(), pkg.c_str(), nullptr);
        _exit(127);
    }
}

static void launchFlatpak(const string& appId) {
    cout << GREEN << "Launching (Flatpak): " << RESET << appId << "\n";
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); close(dn); }
        execlp("flatpak", "flatpak", "run", appId.c_str(), nullptr);
        _exit(127);
    }
}

static void launchSnap(const string& pkg) {
    cout << GREEN << "Launching (Snap): " << RESET << pkg << "\n";
    string snapBin = "/snap/bin/" + pkg;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); close(dn); }
        execlp(snapBin.c_str(), snapBin.c_str(), nullptr);
        execlp(pkg.c_str(), pkg.c_str(), nullptr);
        _exit(127);
    }
}

// ─── main ─────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    zpm_update::checkForUpdates();

    bool   showHelp    = false;
    bool   showVersion = false;
    string pkg;

    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        if      (arg == "--help"    || arg == "-h") showHelp    = true;
        else if (arg == "--version" || arg == "-v") showVersion = true;
        else if (pkg.empty())                       pkg         = arg;
    }

    auto printVersion = [&]() {
        cout << RED << "zrun component version: v" << zpm_version::version()
             << " of ZPM\n" << RESET;
        cout << "https://github.com/Zielina-Konrad-productions/ZPM\n";
        cout << "Copyright (c) 2026 Ignacyyy & Ry3ball\nLicense: MIT\n";
    };
    auto printHelp = [&]() {
        cout << RED << "Usage: " << RESET << argv[0]
             << " <package> or zpm run <package>\n";
        cout << RED << "Options:\n" << RESET;
        cout << "  Finds the app in native PM / Flatpak / Snap and launches it.\n";
        cout << "  Search supports fuzzy matching (e.g. 'firefox' finds 'firefox-esr').\n";
        cout << "  If installed from multiple sources, lets you pick with ↑↓.\n\n";
        cout << "  --version, -v   Show version information\n";
        cout << "  --help,    -h   Show this help message\n";
    };

    if (showVersion && showHelp) {
        cout << YELLOW << "--version\n" << RESET; printVersion();
        cout << "\n" << YELLOW << "--help\n" << RESET; printHelp();
        return 0;
    }
    if (showVersion) { printVersion(); return 0; }
    if (showHelp)    { printHelp();    return 0; }

    if (pkg.empty()) {
        cerr << RED << "No package specified.\n" << RESET;
        return 1;
    }

    string pm = get_package_manager();
    string pmLabel = (pm == "zypper") ? "Zypper" :
                     (pm == "dnf")    ? "DNF"    : "APT";

    bool hasFlatpak = (system("command -v flatpak >/dev/null 2>&1") == 0);
    bool hasSnap    = (system("command -v snap    >/dev/null 2>&1") == 0);

    cout << YELLOW << "Searching for '" << pkg << "'...\r" << RESET << flush;

    // Szukaj zainstalowanych pakietów
    vector<string> nativeMatches;
    if (pm == "apt")
        nativeMatches = aptFindMatches(pkg);
    else
        nativeMatches = rpmFindMatches(pkg); // zypper i dnf — oba rpm

    string flatpakAppId = hasFlatpak ? flatpakInstalled(pkg) : "";
    bool   snapFound    = hasSnap && snapInstalled(pkg);

    cout << "\033[K";

    // Zbuduj menu
    vector<MenuItem> items;

    for (const auto& npkg : nativeMatches) {
        string bin = (pm == "apt") ? aptFindBinary(npkg) : rpmFindBinary(npkg);
        string lbl = pmLabel + ": " + npkg;
        if (toLower(npkg) != toLower(pkg))
            lbl += "  " + YELLOW + "(matched '" + pkg + "')" + RESET;
        lbl += "  (" + bin + ")";
        items.push_back({lbl, "native", bin, npkg});
    }
    if (!flatpakAppId.empty())
        items.push_back({"Flatpak: " + flatpakAppId, "flatpak", flatpakAppId, flatpakAppId});
    if (snapFound)
        items.push_back({"Snap:    " + pkg, "snap", pkg, pkg});

    if (items.empty()) {
        cerr << RED << "'" << pkg << "' is not installed"
             << " (also tried prefix/substring matching).\n" << RESET;
        return 1;
    }

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

    const MenuItem& sel = items[chosen];
    if      (sel.source == "native")  launchNative(sel.pkg, sel.command, pmLabel);
    else if (sel.source == "flatpak") launchFlatpak(sel.command);
    else if (sel.source == "snap")    launchSnap(sel.pkg);

    return 0;
}
