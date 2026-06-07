#include "main.h"
using namespace std;

// ─── helpers ──────────────────────────────────────────────────────────────────
string toLower(string str) {
    transform(str.begin(), str.end(), str.begin(), ::tolower);
    return str;
}

string highlight(const string& text, const string& query) {
    string lowerText  = toLower(text);
    string lowerQuery = toLower(query);
    size_t pos = lowerText.find(lowerQuery);
    if (pos == string::npos) return text;
    return text.substr(0, pos) +
    YELLOW + text.substr(pos, query.length()) + RESET +
    text.substr(pos + query.length());
}

// ─── komunikaty ───────────────────────────────────────────────────────────────
void helpmessage(const char* progName) {
    cout << RED << "Usage: " << RESET << progName
    << " <query> [options] or zpm search <query> [options]\n\n";
    cout << RED << "Options:\n" << RESET;
    cout << "  -h, --help     Show help\n";
    cout << "  -v, --version  Show version\n";
}

void versionmessage() {
    cout << RED << "zsearch component version: v" << zpm_version::version() << " of ZPM\n" << RESET;
    cout << "https://github.com/Zielina-Konrad-productions/ZPM\n";
    cout << "Copyright (c) 2026 Ignacyyy & Ry3ball\nLicense: MIT\n";
}

// ─── sekcje wyszukiwania ──────────────────────────────────────────────────────

void searchApt(const string& query, const string& queryTrimmed) {
    string command = "apt-cache search " + query;
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) { cerr << RED << "Error running apt-cache\n" << RESET; return; }

    char buffer[512];
    int  count = 0;
    bool headerPrinted = false;

    while (fgets(buffer, sizeof(buffer), pipe)) {
        string line(buffer);
        size_t dash = line.find(" - ");
        if (dash != string::npos) {
            if (!headerPrinted) {
                cout << YELLOW << "=== APT results ===\n" << RESET;
                headerPrinted = true;
            }
            string name = highlight(line.substr(0, dash), queryTrimmed);
            string desc = highlight(line.substr(dash + 3), queryTrimmed);
            cout << GREEN << "[APT]" << RESET << " " << name << "\n";
            cout << "    " << desc;
            count++;
        }
    }
    pclose(pipe);

    if (count == 0)
        cout << YELLOW << "=== APT: no results ===\n" << RESET;
    else
        cout << "\n" << GREEN << " APT found: " << count << " packages\n" << RESET;
}

void searchZypper(const string& query, const string& queryTrimmed) {
    // zypper search zwraca tabelę: S | Name | Summary | Type
    string command = "zypper --no-refresh search " + query + " 2>/dev/null";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) { cerr << RED << "Error running zypper\n" << RESET; return; }

    char buffer[512];
    int  count = 0;
    bool headerPrinted = false;
    bool pastHeader    = false; // pomijamy linie nagłówka tabeli

    while (fgets(buffer, sizeof(buffer), pipe)) {
        string line(buffer);
        line.erase(line.find_last_not_of(" \n\r\t") + 1);

        // Linie separatora (--+--+--) i nagłówek (S | Name | ...)
        if (line.find("-+-") != string::npos) { pastHeader = true; continue; }
        if (!pastHeader) continue;
        if (line.empty()) continue;

        // Format: "i | vim | Vi IMproved | package"
        // lub:    "  | vim | Vi IMproved | package"
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
        if (cols.size() < 3) continue;

        string name    = cols[1];
        string summary = cols[2];
        if (name.empty()) continue;

        if (!headerPrinted) {
            cout << YELLOW << "=== Zypper results ===\n" << RESET;
            headerPrinted = true;
        }

        cout << GREEN << "[ZYPPER]" << RESET << " "
        << highlight(name, queryTrimmed) << "\n";
        if (!summary.empty())
            cout << "    " << highlight(summary, queryTrimmed) << "\n";
        count++;
    }
    pclose(pipe);

    if (count == 0)
        cout << YELLOW << "=== Zypper: no results ===\n" << RESET;
    else
        cout << "\n" << GREEN << " Zypper found: " << count << " packages\n" << RESET;
}

void searchDnf(const string& query, const string& queryTrimmed) {
    // Parser dostosowany do Fedory z polskimi locale i dnf5 (wypluwającym wiodące spacje)
    string command = "dnf search " + query + " 2>/dev/null";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) { cerr << RED << "Error running dnf\n" << RESET; return; }

    char buffer[512];
    int  count = 0;
    bool headerPrinted = false;

    while (fgets(buffer, sizeof(buffer), pipe)) {
        string line(buffer);
        line.erase(line.find_last_not_of(" \n\r\t") + 1);
        if (line.empty()) continue;

        // Szybki odsiew linii dekoracyjnych
        if (line.find("====") != string::npos) continue;
        if (line.rfind("Last", 0) == 0) continue;

        // 1. Znajdź prawdziwy początek nazwy pakietu (pomiń wiodące spacje DNF-a)
        size_t nameStart = line.find_first_not_of(" \t");
        if (nameStart == string::npos) continue;

        // 2. Sprawdź, czy w linii występuje jawny separator (starsze wersje DNF)
        size_t sep = line.find_first_of(":|", nameStart);

        string nameArch, summary;
        if (sep != string::npos) {
            // Format z dwukropkiem lub kreską pionową
            nameArch = line.substr(nameStart, sep - nameStart);
            summary = line.substr(sep + 1);
        } else {
            // Format czysto kolumnowy (rozdzielony spacjami)
            size_t nameEnd = line.find_first_of(" \t", nameStart);
            if (nameEnd == string::npos) {
                nameArch = line.substr(nameStart);
                summary = "";
            } else {
                nameArch = line.substr(nameStart, nameEnd - nameStart);
                summary = line.substr(nameEnd);
            }
        }

        // 3. Obustronne czyszczenie (Trim)
        size_t f = nameArch.find_first_not_of(" \t");
        size_t l = nameArch.find_last_not_of(" \t");
        if (f == string::npos) continue;
        nameArch = nameArch.substr(f, l - f + 1);

        size_t sf = summary.find_first_not_of(" \t");
        summary = (sf != string::npos) ? summary.substr(sf) : "";

        // Jeśli opis zaczyna się od zbędnego znaku separatora, odetnij go
        if (!summary.empty() && (summary[0] == ':' || summary[0] == '|')) {
            summary = summary.substr(1);
            size_t sf2 = summary.find_first_not_of(" \t");
            summary = (sf2 != string::npos) ? summary.substr(sf2) : "";
        }

        // 4. Filtrowanie śmieci i logów systemowych (paczka w Linuxie nigdy nie ma spacji w nazwie)
        if (nameArch.find(' ') != string::npos) continue;

        string nameLower = toLower(nameArch);
        if (nameLower == "package" || nameLower == "name" || nameLower == "pakiet" ||
            nameLower == "nazwa" || nameLower == "updating" || nameLower == "loading" ||
            nameLower == "repositories" || nameLower == "matched" || nameLower == "for" ||
            nameLower == "wyszukano" || nameLower == "ostatnio" || nameLower == "summary" ||
            nameLower == "aktualizowanie" || nameLower == "załadowano" || nameLower == "dopasowane") {
            continue;
            }

            // Wycinanie architektury (.x86_64, .noarch itp.) dla estetyki ZPM
            string name = nameArch;
        size_t dot = nameArch.rfind('.');
        if (dot != string::npos && dot > 0) name = nameArch.substr(0, dot);

        if (!headerPrinted) {
            cout << YELLOW << "=== DNF results ===\n" << RESET;
            headerPrinted = true;
        }

        cout << GREEN << "[DNF]" << RESET << " "
        << highlight(name, queryTrimmed) << "\n";
        if (!summary.empty())
            cout << "    " << highlight(summary, queryTrimmed) << "\n";
        count++;
    }
    pclose(pipe);

    if (count == 0)
        cout << YELLOW << "=== DNF: no results ===\n" << RESET;
    else
        cout << "\n" << GREEN << " DNF found: " << count << " packages\n" << RESET;
}

void searchFlatpak(const string& query, const string& queryTrimmed) {
    bool hasFlatpak = (access("/usr/bin/flatpak", X_OK) == 0 ||
    access("/bin/flatpak",     X_OK) == 0);
    if (!hasFlatpak) return;

    string command = "flatpak search --columns=application,name,description "
    + query + " 2>/dev/null";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return;

    char buffer[512];
    int  count = 0;
    bool headerPrinted = false;

    while (fgets(buffer, sizeof(buffer), pipe)) {
        string line(buffer);
        line.erase(line.find_last_not_of(" \n\r\t") + 1);

        size_t t1 = line.find('\t');
        size_t t2 = line.find('\t', t1 + 1);
        if (t1 == string::npos) continue;

        string appId = line.substr(0, t1);
        string name  = (t2 != string::npos) ? line.substr(t1+1, t2-t1-1) : line.substr(t1+1);
        string desc  = (t2 != string::npos) ? line.substr(t2+1) : "";

        if (!headerPrinted) {
            cout << "\n" << YELLOW << "=== Flatpak results ===\n" << RESET;
            headerPrinted = true;
        }

        cout << GREEN << "[FLATPAK]" << RESET << " "
        << highlight(appId, queryTrimmed) << " - "
        << highlight(name,  queryTrimmed) << "\n";
        if (!desc.empty())
            cout << "    " << highlight(desc, queryTrimmed) << "\n";
        count++;
    }
    pclose(pipe);

    if (count == 0)
        cout << "\n" << YELLOW << "=== Flatpak: no results ===\n" << RESET;
    else
        cout << "\n" << GREEN << " Flatpak found: " << count << " packages\n" << RESET;
}

void searchSnap(const string& query, const string& queryTrimmed) {
    bool hasSnap = (access("/usr/bin/snap", X_OK) == 0 ||
    access("/bin/snap",     X_OK) == 0 ||
    access("/snap/bin/snap",X_OK) == 0);
    if (!hasSnap) return;

    string command = "snap find " + query + " 2>/dev/null";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return;

    char buffer[512];
    int  count = 0;
    bool headerPrinted = false;

    // Pierwsza linia to nagłówek — pomijamy
    fgets(buffer, sizeof(buffer), pipe);

    while (fgets(buffer, sizeof(buffer), pipe)) {
        string line(buffer);
        line.erase(line.find_last_not_of(" \n\r\t") + 1);
        if (line.empty()) continue;

        istringstream iss(line);
        string name, version, publisher, notes, summary;
        iss >> name >> version >> publisher >> notes;
        getline(iss, summary);
        if (!summary.empty())
            summary.erase(0, summary.find_first_not_of(" \t"));

        if (!headerPrinted) {
            cout << "\n" << YELLOW << "=== Snap results ===\n" << RESET;
            headerPrinted = true;
        }

        cout << GREEN << "[SNAP]" << RESET << " "
        << highlight(name, queryTrimmed) << "\n";
        if (!summary.empty())
            cout << "    " << highlight(summary, queryTrimmed) << "\n";
        count++;
    }
    pclose(pipe);

    if (count == 0)
        cout << "\n" << YELLOW << "=== Snap: no results ===\n" << RESET;
    else
        cout << "\n" << GREEN << " Snap found: " << count << " packages\n" << RESET;
}

// ─── main ─────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    zpm_update::checkForUpdates();

    bool showHelp    = false;
    bool showVersion = false;
    string query;

    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];

        if      (arg == "--help"    || arg == "-h") { showHelp    = true; continue; }
        else if (arg == "--version" || arg == "-v") { showVersion = true; continue; }

        // Walidacja — blokuj shell injection
        if (arg.find(';') != string::npos ||
            arg.find('&') != string::npos ||
            arg.find('|') != string::npos) {
            cerr << RED << "Invalid characters in query!\n" << RESET;
        return 1;
            }

            if (arg[0] != '-') query += arg + " ";
    }

    if (showVersion && showHelp) {
        cout << YELLOW << "--version\n" << RESET; versionmessage();
        cout << "\n" << YELLOW << "--help\n"    << RESET; helpmessage(argv[0]);
        return 0;
    }
    if (showVersion) { versionmessage();     return 0; }
    if (showHelp)    { helpmessage(argv[0]); return 0; }

    // Usuń trailing space
    string queryTrimmed = query;
    if (!queryTrimmed.empty() && queryTrimmed.back() == ' ')
        queryTrimmed.pop_back();

    if (queryTrimmed.empty()) {
        cerr << YELLOW << "No search query specified!\n" << RESET;
        return 1;
    }

    string pm = get_package_manager();

    cout << GREEN << " Searching: " << RESET << queryTrimmed << "\n\n";

    // Natywny PM
    if      (pm == "apt")    searchApt(query, queryTrimmed);
    else if (pm == "zypper") searchZypper(query, queryTrimmed);
    else if (pm == "dnf")    searchDnf(query, queryTrimmed);
    else {
        cerr << RED << "Error: Could not detect a supported package manager "
        << "(apt / zypper / dnf).\n" << RESET;
        return 1;
    }

    // Zawsze: Flatpak i Snap
    searchFlatpak(query, queryTrimmed);
    searchSnap(query, queryTrimmed);

    return 0;
}
