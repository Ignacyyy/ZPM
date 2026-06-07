#include "main.h"
#include <sstream> // Potrzebne do obsługi istringstream przy parsowaniu DNF

using namespace std;

// Pomocnicza funkcja do zamiany całego stringa na małe litery
string toLower(string s) {
    for (char &c : s) {
        c = tolower((unsigned char)c);
    }
    return s;
}

// ─── komunikaty ───────────────────────────────────────────────────────────────
void helpmessage(const char* progName) {
    cout << RED << "Usage: " << RESET << progName
    << " [options]  or  zpm list [options]\n\n";
    cout << RED << "Options:\n" << RESET;
    cout << "  --version,  -v  Show version information\n";
    cout << "  --help,     -h  Show this help message\n";
    cout << "  --native,   -n  List only native PM packages (apt/zypper/dnf)\n";
    cout << "  --flatpak,  -f  List only Flatpak packages\n";
    cout << "  --snap,     -s  List only Snap packages\n";
}

void versionmessage() {
    cout << RED << "zlist component version: v" << zpm_version::version() << " of ZPM\n" << RESET;
    cout << "https://github.com/Zielina-Konrad-productions/ZPM\n";
    cout << "Copyright (c) 2026 Ignacyyy & Ry3ball\nLicense: MIT\n";
}

// ─── sekcje listowania ────────────────────────────────────────────────────────

void listNative(const string& pm) {
    if (pm == "apt") {
        cout << YELLOW << "=== APT packages ===\n" << RESET;

        // Odpalamy apt list przez popen, żeby sparsować wyjście
        FILE* p = popen("apt list --installed 2>/dev/null", "r");
        if (!p) return;
        char buf[512];

        while (fgets(buf, sizeof(buf), p)) {
            string line(buf);
            line.erase(line.find_last_not_of(" \n\r\t") + 1);
            if (line.empty()) continue;

            // Odsiewamy nagłówek "Listing... Done" / "Listowanie... Gotowe"
            if (line.find("Listing...") != string::npos || line.find("Listowanie...") != string::npos) {
                continue;
            }

            istringstream iss(line);
            string pkg, ver;

            // Pierwsza kolumna to pakiet/repozytorium (np. tmux/noble-updates,now)
            if (!(iss >> pkg)) continue;
            // Druga kolumna to wersja (np. 3.4-1ubuntu0.1)
            if (!(iss >> ver)) continue;

            // Wytnij z nazwy pakietu wszystko od slasha '/' (informacje o repozytorium/stanie)
            size_t slash = pkg.find('/');
            string name = (slash != string::npos) ? pkg.substr(0, slash) : pkg;

            // Wypisujemy w identycznym formacie jak DNF: nazwa (wersja)
            cout << name << " (" << ver << ")\n";
        }
        pclose(p);

    } else if (pm == "zypper") {
        cout << YELLOW << "=== Zypper packages ===\n" << RESET;
        // zypper packages -i: tabela z | — wypisujemy Name + Version
        FILE* p = popen("zypper --no-refresh packages -i 2>/dev/null", "r");
        if (!p) return;
        char buf[512];
        bool pastHeader = false;
        while (fgets(buf, sizeof(buf), p)) {
            string line(buf);
            line.erase(line.find_last_not_of(" \n\r\t") + 1);
            if (line.find("-+-") != string::npos) { pastHeader = true; continue; }
            if (!pastHeader || line.empty()) continue;
            // Kolumny: S | Repository | Name | Version | Arch
            auto splitPipe = [](const string& s) {
                vector<string> parts;
                size_t start = 0, pos;
                while ((pos = s.find('|', start)) != string::npos) {
                    string part = s.substr(start, pos - start);
                    part.erase(0, part.find_first_not_of(" \t"));
                    part.erase(part.find_last_not_of(" \t") + 1);
                    parts.push_back(part);
                    start = pos + 1;
                }
                string last = s.substr(start);
                last.erase(0, last.find_first_not_of(" \t"));
                last.erase(last.find_last_not_of(" \t") + 1);
                parts.push_back(last);
                return parts;
            };
            vector<string> cols = splitPipe(line);
            if (cols.size() < 4) continue;
            string name    = cols[2];
            string version = cols[3];
            if (name.empty()) continue;
            cout << name << " (" << version << ")\n";
        }
        pclose(p);

    } else if (pm == "dnf") {
        cout << YELLOW << "=== DNF packages ===\n" << RESET;
        FILE* p = popen("dnf list --installed 2>/dev/null", "r");
        if (!p) return;
        char buf[512];

        while (fgets(buf, sizeof(buf), p)) {
            string line(buf);
            line.erase(line.find_last_not_of(" \n\r\t") + 1);
            if (line.empty()) continue;

            if (line.find("====") != string::npos) continue;

            istringstream iss(line);
            string pkg, ver;
            if (!(iss >> pkg)) continue;

            string pkgLower = toLower(pkg);
            if (pkgLower == "installed" || pkgLower == "zainstalowane" ||
                pkgLower == "aktualizowanie" || pkgLower == "załadowano" ||
                pkgLower == "updating" || pkgLower == "repositories" ||
                pkgLower == "loading" || pkgLower == "repozytoria" ||
                pkgLower == "package" || pkgLower == "pakiet" ||
                pkgLower == "name" || pkgLower == "nazwa" ||
                pkgLower == "wersja" || pkgLower == "version" ||
                pkgLower == "dopasowane" || pkgLower == "pola:") {
                continue;
                }

                if (iss >> ver) {
                    string name = pkg;
                    size_t dot = pkg.rfind('.');
                    if (dot != string::npos && dot > 0) name = pkg.substr(0, dot);

                    cout << name << " (" << ver << ")\n";
                }
        }
        pclose(p);
    }
}

void listFlatpak() {
    bool hasFlatpak = (system("command -v flatpak >/dev/null 2>&1") == 0);

    if (!hasFlatpak) {
        cerr << "" << endl;
        return;
    }
    cout << "\n" << YELLOW << "=== Flatpak packages ===\n" << RESET;
    FILE* p = popen("flatpak list --columns=application,name,version 2>/dev/null", "r");
    if (!p) return;
    char buf[512];
    while (fgets(buf, sizeof(buf), p)) {
        string line(buf);
        line.erase(line.find_last_not_of(" \n\r\t") + 1);
        if (!line.empty()) cout << line << "\n";
    }
    pclose(p);
}

void listSnap() {
    bool hasSnap = (system("command -v snap >/dev/null 2>&1") == 0);

    if (!hasSnap) {
        cerr << "" << endl;
        return;
    }
    cout << "\n" << YELLOW << "=== Snap packages ===\n" << RESET;
    system("snap list 2>/dev/null");
}

// ─── main ─────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    zpm_update::checkForUpdates();

    bool showHelp    = false;
    bool showVersion = false;
    bool onlyNative  = false;
    bool onlyFlatpak = false;
    bool onlySnap    = false;

    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        if      (arg == "--help"    || arg == "-h") showHelp    = true;
        else if (arg == "--version" || arg == "-v") showVersion = true;
        else if (arg == "--native"  || arg == "-n") onlyNative  = true;
        else if (arg == "--flatpak" || arg == "-f") onlyFlatpak = true;
        else if (arg == "--snap"    || arg == "-s") onlySnap    = true;
    }

    if (showVersion && showHelp) {
        cout << YELLOW << "--version\n" << RESET; versionmessage();
        cout << "\n" << YELLOW << "--help\n" << RESET; helpmessage(argv[0]);
        return 0;
    }
    if (showVersion) { versionmessage();     return 0; }
    if (showHelp)    { helpmessage(argv[0]); return 0; }

    string pm = get_package_manager();

    if (pm == "unknown") {
        cerr << RED << "Error: Could not detect a supported package manager "
        << "(apt / zypper / dnf).\n" << RESET;
        return 1;
    }

    // Tryb filtrowany
    if (onlyNative)  { listNative(pm);  return 0; }
    if (onlyFlatpak) { listFlatpak();   return 0; }
    if (onlySnap)    { listSnap();      return 0; }

    // Tryb domyślny — wszystko
    listNative(pm);
    listFlatpak();
    listSnap();

    return 0;
}
