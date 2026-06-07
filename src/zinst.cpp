
#include "main.h"

using namespace std;

const string LOG_PATH = "/tmp/zinst.log";

volatile sig_atomic_t g_interrupted = 0;
string g_flatpakFlag = "";
string g_pm          = "apt"; // wykrywany w main()

struct PackageResult {
    string name;
    string message;
    bool   success = false;
};

struct InstallTarget {
    string name;
    bool   useFlatpak = false;
    bool   useSnap    = false;
};

// ─── wiadomość pomocy ─────────────────────────────────────────────────────────
void helpmessage(const char* progName) {
    cout << RED << "Usage: " << RESET << progName << " [options] [packages...]"
         << " or zpm inst/install [options] [packages...]\n";
    cout << RED << "Options:\n" << RESET;
    cout << "  (auto)         Picks native PM / Flatpak / Snap per package\n";
    cout << "  --version, -v  Show version information\n";
    cout << "  --help,    -h  Show this help message\n";
}

// ─── wiadomość wersji ─────────────────────────────────────────────────────────
void versionmessage() {
    cout << RED << "zinst component version: v" << zpm_version::version() << " of ZPM\n" << RESET;
    cout << "https://github.com/Zielina-Konrad-productions/ZPM\n";
    cout << "Copyright (c) 2026 Ignacyyy & Ry3ball\nLicense: MIT\n";
}

// ─── signal ───────────────────────────────────────────────────────────────────
void handleSigint(int) { g_interrupted = 1; }

// ─── flatpak remote detection ─────────────────────────────────────────────────
string getFlatpakRemoteFlag() {
    if (system("flatpak remotes --system 2>/dev/null | grep -q flathub") == 0) return "--system";
    if (system("flatpak remotes --user   2>/dev/null | grep -q flathub") == 0) return "--user";
    return "";
}

// ─── detection helpers ────────────────────────────────────────────────────────

bool isInstalledNative(const string& pkg) {
    if (g_pm == "apt") {
        string cmd = "dpkg-query -W -f='${Status}' " + pkg + " 2>/dev/null";
        FILE* p = popen(cmd.c_str(), "r");
        if (!p) return false;
        char buf[128]; string status;
        if (fgets(buf, sizeof(buf), p)) status = buf;
        pclose(p);
        return status.find("install ok installed") != string::npos;
    }
    // zypper i dnf — oba używają rpm
    return system(("rpm -q " + pkg + " >/dev/null 2>&1").c_str()) == 0;
}

bool isInstalledFlatpak(const string& pkg) {
    string cmd = "flatpak list " + g_flatpakFlag
                 + " --columns=application | grep -Fx \"" + pkg + "\" >/dev/null 2>&1";
    return system(cmd.c_str()) == 0;
}

bool isInstalledSnap(const string& pkg) {
    return system(("snap list " + pkg + " >/dev/null 2>&1").c_str()) == 0;
}

// ─── resolveNativeName — aliasy i virtual packages per PM ─────────────────────
//
//  APT:    "firefox" → "firefox-esr" przez apt-cache search
//  Zypper: "firefox" → "MozillaFirefox" przez zypper search -x
//  DNF:    "firefox" → "firefox" (dnf radzi sobie z aliasami sam, zwracamy pkg)
//
string resolveNativeName(const string& pkg) {
    if (g_pm == "apt") {
        // 1. Dosłowna nazwa
        if (system(("apt-cache show " + pkg + " >/dev/null 2>&1").c_str()) == 0) return pkg;
        // 2. Fuzzy: pierwsza nazwa zaczynająca się od pkg
        string cmd = "apt-cache search --names-only '^" + pkg
                     + "' 2>/dev/null | awk '{print $1}' | head -1";
        FILE* p = popen(cmd.c_str(), "r");
        if (!p) return pkg;
        char buf[256] = {};
        fgets(buf, sizeof(buf), p);
        pclose(p);
        string found = buf;
        found.erase(found.find_last_not_of(" \n\r\t") + 1);
        return found.empty() ? pkg : found;
    }

    if (g_pm == "zypper") {
        // 1. Dosłowna nazwa
        if (system(("zypper info " + pkg + " >/dev/null 2>&1").c_str()) == 0) return pkg;
        // 2. Szukaj przez zypper search -x (exact match w nazwie)
        string cmd = "zypper --no-refresh search -x " + pkg
                     + " 2>/dev/null | awk -F'|' 'NR>4 {print $2}' | head -1";
        FILE* p = popen(cmd.c_str(), "r");
        if (!p) return pkg;
        char buf[256] = {};
        fgets(buf, sizeof(buf), p);
        pclose(p);
        string found = buf;
        found.erase(found.find_last_not_of(" \n\r\t") + 1);
        // Usuń leading spaces z zypper output
        size_t start = found.find_first_not_of(' ');
        if (start != string::npos) found = found.substr(start);
        return found.empty() ? pkg : found;
    }

    // DNF: obsługuje provides i aliasy natywnie, zwróć bez zmian
    return pkg;
}

bool nativePackageExists(const string& pkg) {
    string resolved = resolveNativeName(pkg);
    if (g_pm == "apt")
        return system(("apt-cache show " + resolved + " >/dev/null 2>&1").c_str()) == 0;
    if (g_pm == "zypper")
        return system(("zypper --no-refresh info " + resolved + " >/dev/null 2>&1").c_str()) == 0;
    if (g_pm == "dnf")
        return system(("dnf info " + resolved + " >/dev/null 2>&1").c_str()) == 0;
    return false;
}

bool snapPackageExists(const string& pkg) {
    return system(("snap info " + pkg + " >/dev/null 2>&1").c_str()) == 0;
}

// ─── Flatpak search ───────────────────────────────────────────────────────────
vector<string> searchFlatpak(const string& query) {
    vector<string> results;
    string cmd = "flatpak search " + g_flatpakFlag
                 + " --columns=application \"" + query + "\" 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return results;
    char buf[256];
    bool firstLine = true;
    while (fgets(buf, sizeof(buf), p)) {
        string line = buf;
        line.erase(line.find_last_not_of(" \n\r\t") + 1);
        if (firstLine && line == "Application") { firstLine = false; continue; }
        firstLine = false;
        if (!line.empty()) results.push_back(line);
    }
    pclose(p);
    string qLow = query;
    transform(qLow.begin(), qLow.end(), qLow.begin(), ::tolower);
    vector<string> filtered;
    for (const auto& r : results) {
        string l = r; transform(l.begin(), l.end(), l.begin(), ::tolower);
        if (l.find(qLow) != string::npos) filtered.push_back(r);
    }
    sort(filtered.begin(), filtered.end());
    return filtered;
}

// ─── etykieta natywnego PM w menu ─────────────────────────────────────────────
string nativeLabel() {
    if (g_pm == "zypper") return "Zypper: ";
    if (g_pm == "dnf")    return "DNF:    ";
    return "APT:    ";
}

// ─── source-selection menu ────────────────────────────────────────────────────
string chooseSourceMenu(const string& pkg,
                        bool nativeAvail,
                        bool snapAvail,
                        const vector<string>& flatpakResults,
                        bool snapSystemAvail,
                        bool flatpakSystemAvail) {

    bool flatpakAvail = !flatpakResults.empty();
    int  flatpakCount = static_cast<int>(flatpakResults.size());

    struct Option { int num; string key; };
    vector<Option> options;
    int idx = 1;

    cout << "\n" << BOLD << "Package: " << CYAN << pkg << RESET << "\n";

    // 1. Natywny PM
    cout << "  " << BOLD << idx << ". " << nativeLabel() << RESET;
    if (nativeAvail) {
        cout << GREEN << "exist (" << resolveNativeName(pkg) << ")" << RESET << "\n";
        options.push_back({idx, "native"});
    } else {
        cout << RED << "none" << RESET << "\n";
    }
    idx++;

    // 2. Snap
    if (snapSystemAvail) {
        cout << "  " << BOLD << idx << ". Snap:    " << RESET;
        if (snapAvail) {
            cout << GREEN << "exist (" << pkg << ")" << RESET << "\n";
            options.push_back({idx, "snap"});
        } else {
            cout << RED << "none" << RESET << "\n";
        }
        idx++;
    }

    // 3. Flatpak
    if (flatpakSystemAvail) {
        cout << "  " << BOLD << idx << ". Flatpak: " << RESET;
        if (flatpakAvail) {
            cout << GREEN << "exist (" << flatpakCount << " result"
                 << (flatpakCount != 1 ? "s" : "") << ")" << RESET << "\n";
            options.push_back({idx, "flatpak"});
        } else {
            cout << RED << "none" << RESET << "\n";
        }
        idx++;
    }

    cout << "  " << BOLD << "0. Skip" << RESET << "\n";

    if (options.empty()) {
        cout << YELLOW << "No source available for '" << pkg << "'." << RESET << "\n";
        return "";
    }

    while (true) {
        cout << BOLD << "Choose: " << RESET;
        string input;
        if (!getline(cin, input)) return "";
        int choice = -1;
        try { choice = stoi(input); } catch (...) {}
        if (choice == 0) return "";
        for (const auto& o : options)
            if (o.num == choice) return o.key;
        cout << RED << "Invalid choice, try again.\n" << RESET;
    }
}

// ─── sub-menu: pick one Flatpak app-id from list ──────────────────────────────
string chooseFlatpakPackage(const vector<string>& packages, const string& query) {
    if (packages.empty()) {
        cout << YELLOW << "No Flatpak packages found for '" << query << "'.\n" << RESET;
        return "";
    }
    cout << GREEN << "\nFlatpak results for '" << query << "':\n" << RESET;
    for (size_t i = 0; i < packages.size(); ++i)
        cout << "  " << (i+1) << ". " << packages[i] << "\n";
    cout << "  0. Cancel\n" << BOLD << "Choose: " << RESET;

    string input;
    if (!getline(cin, input)) return "";
    int choice = -1;
    try { choice = stoi(input); } catch (...) {}
    if (choice == 0) return "";
    if (choice >= 1 && choice <= (int)packages.size()) return packages[choice-1];
    cout << RED << "Invalid choice!\n" << RESET;
    return "";
}

// ─── installers ───────────────────────────────────────────────────────────────

int installNative(const string& pkg, float startPct, float endPct, int idx, int total) {
    string pmLabel = (g_pm == "zypper") ? "Zypper" :
                     (g_pm == "dnf")    ? "DNF"    : "APT";
    string label = to_string(idx) + "/" + to_string(total) + " | " + pmLabel + ": " + pkg;
    int st = 1;

    if (g_pm == "apt") {
        progressbar_start(startPct, label + " — refreshing cache...");
        system(("apt-get update -qq >> " + LOG_PATH + " 2>&1").c_str());
        if (g_interrupted) return 130;

        progressbar_update(startPct + (endPct - startPct) * 0.3f, label + " — installing...");
        setenv("DEBIAN_FRONTEND", "noninteractive", 1);
        st = system(("apt-get install -y " + pkg + " >> " + LOG_PATH + " 2>&1").c_str());

    } else if (g_pm == "zypper") {
        progressbar_start(startPct, label + " — installing...");
        st = system(("zypper install -y " + pkg + " >> " + LOG_PATH + " 2>&1").c_str());

    } else if (g_pm == "dnf") {
        progressbar_start(startPct, label + " — installing...");
        st = system(("dnf install -y " + pkg + " >> " + LOG_PATH + " 2>&1").c_str());
    }

    if (g_interrupted) return 130;
    progressbar_update(endPct, label + (st == 0 ? " — done" : " — failed"));
    return (st == 0) ? 0 : 1;
}

int installFlatpak(const string& pkg, float startPct, float endPct, int idx, int total) {
    string label = to_string(idx) + "/" + to_string(total) + " | Flatpak: " + pkg;
    progressbar_start(startPct, label + " — installing...");
    string cmd = "flatpak install " + g_flatpakFlag + " -y --noninteractive flathub "
                 + pkg + " >> " + LOG_PATH + " 2>&1";
    int st = system(cmd.c_str());
    if (g_interrupted) return 130;
    progressbar_update(endPct, label + (st == 0 ? " — done" : " — failed"));
    return (st == 0) ? 0 : 1;
}

int installSnap(const string& pkg, float startPct, float endPct, int idx, int total) {
    string label = to_string(idx) + "/" + to_string(total) + " | Snap: " + pkg;
    progressbar_start(startPct, label + " — installing...");
    int st = system(("snap install " + pkg + " >> " + LOG_PATH + " 2>&1").c_str());
    if (g_interrupted) return 130;
    progressbar_update(endPct, label + (st == 0 ? " — done" : " — failed"));
    return (st == 0) ? 0 : 1;
}

// ─── install loop ─────────────────────────────────────────────────────────────
void runInstallLoop(const vector<InstallTarget>& targets) {
    vector<PackageResult> results;
    bool anyFailed = false;
    int  totalPkgs = static_cast<int>(targets.size());

    for (int i = 0; i < totalPkgs; ++i) {
        if (g_interrupted) {
            cout << "\n" << YELLOW << "Cancelled by user (Ctrl+C).\n" << RESET;
            return;
        }

        const string& p    = targets[i].name;
        float startPct = (100.0f *  i)    / totalPkgs;
        float endPct   = (100.0f * (i+1)) / totalPkgs;
        PackageResult res; res.name = p;

        auto finish = [&](int st, const string& alreadyLabel) -> bool {
            if (st == 130) {
                progressbar_finish("Cancelled!");
                cout << "\n" << YELLOW << "Cancelled.\n" << RESET;
                return false;
            }
            if (st == -1) { // already installed
                progressbar_update(endPct, alreadyLabel);
                res.message = YELLOW + "Package " + p + " is already installed." + RESET;
                res.success = true;
            } else if (st == 0) {
                res.message = "Package " + p + " installed successfully.";
                res.success = true;
            } else {
                res.message = RED + "Package " + p + " installation failed." + RESET;
                anyFailed = true;
            }
            return true;
        };

        bool cont = true;

        if (targets[i].useFlatpak) {
            string alreadyLbl = to_string(i+1) + "/" + to_string(totalPkgs)
                                + " | Flatpak: " + p + ": already installed";
            int st = isInstalledFlatpak(p) ? -1
                   : installFlatpak(p, startPct, endPct, i+1, totalPkgs);
            cont = finish(st, alreadyLbl);

        } else if (targets[i].useSnap) {
            string alreadyLbl = to_string(i+1) + "/" + to_string(totalPkgs)
                                + " | Snap: " + p + ": already installed";
            int st = isInstalledSnap(p) ? -1
                   : installSnap(p, startPct, endPct, i+1, totalPkgs);
            cont = finish(st, alreadyLbl);

        } else {
            string pmLabel = (g_pm == "zypper") ? "Zypper" :
                             (g_pm == "dnf")    ? "DNF"    : "APT";
            string alreadyLbl = to_string(i+1) + "/" + to_string(totalPkgs)
                                + " | " + pmLabel + ": " + p + ": already installed";
            int st = isInstalledNative(p) ? -1
                   : installNative(p, startPct, endPct, i+1, totalPkgs);
            cont = finish(st, alreadyLbl);
        }

        if (!cont) return;
        results.push_back(res);
    }

    if (anyFailed) {
        progressbar_finish("Done with errors!");
        cout << "\n";
        for (const auto& r : results) cout << r.message << "\n";
        cout << RED    << "Installation finished with errors!\n" << RESET;
        cout << YELLOW << "[RAPORT] " << RESET << LOG_PATH << "\n";
        return;
    }

    progressbar_finish("Done!");
    cout << "\n";
    for (const auto& r : results) cout << r.message << "\n";
    cout << GREEN  << "Installation complete!\n" << RESET;
    cout << YELLOW << "[RAPORT] " << RESET << LOG_PATH << "\n";
}

// ─── resolve packages to targets ──────────────────────────────────────────────
vector<InstallTarget> resolveTargets(const vector<string>& packages,
                                     bool hasSnap, bool hasFlatpak) {
    vector<InstallTarget> targets;

    for (const string& pkg : packages) {
        bool           nativeAvail  = nativePackageExists(pkg);
        bool           snapAvail    = hasSnap    && snapPackageExists(pkg);
        vector<string> flatpakFound = hasFlatpak ? searchFlatpak(pkg) : vector<string>{};

        string source = chooseSourceMenu(pkg, nativeAvail, snapAvail,
                                         flatpakFound, hasSnap, hasFlatpak);

        if (source == "native") {
            targets.push_back({resolveNativeName(pkg), false, false});
        } else if (source == "snap") {
            targets.push_back({pkg, false, true});
        } else if (source == "flatpak") {
            bool exactMatch = false;
            for (const auto& c : flatpakFound)
                if (c == pkg) { exactMatch = true; break; }
            string selected = exactMatch ? pkg : chooseFlatpakPackage(flatpakFound, pkg);
            if (!selected.empty()) targets.push_back({selected, true, false});
        }
    }

    return targets;
}

// ─── main ─────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    zpm_update::checkForUpdates();
    signal(SIGINT, handleSigint);
    setvbuf(stdout, nullptr, _IONBF, 0);

    bool           showHelp       = false;
    bool           showVersion    = false;
    bool           useTestPackage = false;
    vector<string> packages;

    g_pm = get_package_manager();

    if (g_pm == "unknown") {
        cout << RED << "Error: Could not detect a supported package manager "
             << "(apt / zypper / dnf).\n" << RESET;
        return 1;
    }

    bool hasFlatpak = (system("command -v flatpak >/dev/null 2>&1") == 0);
    bool hasSnap    = (system("command -v snap    >/dev/null 2>&1") == 0);

    if (hasFlatpak) {
        g_flatpakFlag = getFlatpakRemoteFlag();
        if (g_flatpakFlag.empty()) {
            cout << YELLOW << "Warning: No flathub remote found for flatpak "
                 << "(tried --system and --user).\n" << RESET;
            hasFlatpak = false;
        }
    }

    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        if      (arg == "--help"    || arg == "-h") showHelp       = true;
        else if (arg == "--version" || arg == "-v") showVersion    = true;
        else if (arg == "--dry-run")                useTestPackage = true;
        else packages.push_back(arg);
    }

    if (showVersion && showHelp) {
        cout << YELLOW << "--version" << RESET << "\n"; versionmessage();
        cout << "\n" << YELLOW << "--help" << RESET << "\n"; helpmessage(argv[0]);
        return 0;
    }
    if (showVersion) { versionmessage();     return 0; }
    if (showHelp)    { helpmessage(argv[0]); return 0; }

    if (geteuid() != 0) {
        cout << RED << "Run with sudo!\n" << RESET;
        return 1;
    }

    // --dry-run: użyj "sl" jako pakietu testowego
    if (useTestPackage) packages.push_back("sl");

    if (packages.empty()) {
        cout << YELLOW << "No package specified!\n" << RESET;
        return 1;
    }

    vector<InstallTarget> targets = resolveTargets(packages, hasSnap, hasFlatpak);

    if (targets.empty()) {
        cout << YELLOW << "No packages selected.\n" << RESET;
        return 0;
    }

    cout << "\n" << RED << "Auto mode: " << g_pm << " / Flatpak / Snap per package\n" << RESET;
    cout << "Installing packages...\n\n";

    runInstallLoop(targets);
    return 0;
}
