#include "main.h"

using namespace std;

const string LOG_PATH = "/tmp/zinst.log";

volatile sig_atomic_t g_interrupted = 0;
string g_flatpakFlag = ""; // --system lub --user, wykrywane w main()

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

//wiadomosc pomocy
void helpmessage(const char* progName){
    cout << RED << "Usage: " << RESET << progName << " [options] [packages...]" << " or zpm inst/install [options] [packages...]"  "\n\n";
    cout << RED << "Options:\n" << RESET;
    cout << "  (auto)         Picks APT / Flatpak / Snap per package\n";
    cout << "  --version, -v  Show version information\n";
    cout << "  --help,    -h  Show this help message\n";
}

//wiadomosc versji
void versionmessage(){
    cout << RED << "zinst component version: v" << zpm_version::version() << " of ZPM\n" << RESET;
    cout << "https://github.com/Zielina-Konrad-productions/ZPM\n";
    cout << "Copyright (c) 2026 Ignacyyy & Ry3ball \nLicense: MIT\n";
}

// ─── signal ──────────────────────────────────────────────────────────────────
void handleSigint(int) { g_interrupted = 1; }

// ─── flatpak remote detection ─────────────────────────────────────────────────
string getFlatpakRemoteFlag() {
    if (system("flatpak remotes --system 2>/dev/null | grep -q flathub") == 0)
        return "--system";
    if (system("flatpak remotes --user 2>/dev/null | grep -q flathub") == 0)
        return "--user";
    return "";
}

// ─── detection helpers ────────────────────────────────────────────────────────
bool isInstalledAPT(const string& pkg) {
    string cmd = "dpkg-query -W -f='${Status}' " + pkg + " 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return false;
    char buffer[128]; string status;
    if (fgets(buffer, sizeof(buffer), pipe)) status = buffer;
    pclose(pipe);
    return status.find("install ok installed") != string::npos;
}

bool isInstalledFlatpak(const string& pkg) {
    string cmd = "flatpak list " + g_flatpakFlag + " --columns=application | grep -Fx \"" + pkg + "\" >/dev/null 2>&1";
    return system(cmd.c_str()) == 0;
}

bool isInstalledSnap(const string& pkg) {
    string cmd = "snap list " + pkg + " >/dev/null 2>&1";
    return system(cmd.c_str()) == 0;
}

// Zwraca rzeczywistą nazwę pakietu w APT (obsługuje aliasy i virtual packages).
// Np. "firefox" -> "firefox-esr" na Debianie.
// Jeśli podana nazwa istnieje dosłownie — zwraca ją bez zmian.
// Jeśli nie — szuka przez apt-cache search i bierze pierwsze trafienie,
// którego nazwa zaczyna się od zapytania (np. firefox -> firefox-esr).
string resolveAptName(const string& pkg) {
    // 1. Sprawdź czy nazwa dosłowna istnieje
    string checkExact = "apt-cache show " + pkg + " >/dev/null 2>&1";
    if (system(checkExact.c_str()) == 0) return pkg;

    // 2. Szukaj przez apt-cache search — bierz pierwszą nazwę zaczynającą się od pkg
    string cmd = "apt-cache search --names-only '^" + pkg + "' 2>/dev/null | awk '{print $1}' | head -1";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return pkg;
    char buf[256] = {};
    fgets(buf, sizeof(buf), pipe);
    pclose(pipe);

    string found = buf;
    found.erase(found.find_last_not_of(" \n\r\t") + 1);
    return found.empty() ? pkg : found;
}

bool aptPackageExists(const string& pkg) {
    string resolved = resolveAptName(pkg);
    return !resolved.empty()
        && system(("apt-cache show " + resolved + " >/dev/null 2>&1").c_str()) == 0;
}

bool snapPackageExists(const string& pkg) {
    return system(("snap info " + pkg + " >/dev/null 2>&1").c_str()) == 0;
}

// Returns all Flatpak application IDs matching query (case-insensitive substring)
vector<string> searchFlatpak(const string& query) {
    vector<string> results;
    string cmd = "flatpak search " + g_flatpakFlag + " --columns=application \"" + query + "\" 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return results;

    char buffer[256];
    bool firstLine = true;
    while (fgets(buffer, sizeof(buffer), pipe)) {
        string line = buffer;
        line.erase(line.find_last_not_of(" \n\r\t") + 1);
        if (firstLine && line == "Application") { firstLine = false; continue; }
        firstLine = false;
        if (!line.empty()) results.push_back(line);
    }
    pclose(pipe);

    string qLower = query;
    transform(qLower.begin(), qLower.end(), qLower.begin(), ::tolower);
    vector<string> filtered;
    for (const auto& pkg : results) {
        string p = pkg;
        transform(p.begin(), p.end(), p.begin(), ::tolower);
        if (p.find(qLower) != string::npos) filtered.push_back(pkg);
    }
    sort(filtered.begin(), filtered.end());
    return filtered;
}

// ─── source-selection menu ────────────────────────────────────────────────────
string chooseSourceMenu(const string& pkg,
                        bool aptAvail,
                        bool snapAvail,
                        const vector<string>& flatpakResults,
                        bool snapSystemAvail,
                        bool flatpakSystemAvail) {

    bool flatpakAvail = !flatpakResults.empty();
    int  flatpakCount = static_cast<int>(flatpakResults.size());

    struct Option { int num; string key; };
    vector<Option> options;
    int idx = 1;

    int aptNum     = -1;
    int snapNum    = -1;
    int flatpakNum = -1;

    cout << "\n" << BOLD << "Package: " << CYAN << pkg << RESET << "\n";

    cout << "  " << BOLD << idx << ". APT:     " << RESET;
    if (aptAvail) {
        cout << GREEN << "exist (" << pkg << ")" << RESET << "\n";
        aptNum = idx++;
        options.push_back({aptNum, "apt"});
    } else {
        cout << RED << "none" << RESET << "\n";
        idx++;
    }

    if (snapSystemAvail) {
        cout << "  " << BOLD << idx << ". Snap:    " << RESET;
        if (snapAvail) {
            cout << GREEN << "exist (" << pkg << ")" << RESET << "\n";
            snapNum = idx;
            options.push_back({snapNum, "snap"});
        } else {
            cout << RED << "none" << RESET << "\n";
        }
        idx++;
    }

    if (flatpakSystemAvail) {
        cout << "  " << BOLD << idx << ". Flatpak: " << RESET;
        if (flatpakAvail) {
            cout << GREEN << "exist (" << flatpakCount << " result"
                 << (flatpakCount != 1 ? "s" : "") << ")" << RESET << "\n";
            flatpakNum = idx;
            options.push_back({flatpakNum, "flatpak"});
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
        cout << "  " << (i + 1) << ". " << packages[i] << "\n";
    cout << "  0. Cancel\n" << BOLD << "Choose: " << RESET;

    string input;
    if (!getline(cin, input)) return "";
    int choice = -1;
    try { choice = stoi(input); } catch (...) {}
    if (choice == 0) return "";
    if (choice >= 1 && choice <= static_cast<int>(packages.size()))
        return packages[choice - 1];

    cout << RED << "Invalid choice!\n" << RESET;
    return "";
}

// ─── installers ───────────────────────────────────────────────────────────────

int installAPT(const string& pkg, float startPct, float endPct, int idx, int total) {
    string label = to_string(idx) + "/" + to_string(total) + " | APT: " + pkg;

    progressbar_start(startPct, label + " — refreshing cache...");
    int st = system(("apt-get update -qq >> " + LOG_PATH + " 2>&1").c_str());
    if (g_interrupted) return 130;

    progressbar_update(startPct + (endPct - startPct) * 0.3f,
                       label + " — installing...");
    setenv("DEBIAN_FRONTEND", "noninteractive", 1);
    st = system(("apt-get install -y -o APT::Status-Fd=/dev/null "
                 + pkg + " >> " + LOG_PATH + " 2>&1").c_str());
    if (g_interrupted) return 130;

    progressbar_update(endPct, label + " — done");
    return (st == 0) ? 0 : 1;
}

int installFlatpak(const string& pkg, float startPct, float endPct, int idx, int total) {
    string label = to_string(idx) + "/" + to_string(total) + " | Flatpak: " + pkg;

    progressbar_start(startPct, label + " — installing...");
    string cmd = "flatpak install " + g_flatpakFlag + " -y --noninteractive flathub "
                 + pkg + " >> " + LOG_PATH + " 2>&1";
    int st = system(cmd.c_str());
    if (g_interrupted) return 130;

    progressbar_update(endPct, label + " — done");
    return (st == 0) ? 0 : 1;
}

int installSnap(const string& pkg, float startPct, float endPct, int idx, int total) {
    string label = to_string(idx) + "/" + to_string(total) + " | Snap: " + pkg;

    progressbar_start(startPct, label + " — installing...");
    string cmd = "snap install " + pkg + " >> " + LOG_PATH + " 2>&1";
    int st = system(cmd.c_str());
    if (g_interrupted) return 130;

    progressbar_update(endPct, label + " — done");
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

        const string& p      = targets[i].name;
        float startPct = (100.0f *  i)      / totalPkgs;
        float endPct   = (100.0f * (i + 1)) / totalPkgs;

        PackageResult res; res.name = p;

        // ── Flatpak ───────────────────────────────────────────────────────────
        if (targets[i].useFlatpak) {
            string label = to_string(i+1) + "/" + to_string(totalPkgs) + " | Flatpak: " + p;
            if (isInstalledFlatpak(p)) {
                progressbar_update(endPct, label + ": already installed");
                res.message = YELLOW + "Package " + p + " is already installed." + RESET;
                res.success = true;
            } else {
                int st = installFlatpak(p, startPct, endPct, i+1, totalPkgs);
                if (st == 130) { progressbar_finish("Cancelled!"); cout << "\n" << YELLOW << "Cancelled.\n" << RESET; return; }
                if (st == 0) {
                    res.message = "Package " + p + " installed successfully." + RESET;
                    res.success = true;
                } else {
                    res.message = RED + "Package " + p + " installation failed." + RESET;
                    anyFailed   = true;
                }
            }
        }
        // ── Snap ──────────────────────────────────────────────────────────────
        else if (targets[i].useSnap) {
            string label = to_string(i+1) + "/" + to_string(totalPkgs) + " | Snap: " + p;
            if (isInstalledSnap(p)) {
                progressbar_update(endPct, label + ": already installed");
                res.message = YELLOW + "Package " + p + " is already installed." + RESET;
                res.success = true;
            } else {
                int st = installSnap(p, startPct, endPct, i+1, totalPkgs);
                if (st == 130) { progressbar_finish("Cancelled!"); cout << "\n" << YELLOW << "Cancelled.\n" << RESET; return; }
                if (st == 0) {
                    res.message = "Package " + p + " installed successfully." + RESET;
                    res.success = true;
                } else {
                    res.message = RED + "Package " + p + " installation failed." + RESET;
                    anyFailed   = true;
                }
            }
        }
        // ── APT ───────────────────────────────────────────────────────────────
        else {
            string label = to_string(i+1) + "/" + to_string(totalPkgs) + " | APT: " + p;
            if (isInstalledAPT(p)) {
                progressbar_update(endPct, label + ": already installed");
                res.message = YELLOW + "Package " + p + " is already installed." + RESET;
                res.success = true;
            } else {
                int st = installAPT(p, startPct, endPct, i+1, totalPkgs);
                if (st == 130) { progressbar_finish("Cancelled!"); cout << "\n" << YELLOW << "Cancelled.\n" << RESET; return; }
                if (st == 0) {
                    res.message = "Package " + p + " installed successfully." + RESET;
                    res.success = true;
                } else {
                    res.message = RED + "Package " + p + " installation failed." + RESET;
                    anyFailed   = true;
                }
            }
        }

        results.push_back(res);
    }

    // ── finalizacja ───────────────────────────────────────────────────────────
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
        bool           aptAvail     = aptPackageExists(pkg);
        bool           snapAvail    = hasSnap    && snapPackageExists(pkg);
        vector<string> flatpakFound = hasFlatpak ? searchFlatpak(pkg) : vector<string>{};

        string source = chooseSourceMenu(pkg, aptAvail, snapAvail,
                                         flatpakFound, hasSnap, hasFlatpak);

        if (source == "apt") {
            targets.push_back({resolveAptName(pkg), false, false});
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
    bool           y              = false;
    vector<string> packages;

    bool hasFlatpak = (system("command -v flatpak >/dev/null 2>&1") == 0);
    bool hasSnap    = (system("command -v snap    >/dev/null 2>&1") == 0);

    if (hasFlatpak) {
        g_flatpakFlag = getFlatpakRemoteFlag();
        if (g_flatpakFlag.empty()) {
            cout << YELLOW << "Warning: No flathub remote found for flatpak (tried --system and --user).\n" << RESET;
            hasFlatpak = false;
        }
    }

    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        if      (arg == "--help"    || arg == "-h") showHelp    = true;
        else if (arg == "--version" || arg == "-v") showVersion = true;
        else if (arg == "--dry-run")                useTestPackage = true;
        else if (arg == "--yes"     || arg == "-y") y           = true;
        else packages.push_back(arg);
    }

    if (showVersion && showHelp) {
        cout << YELLOW << "--version" << RESET << endl;
        versionmessage();
        cout << "" << endl;
        cout << YELLOW <<"--help" << RESET << endl;
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
        cout << RED << "Run with sudo!\n" << RESET;
        return 1;
    }

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

    cout << "\n" << RED << "Auto mode: APT/Flatpak/Snap per package\n" << RESET;
    cout << "Installing packages...\n\n";

    runInstallLoop(targets);
    return 0;
}