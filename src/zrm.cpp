
#include "main.h"

using namespace std;

const string LOG_PATH = "/tmp/zrm.log";

volatile sig_atomic_t g_interrupted = 0;
string g_flatpakFlag = "";
string g_pm          = "apt"; // wykrywany w main(), używany w helperach

struct PackageResult {
    string name;
    string message;
    bool   success = false;
};

struct RemoveTarget {
    string name;
    bool   useFlatpak = false;
    bool   useSnap    = false;
    bool   purge      = false; // tylko APT
};

// ─── wiadomość pomocy ─────────────────────────────────────────────────────────
void helpmessage(const char* progName) {
    cout << RED << "Usage: " << RESET << progName << " [options] [packages...]"
         << " or zpm rm/remove [options] [packages...]\n\n";
    cout << RED << "Options:\n" << RESET;
    cout << "  (auto)         Picks native PM / Flatpak / Snap per package\n";
    cout << "  --purge, -p    APT purge instead of remove (APT only)\n";
    cout << "  --version, -v  Show version information\n";
    cout << "  --help,    -h  Show this help message\n";
}

// ─── wiadomość wersji ─────────────────────────────────────────────────────────
void versionmessage() {
    cout << RED << "zrm component version: v" << zpm_version::version() << " of ZPM\n" << RESET;
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

// Czy pakiet jest zainstalowany przez natywny PM?
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

// ─── installed Flatpak list ───────────────────────────────────────────────────
vector<string> getInstalledFlatpaks(const string& query = "") {
    vector<string> results;
    string cmd = "flatpak list " + g_flatpakFlag + " --columns=application 2>/dev/null";
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
    if (!query.empty()) {
        string q = query;
        transform(q.begin(), q.end(), q.begin(), ::tolower);
        vector<string> filtered;
        for (auto& r : results) {
            string l = r; transform(l.begin(), l.end(), l.begin(), ::tolower);
            if (l.find(q) != string::npos) filtered.push_back(r);
        }
        results = filtered;
    }
    sort(results.begin(), results.end());
    return results;
}

// ─── etykieta natywnego PM w menu ─────────────────────────────────────────────
string nativeLabel() {
    if (g_pm == "zypper") return "Zypper: ";
    if (g_pm == "dnf")    return "DNF:    ";
    return "APT:    ";
}

// ─── removal source menu ──────────────────────────────────────────────────────
string chooseRemoveMenu(const string& pkg,
                        bool nativeInstalled,
                        bool snapInstalled,
                        bool flatpakInstalled,
                        bool snapAvail,
                        bool flatpakAvail) {
    struct Option { int num; string key; };
    vector<Option> options;
    int idx = 1;

    cout << "\n" << BOLD << "Package: " << CYAN << pkg << RESET << "\n";

    // 1. Natywny PM
    cout << "  " << BOLD << idx << ". " << nativeLabel() << RESET;
    if (nativeInstalled) {
        cout << GREEN << "installed" << RESET << "\n";
        options.push_back({idx, "native"});
    } else {
        cout << RED << "none" << RESET << "\n";
    }
    idx++;

    // 2. Snap
    if (snapAvail) {
        cout << "  " << BOLD << idx << ". Snap:    " << RESET;
        if (snapInstalled) {
            cout << GREEN << "installed" << RESET << "\n";
            options.push_back({idx, "snap"});
        } else {
            cout << RED << "none" << RESET << "\n";
        }
        idx++;
    }

    // 3. Flatpak
    if (flatpakAvail) {
        cout << "  " << BOLD << idx << ". Flatpak: " << RESET;
        if (flatpakInstalled) {
            cout << GREEN << "installed" << RESET << "\n";
            options.push_back({idx, "flatpak"});
        } else {
            cout << RED << "none" << RESET << "\n";
        }
        idx++;
    }

    cout << "  " << BOLD << "0. Skip" << RESET << "\n";

    if (options.empty()) {
        cout << YELLOW << "Package '" << pkg << "' is not installed anywhere.\n" << RESET;
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

// ─── sub-menu: wybór flatpaków do usunięcia ───────────────────────────────────
vector<string> chooseFlatpakToRemove(const vector<string>& installed, const string& query) {
    if (installed.empty()) {
        cout << YELLOW << "No installed Flatpak packages";
        if (!query.empty()) cout << " matching '" << query << "'";
        cout << ".\n" << RESET;
        return {};
    }
    cout << GREEN << "\nInstalled Flatpak packages";
    if (!query.empty()) cout << " matching '" << query << "'";
    cout << ":\n" << RESET;
    for (size_t i = 0; i < installed.size(); ++i)
        cout << "  " << (i+1) << ". " << installed[i] << "\n";
    cout << "  0. Cancel\n" << BOLD << "Enter number(s) to remove (e.g. 1 3): " << RESET;

    string input;
    if (!getline(cin, input)) return {};
    vector<string> selected;
    size_t pos = 0;
    while (pos < input.size()) {
        while (pos < input.size() && isspace((unsigned char)input[pos])) pos++;
        if (pos >= input.size()) break;
        size_t end = pos;
        while (end < input.size() && !isspace((unsigned char)input[end])) end++;
        string tok = input.substr(pos, end - pos);
        try {
            int n = stoi(tok);
            if (n == 0) return {};
            if (n >= 1 && n <= (int)installed.size()) selected.push_back(installed[n-1]);
        } catch (...) {}
        pos = end;
    }
    return selected;
}

// ─── removers ─────────────────────────────────────────────────────────────────

int removeNative(const string& pkg, bool purge,
                 float startPct, float endPct, int idx, int total) {

    string pmLabel = (g_pm == "zypper") ? "Zypper" :
                     (g_pm == "dnf")    ? "DNF"    : "APT";
    string op = (g_pm == "apt" && purge) ? "purge" : "remove";
    string label = to_string(idx) + "/" + to_string(total)
                   + " | " + pmLabel + " " + op + ": " + pkg;

    progressbar_start(startPct, label + " — removing...");

    int st = 1;
    if (g_pm == "apt") {
        setenv("DEBIAN_FRONTEND", "noninteractive", 1);
        st = system(("apt-get -y " + op + " " + pkg + " >> " + LOG_PATH + " 2>&1").c_str());
    } else if (g_pm == "zypper") {
        st = system(("zypper remove -y " + pkg + " >> " + LOG_PATH + " 2>&1").c_str());
    } else if (g_pm == "dnf") {
        st = system(("dnf remove -y " + pkg + " >> " + LOG_PATH + " 2>&1").c_str());
    }

    if (g_interrupted) return 130;
    progressbar_update(endPct, label + (st == 0 ? " — done" : " — failed"));
    return (st == 0) ? 0 : 1;
}

int removeFlatpak(const string& pkg, float startPct, float endPct, int idx, int total) {
    string label = to_string(idx) + "/" + to_string(total) + " | Flatpak: " + pkg;
    progressbar_start(startPct, label + " — removing...");

    string cmd = "flatpak uninstall " + g_flatpakFlag + " -y --delete-data "
                 + pkg + " >> " + LOG_PATH + " 2>&1";
    system(cmd.c_str());
    if (g_interrupted) return 130;

    bool stillInstalled = isInstalledFlatpak(pkg);
    progressbar_update(endPct, label + (!stillInstalled ? " — done" : " — failed"));
    return stillInstalled ? 1 : 0;
}

int removeSnap(const string& pkg, float startPct, float endPct, int idx, int total) {
    string label = to_string(idx) + "/" + to_string(total) + " | Snap: " + pkg;
    progressbar_start(startPct, label + " — removing...");

    int st = system(("snap remove " + pkg + " >> " + LOG_PATH + " 2>&1").c_str());
    if (g_interrupted) return 130;

    progressbar_update(endPct, label + (st == 0 ? " — done" : " — failed"));
    return (st == 0) ? 0 : 1;
}

// ─── resolve targets ──────────────────────────────────────────────────────────
vector<RemoveTarget> resolveRemoveTargets(const vector<string>& packages,
                                          bool hasSnap, bool hasFlatpak, bool purge) {
    vector<RemoveTarget> targets;

    for (const string& pkg : packages) {
        bool nativeInst = isInstalledNative(pkg);
        bool snapInst   = hasSnap    && isInstalledSnap(pkg);

        bool flatpakInst = false;
        vector<string> flatpakMatches;
        if (hasFlatpak) {
            if (isInstalledFlatpak(pkg)) {
                flatpakInst    = true;
                flatpakMatches = {pkg};
            } else {
                flatpakMatches = getInstalledFlatpaks(pkg);
                flatpakInst    = !flatpakMatches.empty();
            }
        }

        string source = chooseRemoveMenu(pkg, nativeInst, snapInst, flatpakInst,
                                         hasSnap, hasFlatpak);

        if (source == "native") {
            targets.push_back({pkg, false, false, purge});
        } else if (source == "snap") {
            targets.push_back({pkg, false, true, false});
        } else if (source == "flatpak") {
            vector<string> toRemove = (flatpakMatches.size() == 1)
                                      ? flatpakMatches
                                      : chooseFlatpakToRemove(flatpakMatches, pkg);
            for (const auto& fp : toRemove)
                targets.push_back({fp, true, false, false});
        }
    }

    return targets;
}

// ─── remove loop ──────────────────────────────────────────────────────────────
void runRemoveLoop(const vector<RemoveTarget>& targets) {
    vector<PackageResult> results;
    bool anyFailed = false;
    int  totalPkgs = static_cast<int>(targets.size());

    for (int i = 0; i < totalPkgs; ++i) {
        if (g_interrupted) {
            cout << "\n" << YELLOW << "Cancelled by user (Ctrl+C).\n" << RESET;
            return;
        }

        const string& p    = targets[i].name;
        float startPct = (100.0f *  i)      / totalPkgs;
        float endPct   = (100.0f * (i+1))   / totalPkgs;
        PackageResult res; res.name = p;

        auto handleResult = [&](int st, const string& successMsg, const string& failMsg) {
            if (st == 130) {
                progressbar_finish("Cancelled!");
                cout << "\n" << YELLOW << "Cancelled.\n" << RESET;
                return false;
            }
            if (st == 0) {
                res.message = successMsg; res.success = true;
            } else {
                res.message = RED + failMsg + RESET; anyFailed = true;
            }
            return true;
        };

        bool cont = true;
        if (targets[i].useFlatpak) {
            int st = removeFlatpak(p, startPct, endPct, i+1, totalPkgs);
            cont = handleResult(st,
                "Package " + p + " removed successfully.",
                "Package " + p + " removal failed.");
        } else if (targets[i].useSnap) {
            int st = removeSnap(p, startPct, endPct, i+1, totalPkgs);
            cont = handleResult(st,
                "Package " + p + " removed successfully.",
                "Package " + p + " removal failed.");
        } else {
            int st = removeNative(p, targets[i].purge, startPct, endPct, i+1, totalPkgs);
            string op = (g_pm == "apt" && targets[i].purge) ? "purged" : "removed";
            cont = handleResult(st,
                "Package " + p + " " + op + " successfully.",
                "Package " + p + " removal failed.");
        }

        if (!cont) return;
        results.push_back(res);
    }

    if (anyFailed) {
        progressbar_finish("Done with errors!");
        cout << "\n";
        for (const auto& r : results) cout << r.message << "\n";
        cout << RED    << "Removal finished with errors!\n" << RESET;
        cout << YELLOW << "[RAPORT] " << RESET << LOG_PATH << "\n";
        return;
    }

    progressbar_finish("Done!");
    cout << "\n";
    for (const auto& r : results) cout << r.message << "\n";
    cout << GREEN  << "Removal complete!\n" << RESET;
    cout << YELLOW << "[RAPORT] " << RESET << LOG_PATH << "\n";
}

// ─── main ─────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    zpm_update::checkForUpdates();
    signal(SIGINT, handleSigint);
    setvbuf(stdout, nullptr, _IONBF, 0);

    bool showHelp    = false;
    bool showVersion = false;
    bool purge       = false;
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
        if      (arg == "--help"    || arg == "-h") showHelp    = true;
        else if (arg == "--version" || arg == "-v") showVersion = true;
        else if (arg == "--purge"   || arg == "-p") purge       = true;
        else packages.push_back(arg);
    }

    // --purge ma sens tylko na APT
    if (purge && g_pm != "apt") {
        cout << YELLOW << "Warning: --purge is APT-only, ignored on " << g_pm << ".\n" << RESET;
        purge = false;
    }

    if ((purge && showVersion) || (purge && showHelp)) {
        cout << RED << "Error: --purge cannot be combined with --help or --version.\n" << RESET;
        return 1;
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

    if (packages.empty()) {
        cout << YELLOW << "No package specified!\n" << RESET;
        return 1;
    }

    vector<RemoveTarget> targets = resolveRemoveTargets(packages, hasSnap, hasFlatpak, purge);

    if (targets.empty()) {
        cout << YELLOW << "No packages selected.\n" << RESET;
        return 0;
    }

    cout << "\n" << RED << "Auto mode: " << g_pm << " / Flatpak / Snap per package\n" << RESET;
    cout << "Removing packages...\n\n";

    runRemoveLoop(targets);
    return 0;
}
