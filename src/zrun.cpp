
// zrun.cpp — part of ZPM
// Launch an installed application from APT, Flatpak, or Snap.
// If found in multiple sources, presents an arrow-key menu to choose.

#include "main.h"
#include <termios.h>

using namespace std;

// ─── ANSI arrow-key menu ──────────────────────────────────────────────────────

struct MenuItem {
    string label;   // e.g. "APT:     firefox-esr"
    string source;  // "apt" | "flatpak" | "snap"
    string command; // exact command to exec
    string aptPkg;  // rzeczywista nazwa pakietu APT (może różnić się od query)
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

        if (c == '\r' || c == '\n') { disableRaw(); return selected; }

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

// ─── APT helpers ──────────────────────────────────────────────────────────────

static string toLower(const string& s) {
    string r = s;
    transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

// Sprawdza czy konkretna nazwa pakietu jest zainstalowana
static bool aptPkgInstalled(const string& pkg) {
    string cmd = "dpkg-query -W -f='${Status}' " + pkg + " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return false;
    char buf[128]; string status;
    if (fgets(buf, sizeof(buf), p)) status = buf;
    pclose(p);
    return status.find("install ok installed") != string::npos;
}

// Szuka zainstalowanych pakietów APT pasujących do zapytania.
// Strategia (w kolejności priorytetu):
//   1. Dokładne dopasowanie nazwy
//   2. Nazwa zaczyna się od query (np. "firefox" → "firefox-esr")
//   3. Nazwa zawiera query jako człon (np. "firefox" → "firefox-esr", "firefox-locale-pl")
//   4. Query jest sufiksem członu (np. "esr" → "firefox-esr") — tylko jeśli pusto
// Zwraca listę pasujących nazw pakietów (bez duplikatów).
static vector<string> aptFindMatches(const string& query) {
    string q = toLower(query);

    // Pobierz wszystkie zainstalowane pakiety przez dpkg-query
    FILE* p = popen("dpkg-query -W -f='${Package} ${Status}\\n' 2>/dev/null", "r");
    if (!p) return {};

    char buf[256];
    vector<string> exact, prefix, contains;

    while (fgets(buf, sizeof(buf), p)) {
        string line = buf;
        // format: "pakiet install ok installed"
        size_t sp = line.find(' ');
        if (sp == string::npos) continue;
        string name   = line.substr(0, sp);
        string status = line.substr(sp + 1);
        if (status.find("install ok installed") == string::npos) continue;

        string nl = toLower(name);

        if (nl == q)
            exact.push_back(name);
        else if (nl.find(q) == 0)          // prefix: "firefox-esr", "firefox-locale-*"
            prefix.push_back(name);
        else if (nl.find(q) != string::npos)
            contains.push_back(name);
    }
    pclose(p);

    // Sklej wyniki: exact najpierw, potem prefix, potem contains
    vector<string> result;
    for (auto& v : {exact, prefix, contains})
        for (auto& n : v)
            result.push_back(n);

    return result;
}

// Znajdź binarny plik pakietu APT (pierwszy w /usr/bin/ lub /bin/)
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

// ─── Flatpak / Snap helpers ───────────────────────────────────────────────────

static bool snapInstalled(const string& pkg) {
    return system(("snap list " + pkg + " >/dev/null 2>&1").c_str()) == 0;
}

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

    for (const auto& id : ids)          // 1. exact app-id
        if (id == pkg) return id;

    for (const auto& id : ids) {        // 2. ostatni człon app-id
        size_t dot  = id.rfind('.');
        string tail = (dot != string::npos) ? id.substr(dot + 1) : id;
        if (toLower(tail) == q) return id;
    }

    for (const auto& id : ids)          // 3. substring
        if (toLower(id).find(q) != string::npos) return id;

    return "";
}

// ─── launchers ────────────────────────────────────────────────────────────────

static void launchAPT(const string& pkg, const string& binary) {
    cout << GREEN << "Launching (APT): " << RESET << binary
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

    // ── --version / --help ────────────────────────────────────────────────────
    auto printVersion = [&]() {
        cout << RED << "zrun component version: v" << zpm_version::version()
             << " of ZPM\n" << RESET;
        cout << "https://github.com/Zielina-Konrad-productions/ZPM\n";
        cout << "Copyright (c) 2026 Ignacyyy & Ry3ball\nLicense: MIT\n";
    };
    auto printHelp = [&]() {
        cout << RED << "Usage: " << RESET << argv[0]
             << " <package>  or  zpm run <package>\n\n";
        cout << RED << "Options:\n" << RESET;
        cout << "  Finds the app in APT / Flatpak / Snap and launches it.\n";
        cout << "  APT search supports fuzzy matching (e.g. 'firefox' finds 'firefox-esr').\n";
        cout << "  If installed from multiple sources, lets you pick with ↑↓.\n\n";
        cout << "  --version, -v   Show version information\n";
        cout << "  --help,    -h   Show this help message\n";
    };

    if (showVersion && showHelp) {
        cout << YELLOW << "--version\n" << RESET; printVersion();
        cout << "\n" << YELLOW << "--help\n"    << RESET; printHelp();
        return 0;
    }
    if (showVersion) { printVersion(); return 0; }
    if (showHelp)    { printHelp();    return 0; }

    if (pkg.empty()) {
        cout << RED << "No package specified.\n" << RESET;
        return 1;
    }

    // ── sprawdź dostępność menedżerów ─────────────────────────────────────────
    bool hasFlatpak = (system("command -v flatpak >/dev/null 2>&1") == 0);
    bool hasSnap    = (system("command -v snap    >/dev/null 2>&1") == 0);

    // ── szukaj ────────────────────────────────────────────────────────────────
    cout << YELLOW << "Searching for '" << pkg << "'...\r" << RESET << flush;

    vector<string> aptMatches    = aptFindMatches(pkg);
    string         flatpakAppId  = hasFlatpak ? flatpakInstalled(pkg) : "";
    bool           snapFound     = hasSnap && snapInstalled(pkg);

    cout << "\033[K"; // wyczyść linię "Searching..."

    // ── zbuduj menu ───────────────────────────────────────────────────────────
    vector<MenuItem> items;

    for (const auto& aptPkg : aptMatches) {
        string bin = aptFindBinary(aptPkg);
        // Pokaż użytkownikowi oryginalny query → znaleziony pakiet
        string lbl = "APT:     " + aptPkg;
        if (aptPkg != pkg)
            lbl += "  " + YELLOW + "(matched '" + pkg + "')" + RESET;
        lbl += "  (" + bin + ")";
        items.push_back({lbl, "apt", bin, aptPkg});
    }
    if (!flatpakAppId.empty()) {
        items.push_back({"Flatpak: " + flatpakAppId, "flatpak", flatpakAppId, ""});
    }
    if (snapFound) {
        items.push_back({"Snap:    " + pkg, "snap", pkg, ""});
    }

    if (items.empty()) {
        cout << RED << "'" << pkg << "' is not installed"
             << " (also tried prefix/substring matching for APT).\n" << RESET;
        return 1;
    }

    // ── jeśli jeden wynik — uruchom bez menu ─────────────────────────────────
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

    // ── uruchom ───────────────────────────────────────────────────────────────
    const MenuItem& sel = items[chosen];
    if      (sel.source == "apt")     launchAPT(sel.aptPkg, sel.command);
    else if (sel.source == "flatpak") launchFlatpak(sel.command);
    else if (sel.source == "snap")    launchSnap(pkg);

    return 0;
}
