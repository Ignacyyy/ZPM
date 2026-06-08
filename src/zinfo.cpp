#include "main.h"

using namespace std;

// ─── FORWARD DECLARATIONS ────────────────────────────────────────────────────
void showAptPackageInfo(const std::string& pkg);
void showRpmPackageInfo(const std::string& pkg, const std::string& pm);
void showFlatpakPackageInfo(const std::string& pkg);
void showSnapPackageInfo(const std::string& pkg);

// ─── helper: trim whitespace ─────────────────────────────────────────────────
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\n\r");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\n\r");
    return s.substr(a, b - a + 1);
}

static std::string toLowerStr(std::string s) {
    for (char& c : s) c = tolower((unsigned char)c);
    return s;
}

// ─── komunikaty ───────────────────────────────────────────────────────────────
void helpmessage(const char* progName) {
    cout << RED << "Usage: " << RESET << progName
         << " <package> [options] or zpm info <package> [options]\n"
         << RED << "Options:\n" << RESET
         << "  --help,     -h  Show this help message\n"
         << "  --version,  -v  Show version information\n";
}

void versionmessage() {
    cout << RED << "zinfo component version: v" << zpm_version::version() << " of ZPM\n" << RESET
         << "https://github.com/Zielina-Konrad-productions/ZPM\n"
         << "Copyright (c) 2026 Ignacyyy & Ry3ball\nLicense: MIT\n";
}

// ─── APT (bez zmian) ─────────────────────────────────────────────────────────
void showAptPackageInfo(const std::string& pkg) {
    FILE* pipe = popen(("apt show " + pkg + " 2>/dev/null").c_str(), "r");
    if (!pipe) return;

    char buffer[512];
    std::string line, name, version, priority, section, depends, recommends, homepage, desc;
    bool inDescription = false;

    while (fgets(buffer, sizeof(buffer), pipe)) {
        line = buffer;
        if      (line.find("Package:")     == 0) name       = line.substr(9);
        else if (line.find("Version:")     == 0) version    = line.substr(9);
        else if (line.find("Priority:")    == 0) priority   = line.substr(10);
        else if (line.find("Section:")     == 0) section    = line.substr(9);
        else if (line.find("Depends:")     == 0) depends    = line.substr(9);
        else if (line.find("Recommends:")  == 0) recommends = line.substr(12);
        else if (line.find("Homepage:")    == 0) homepage   = line.substr(10);
        else if (line.find("Description:") == 0) { desc = line.substr(13); inDescription = true; }
        else if (inDescription) desc += line;
    }
    pclose(pipe);

    if (name.empty()) return;

    FILE* p = popen(("dpkg -s " + pkg + " 2>/dev/null").c_str(), "r");
    bool installed = false;
    while (fgets(buffer, sizeof(buffer), p)) {
        if (std::string(buffer).find("Status: install ok installed") != std::string::npos)
            { installed = true; break; }
    }
    pclose(p);

    std::cout << YELLOW << "[APT] " << GREEN << name << RESET
              << CYAN   << "(" << trim(version) << ")" << RESET
              << BLUE   << (installed ? " [✓ Installed]" : " [ ] Not installed") << "\n\n"
              << YELLOW << "[I] " << GREEN << "Basic info\n" << RESET
              << "  Priority: " << priority
              << "  Section : " << section << "\n\n"
              << YELLOW << "[D] " << GREEN << "Dependencies\n" << RESET
              << "  Depends    : " << depends
              << "  Recommends : " << recommends << "\n\n"
              << YELLOW << "[H] " << GREEN << "Homepage\n" << RESET
              << "  " << homepage << "\n\n"
              << YELLOW << "[d] " << GREEN << "Description\n" << RESET
              << desc << "----------------------------------------\n\n";
}

// ─── RPM (zypper + dnf) ──────────────────────────────────────────────────────
void showRpmPackageInfo(const std::string& pkg, const std::string& pm) {
    std::string infoCmd = (pm == "zypper")
        ? "zypper --no-refresh info " + pkg + " 2>/dev/null"
        : "dnf info "                 + pkg + " 2>/dev/null";

    FILE* pipe = popen(infoCmd.c_str(), "r");
    if (!pipe) return;

    char buffer[512];
    std::string name, version, release, arch, summary, desc, homepage,
                group, vendor, installedSize, repo, license;
    bool inDesc = false, found = false;

    while (fgets(buffer, sizeof(buffer), pipe)) {
        std::string line(buffer);

        // Linie opisu zaczynają się od spacji (zypper) lub ": " po wielokrotnych liniach (dnf)
        if (inDesc) {
            // Zypper: opis kontynuowany wcięciem
            // DNF:    kolejne linie opisu mają ": " na początku
            if (line.size() >= 2 && (line[0] == ' ' || line[0] == '\t')) {
                desc += trim(line) + "\n";
                continue;
            }
            // DNF: linia kontynuacji zaczyna się od ": "
            if (line.find("                : ") != std::string::npos &&
                line.find(':') < 20) {
                // To nowe pole — nie opis
            } else if (line.find(": ") == std::string::npos && !trim(line).empty()) {
                desc += trim(line) + "\n";
                continue;
            }
            inDesc = false;
        }

        // Parsuj "Klucz : Wartość"
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string key = trim(line.substr(0, colon));
        std::string val = trim(line.substr(colon + 1));
        std::string k   = toLowerStr(key);

        // Nazwa / Name
        if (k == "name" || k == "nazwa" || k == "nom" || k == "nome") {
            name = val; found = true; inDesc = false;
        }
        // Wersja
        else if (k == "version" || k == "wersja" || k == "version") {
            version = val; inDesc = false;
        }
        // Release / Wydanie
        else if (k == "release" || k == "wydanie" || k == "révision" || k == "vydani") {
            release = val; inDesc = false;
        }
        // Arch
        else if (k == "arch" || k == "architektura" || k == "architecture" || k == "architett.") {
            arch = val; inDesc = false;
        }
        // Summary / Podsumowanie
        else if (k == "summary" || k == "podsumowanie" || k == "résumé" || k == "zusammenfassung"
              || k == "sommario") {
            summary = val; inDesc = false;
        }
        // Homepage / URL
        else if (k == "url" || k == "homepage" || k == "upstream url"
              || k == "strona domowa" || k == "adres url" || k == "url strony domowej") {
            homepage = val; inDesc = false;
        }
        // Group / Grupa
        else if (k == "group" || k == "grupa" || k == "gruppe" || k == "groupe") {
            group = val; inDesc = false;
        }
        // Vendor / Dostawca
        else if (k == "vendor" || k == "dostawca" || k == "vendeur" || k == "fornitore") {
            vendor = val; inDesc = false;
        }
        // Installed Size / Zainstalowany rozmiar
        else if (k == "installed size" || k == "zainstalowany rozmiar"
              || k == "taille installée" || k == "installierte größe") {
            installedSize = val; inDesc = false;
        }
        // Repo / Z repozytorium
        else if (k == "repository" || k == "z repozytorium" || k == "repo"
              || k == "référentiel" || k == "from repo") {
            repo = val; inDesc = false;
        }
        // License / Licencja
        else if (k == "license" || k == "licencja" || k == "licence" || k == "lizenz") {
            license = val; inDesc = false;
        }
        // Description / Opis
        else if (k == "description" || k == "opis" || k == "beschreibung"
              || k == "descrizione" || k == "description") {
            desc = val.empty() ? "" : val + "\n";
            inDesc = true;
        }
    }
    pclose(pipe);

    if (!found || name.empty()) return;

    bool installed = (system(("rpm -q " + pkg + " >/dev/null 2>&1").c_str()) == 0);

    std::string fullVer = version;
    if (!release.empty()) fullVer += "-" + release;
    if (!arch.empty())    fullVer += "." + arch;

    const char* pmLabel = (pm == "zypper") ? "ZYPPER" : "DNF";

    // ── nagłówek ──
    std::cout << YELLOW << "[" << pmLabel << "] "
              << GREEN  << name << RESET
              << CYAN   << " (" << fullVer << ")" << RESET
              << BLUE   << (installed ? " [✓ Installed]" : " [ ] Not installed") << RESET
              << "\n\n";

    // ── Basic info ──
    std::cout << YELLOW << "[I] " << GREEN << "Basic info\n" << RESET;
    if (!summary.empty())       std::cout << "  Summary  : " << summary       << "\n";
    if (!group.empty())         std::cout << "  Group    : " << group         << "\n";
    if (!repo.empty())          std::cout << "  Repo     : " << repo          << "\n";
    if (!vendor.empty())        std::cout << "  Vendor   : " << vendor        << "\n";
    if (!installedSize.empty()) std::cout << "  Size     : " << installedSize << "\n";
    if (!license.empty())       std::cout << "  License  : " << license       << "\n";
    std::cout << "\n";

    // ── Homepage ──
    if (!homepage.empty()) {
        std::cout << YELLOW << "[H] " << GREEN << "Homepage\n" << RESET
                  << "  " << homepage << "\n\n";
    }

    // ── Description ──
    if (!desc.empty()) {
        std::cout << YELLOW << "[d] " << GREEN << "Description\n" << RESET
                  << desc << "\n"
                  << "----------------------------------------\n\n";
    }
}

// ─── FLATPAK (bez zmian) ─────────────────────────────────────────────────────
void showFlatpakPackageInfo(const std::string& pkg) {
    bool flatpakFound = (access("/usr/bin/flatpak",     X_OK) == 0 ||
                         access("/usr/local/bin/flatpak",X_OK) == 0 ||
                         access("/bin/flatpak",          X_OK) == 0);
    if (!flatpakFound) return;

    char buffer[512];
    FILE* pipe = popen(("flatpak search --columns=application,version,description "
                        + pkg + " 2>/dev/null").c_str(), "r");
    if (!pipe) return;

    std::string appId, version, desc;
    bool found = false;

    if (fgets(buffer, sizeof(buffer), pipe)) {
        std::string row(buffer);
        size_t t1 = row.find('\t');
        size_t t2 = row.find('\t', t1 + 1);
        if (t1 != std::string::npos) {
            appId   = row.substr(0, t1);
            version = (t2 != std::string::npos) ? row.substr(t1+1, t2-t1-1) : row.substr(t1+1);
            desc    = (t2 != std::string::npos) ? row.substr(t2+1) : "";

            std::string appIdLower = toLowerStr(appId);
            std::string pkgLower   = toLowerStr(pkg);
            std::string lastSeg    = appIdLower;
            size_t dot = appIdLower.rfind('.');
            if (dot != std::string::npos) lastSeg = appIdLower.substr(dot+1);

            if (lastSeg == pkgLower || appIdLower.find("." + pkgLower) != std::string::npos)
                found = true;
        }
    }
    pclose(pipe);
    if (!found || appId.empty()) return;

    FILE* p = popen(("flatpak list --columns=application 2>/dev/null").c_str(), "r");
    bool installed = false;
    while (fgets(buffer, sizeof(buffer), p)) {
        std::string s = trim(std::string(buffer));
        if (s == appId) { installed = true; break; }
    }
    pclose(p);

    std::string homepage;
    if (installed) {
        FILE* ip = popen(("flatpak info " + appId + " 2>/dev/null").c_str(), "r");
        while (fgets(buffer, sizeof(buffer), ip)) {
            std::string s(buffer);
            if (s.find("URL:") != std::string::npos) {
                homepage = trim(s.substr(s.find("URL:") + 4));
                break;
            }
        }
        pclose(ip);
    }

    desc    = trim(desc);
    version = trim(version);

    std::cout << YELLOW << "[FLATPAK] " << GREEN << appId << RESET
              << CYAN   << " (" << version << ")" << RESET
              << BLUE   << (installed ? " [✓ Installed]" : " [ ] Not installed") << RESET
              << "\n\n";

    if (!homepage.empty())
        std::cout << YELLOW << "[H] " << GREEN << "Homepage\n" << RESET
                  << "  " << homepage << "\n\n";

    std::cout << YELLOW << "[d] " << GREEN << "Description\n" << RESET
              << "  " << desc << "\n"
              << "----------------------------------------\n\n";
}

// ─── SNAP (bez zmian) ────────────────────────────────────────────────────────
void showSnapPackageInfo(const std::string& pkg) {
    bool snapFound = (access("/usr/bin/snap",     X_OK) == 0 ||
                      access("/bin/snap",          X_OK) == 0 ||
                      access("/snap/bin/snap",     X_OK) == 0);
    if (!snapFound) return;

    char buffer[512];
    FILE* pipe = popen(("snap info " + pkg + " 2>/dev/null").c_str(), "r");
    if (!pipe) return;

    std::string name, version, publisher, homepage, summary, desc;
    bool inDesc = false, found = false;

    while (fgets(buffer, sizeof(buffer), pipe)) {
        std::string line(buffer);

        if (line.find("name:") == 0) {
            name = trim(line.substr(5)); found = true; inDesc = false;
        }
        else if (line.find("snap-id:") == 0) inDesc = false;
        else if (line.find("summary:") == 0) {
            summary = trim(line.substr(8)); inDesc = false;
        }
        else if (line.find("publisher:") == 0) {
            publisher = trim(line.substr(10)); inDesc = false;
        }
        else if (line.find("contact:") == 0 || line.find("links:") == 0) {
            homepage = trim(line.substr(line.find(':') + 1)); inDesc = false;
        }
        else if (line.find("description:") == 0) {
            desc = line.substr(12); inDesc = true;
        }
        else if (line.find("installed:") == 0) {
            std::string val = trim(line.substr(10));
            size_t sp = val.find(' ');
            version = (sp != std::string::npos) ? val.substr(0, sp) : val;
            inDesc = false;
        }
        else if (line.find("  ") == 0 && inDesc) desc += line.substr(2);
        else if (!line.empty() && line[0] != ' ') inDesc = false;
    }
    pclose(pipe);
    if (!found) return;

    // Fallback wersja ze stable channel
    if (version.empty()) {
        FILE* p2 = popen(("snap info " + pkg + " 2>/dev/null").c_str(), "r");
        while (fgets(buffer, sizeof(buffer), p2)) {
            std::string s(buffer);
            if (s.find("stable:") != std::string::npos) {
                version = trim(s.substr(s.find("stable:") + 7));
                size_t sp = version.find(' ');
                if (sp != std::string::npos) version = version.substr(0, sp);
                break;
            }
        }
        pclose(p2);
    }

    FILE* p = popen(("snap list " + pkg + " 2>/dev/null").c_str(), "r");
    bool installed = false;
    fgets(buffer, sizeof(buffer), p);
    if (fgets(buffer, sizeof(buffer), p)) installed = true;
    pclose(p);

    desc = trim(desc);

    std::cout << YELLOW << "[SNAP] " << GREEN << name << RESET;
    if (!version.empty())
        std::cout << CYAN << " (" << version << ")" << RESET;
    std::cout << BLUE << (installed ? " [✓ Installed]" : " [ ] Not installed") << RESET
              << "\n\n"
              << YELLOW << "[I] " << GREEN << "Basic info\n" << RESET
              << "  Publisher : " << publisher << "\n"
              << "  Summary   : " << summary   << "\n\n";

    if (!homepage.empty())
        std::cout << YELLOW << "[H] " << GREEN << "Homepage\n" << RESET
                  << "  " << homepage << "\n\n";

    std::cout << YELLOW << "[d] " << GREEN << "Description\n" << RESET
              << desc << "\n"
              << "----------------------------------------\n\n";
}

// ─── dispatcher ───────────────────────────────────────────────────────────────
void showPackageInfo(const std::string& pkg, const std::string& pm) {
    if (pm == "apt") showAptPackageInfo(pkg);
    else             showRpmPackageInfo(pkg, pm);
    showFlatpakPackageInfo(pkg);
    showSnapPackageInfo(pkg);
}

// ─── main ─────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    zpm_update::checkForUpdates();

    bool showHelp    = false;
    bool showVersion = false;
    vector<string> packages;

    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        if      (arg == "--help"    || arg == "-h") showHelp    = true;
        else if (arg == "--version" || arg == "-v") showVersion = true;
        else packages.push_back(arg);
    }

    if (showVersion && showHelp) {
        cout << YELLOW << "--version\n" << RESET; versionmessage();
        cout << "\n" << YELLOW << "--help\n" << RESET; helpmessage(argv[0]);
        return 0;
    }
    if (showVersion) { versionmessage();     return 0; }
    if (showHelp)    { helpmessage(argv[0]); return 0; }

    if (packages.empty()) {
        cerr << YELLOW << "No package specified!\n" << RESET;
        return 1;
    }

    string pm = get_package_manager();
    if (pm == "unknown") {
        cerr << RED << "Error: Could not detect a supported package manager "
             << "(apt / zypper / dnf).\n" << RESET;
        return 1;
    }

    for (const string& pkg : packages)
        showPackageInfo(pkg, pm);

    return 0;
}
