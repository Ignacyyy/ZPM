#include "main.h"

// ─── FORWARD DECLARATIONS ────────────────────────────────────────────────────
void showAptPackageInfo(const std::string& pkg);
void showRpmPackageInfo(const std::string& pkg);
void showFlatpakPackageInfo(const std::string& pkg);
void showSnapPackageInfo(const std::string& pkg);
std::string get_package_manager();

// ─── WYKRYWANIE PM ───────────────────────────────────────────────────────────
std::string get_package_manager() {
    FILE* f = fopen("/etc/os-release", "r");
    if (f) {
        char line[256];
        std::string id, id_like;
        while (fgets(line, sizeof(line), f)) {
            std::string s(line);
            if (!s.empty() && s.back() == '\n') s.pop_back();
            auto stripQ = [](const std::string& v) {
                std::string r = v;
                if (r.size() >= 2 && r.front() == '"' && r.back() == '"')
                    r = r.substr(1, r.size() - 2);
                return r;
            };
            if      (s.rfind("ID=",      0) == 0) id      = stripQ(s.substr(3));
            else if (s.rfind("ID_LIKE=", 0) == 0) id_like = stripQ(s.substr(8));
        }
        fclose(f);

        auto word = [](const std::string& hay, const std::string& needle) {
            size_t pos = hay.find(needle);
            if (pos == std::string::npos) return false;
            bool l = (pos == 0 || hay[pos-1] == ' ');
            bool r = (pos + needle.size() == hay.size() || hay[pos+needle.size()] == ' ');
            return l && r;
        };

        for (const std::string& src : {id_like, id}) {
            if (word(src,"debian") || word(src,"ubuntu"))                    return "apt";
            if (word(src,"suse")   || word(src,"opensuse"))                  return "zypper";
            if (word(src,"fedora") || word(src,"rhel") || word(src,"centos")
                || word(src,"rocky")  || word(src,"alma"))                      return "dnf";
        }
    }
    if (access("/usr/bin/apt-get", X_OK)==0 || access("/bin/apt-get", X_OK)==0) return "apt";
    if (access("/usr/bin/zypper",  X_OK)==0) return "zypper";
    if (access("/usr/bin/dnf",     X_OK)==0) return "dnf";
    return "unknown";
}

// ─── APT ─────────────────────────────────────────────────────────────────────
void showAptPackageInfo(const std::string& pkg) {
    std::string command = "apt show " + pkg + " 2>/dev/null";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return;

    char buffer[512];
    std::string line;
    std::string name, version, priority, section;
    std::string depends, recommends, homepage, desc;
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
        else if (line.find("Description:") == 0) {
            desc = line.substr(13);
            inDescription = true;
        }
        else if (inDescription) desc += line;
    }
    pclose(pipe);

    if (name.empty()) return;

    char buffer2[512];
    std::string check = "dpkg -s " + pkg + " 2>/dev/null";
    FILE* p = popen(check.c_str(), "r");
    bool installed = false;
    while (fgets(buffer2, sizeof(buffer2), p)) {
        if (std::string(buffer2).find("Status: install ok installed") != std::string::npos) {
            installed = true; break;
        }
    }
    pclose(p);

    std::cout << YELLOW << "[APT] " << GREEN << name << RESET;
    std::cout << CYAN   << " (" << version << ")" << RESET;
    if (installed) std::cout << BLUE << " [✓ Installed]"     << RESET;
    else           std::cout << BLUE << " [ ] Not installed" << RESET;
    std::cout << "\n\n";

    std::cout << YELLOW << "[I] " << GREEN << "Basic info\n"   << RESET;
    std::cout << "  Priority: " << priority;
    std::cout << "  Section : " << section << "\n\n";

    std::cout << YELLOW << "[D] " << GREEN << "Dependencies\n" << RESET;
    std::cout << "  Depends    : " << depends;
    std::cout << "  Recommends : " << recommends << "\n\n";

    std::cout << YELLOW << "[H] " << GREEN << "Homepage\n"     << RESET;
    std::cout << "  " << homepage << "\n\n";

    std::cout << YELLOW << "[d] " << GREEN << "Description\n"  << RESET;
    std::cout << desc << "\n";
    std::cout << "----------------------------------------\n\n";
}

// ─── RPM (zypper + dnf — oba używają rpm) ────────────────────────────────────
void showRpmPackageInfo(const std::string& pkg, const std::string& pm) {
    // Najpierw sprawdź czy pakiet w ogóle istnieje w repo
    std::string infoCmd;
    if (pm == "zypper")
        infoCmd = "zypper --no-refresh info " + pkg + " 2>/dev/null";
    else
        infoCmd = "dnf info " + pkg + " 2>/dev/null";

    FILE* pipe = popen(infoCmd.c_str(), "r");
    if (!pipe) return;

    char buffer[512];
    std::string name, version, release, arch;
    std::string summary, desc, homepage, group;
    bool inDesc = false;
    bool found  = false;

    while (fgets(buffer, sizeof(buffer), pipe)) {
        std::string line(buffer);

        auto extractVal = [&](size_t colonPos) {
            std::string val = line.substr(colonPos + 1);
            val.erase(0, val.find_first_not_of(" \t"));
            val.erase(val.find_last_not_of(" \n\r\t") + 1);
            return val;
        };

        // zypper używa "Name        :", dnf używa "Name         :"
        // szukamy po słowie kluczowym bez względu na padding
        auto startsWith = [&](const std::string& key) {
            size_t pos = line.find(key);
            return pos != std::string::npos && pos < 20; // klucz w pierwszych 20 znakach
        };

        if (startsWith("Name") && line.find(':') != std::string::npos) {
            name = extractVal(line.find(':'));
            found = true; inDesc = false;
        }
        else if (startsWith("Version") && line.find(':') != std::string::npos) {
            version = extractVal(line.find(':')); inDesc = false;
        }
        else if (startsWith("Release") && line.find(':') != std::string::npos) {
            release = extractVal(line.find(':')); inDesc = false;
        }
        else if (startsWith("Arch") && line.find(':') != std::string::npos) {
            arch = extractVal(line.find(':')); inDesc = false;
        }
        else if (startsWith("Summary") && line.find(':') != std::string::npos) {
            summary = extractVal(line.find(':')); inDesc = false;
        }
        else if ((startsWith("URL") || startsWith("Homepage")) && line.find(':') != std::string::npos) {
            // URL może mieć https:// — bierzemy wszystko po pierwszym ':'
            size_t firstColon = line.find(':');
            homepage = extractVal(firstColon);
            // Jeśli to "http" zostało przycięte, sklejamy z resztą
            if (homepage == "//") {
                // edge case — bierzemy surową linię od drugiego ':'
                size_t second = line.find(':', firstColon + 1);
                if (second != std::string::npos)
                    homepage = "https:" + line.substr(second - 5 > 0 ? second - 5 : 0);
            }
            // Lepszy sposób: wytnij od pierwszego http/ftp
            size_t urlStart = line.find("http");
            if (urlStart != std::string::npos) {
                homepage = line.substr(urlStart);
                homepage.erase(homepage.find_last_not_of(" \n\r\t") + 1);
            }
            inDesc = false;
        }
        else if ((startsWith("Group") || startsWith("Repo")) && line.find(':') != std::string::npos) {
            group = extractVal(line.find(':')); inDesc = false;
        }
        else if (startsWith("Description") && line.find(':') != std::string::npos) {
            desc   = "";
            inDesc = true;
        }
        else if (inDesc) {
            // zypper i dnf inaczej formatują opis — bierzemy wszystko niepuste
            if (line != "\n" && !line.empty())
                desc += line;
        }
    }
    pclose(pipe);

    if (!found || name.empty()) return;

    // Sprawdź czy zainstalowany przez rpm -q
    bool installed = (system(("rpm -q " + pkg + " >/dev/null 2>&1").c_str()) == 0);

    std::string pmLabel = (pm == "zypper") ? "ZYPPER" : "DNF";
    std::string fullVer = version;
    if (!release.empty()) fullVer += "-" + release;
    if (!arch.empty())    fullVer += "." + arch;

    std::cout << YELLOW << "[" << pmLabel << "] " << GREEN << name << RESET;
    std::cout << CYAN   << " (" << fullVer << ")" << RESET;
    if (installed) std::cout << BLUE << " [✓ Installed]"     << RESET;
    else           std::cout << BLUE << " [ ] Not installed" << RESET;
    std::cout << "\n\n";

    std::cout << YELLOW << "[I] " << GREEN << "Basic info\n" << RESET;
    if (!group.empty())   std::cout << "  Group   : " << group   << "\n";
    if (!summary.empty()) std::cout << "  Summary : " << summary << "\n";
    std::cout << "\n";

    if (!homepage.empty()) {
        std::cout << YELLOW << "[H] " << GREEN << "Homepage\n" << RESET;
        std::cout << "  " << homepage << "\n\n";
    }

    if (!desc.empty()) {
        std::cout << YELLOW << "[d] " << GREEN << "Description\n" << RESET;
        std::cout << desc << "\n";
    }
    std::cout << "----------------------------------------\n\n";
}

// ─── FLATPAK ─────────────────────────────────────────────────────────────────
void showFlatpakPackageInfo(const std::string& pkg) {
    bool flatpakFound = (access("/usr/bin/flatpak", X_OK) == 0 ||
    access("/bin/flatpak",     X_OK) == 0);
    if (!flatpakFound) return;

    char buffer[512];
    std::string searchCmd = "flatpak search --columns=application,version,description "
    + pkg + " 2>/dev/null";
    FILE* pipe = popen(searchCmd.c_str(), "r");
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

            std::string appIdLower = appId, pkgLower = pkg;
            std::transform(appIdLower.begin(), appIdLower.end(), appIdLower.begin(), ::tolower);
            std::transform(pkgLower.begin(),   pkgLower.end(),   pkgLower.begin(),   ::tolower);

            std::string lastSegment = appIdLower;
            size_t dot = appIdLower.rfind('.');
            if (dot != std::string::npos) lastSegment = appIdLower.substr(dot+1);

            if (lastSegment == pkgLower || appIdLower.find("." + pkgLower) != std::string::npos)
                found = true;
        }
    }
    pclose(pipe);

    if (!found || appId.empty()) return;

    std::string checkCmd = "flatpak list --columns=application 2>/dev/null";
    FILE* p = popen(checkCmd.c_str(), "r");
    bool installed = false;
    while (fgets(buffer, sizeof(buffer), p)) {
        std::string s(buffer);
        s.erase(s.find_last_not_of(" \n\r\t") + 1);
        if (s == appId) { installed = true; break; }
    }
    pclose(p);

    std::string homepage;
    if (installed) {
        FILE* ip = popen(("flatpak info " + appId + " 2>/dev/null").c_str(), "r");
        while (fgets(buffer, sizeof(buffer), ip)) {
            std::string s(buffer);
            if (s.find("URL:") != std::string::npos) {
                size_t pos = s.find("URL:");
                homepage = s.substr(pos+4);
                homepage.erase(0, homepage.find_first_not_of(" \t"));
                homepage.erase(homepage.find_last_not_of(" \n\r\t") + 1);
                break;
            }
        }
        pclose(ip);
    }

    desc.erase(desc.find_last_not_of(" \n\r\t") + 1);
    version.erase(version.find_last_not_of(" \n\r\t") + 1);

    std::cout << YELLOW << "[FLATPAK] " << GREEN << appId << RESET;
    std::cout << CYAN   << " (" << version << ")" << RESET;
    if (installed) std::cout << BLUE << " [✓ Installed]"     << RESET;
    else           std::cout << BLUE << " [ ] Not installed" << RESET;
    std::cout << "\n\n";

    if (!homepage.empty()) {
        std::cout << YELLOW << "[H] " << GREEN << "Homepage\n" << RESET;
        std::cout << "  " << homepage << "\n\n";
    }

    std::cout << YELLOW << "[d] " << GREEN << "Description\n" << RESET;
    std::cout << "  " << desc << "\n";
    std::cout << "----------------------------------------\n\n";
}

// ─── SNAP ────────────────────────────────────────────────────────────────────
void showSnapPackageInfo(const std::string& pkg) {
    bool snapFound = (access("/usr/bin/snap", X_OK) == 0 ||
    access("/bin/snap",     X_OK) == 0 ||
    access("/snap/bin/snap",X_OK) == 0);
    if (!snapFound) return;

    char buffer[512];
    FILE* pipe = popen(("snap info " + pkg + " 2>/dev/null").c_str(), "r");
    if (!pipe) return;

    std::string name, version, publisher, homepage, summary, desc;
    bool inDesc = false;
    bool found  = false;

    while (fgets(buffer, sizeof(buffer), pipe)) {
        std::string line(buffer);

        if (line.find("name:") == 0) {
            name = line.substr(5);
            name.erase(0, name.find_first_not_of(" \t"));
            name.erase(name.find_last_not_of(" \n\r\t") + 1);
            found = true; inDesc = false;
        }
        else if (line.find("snap-id:") == 0) inDesc = false;
        else if (line.find("summary:") == 0) {
            summary = line.substr(8);
            summary.erase(0, summary.find_first_not_of(" \t"));
            summary.erase(summary.find_last_not_of(" \n\r\t") + 1);
            inDesc = false;
        }
        else if (line.find("publisher:") == 0) {
            publisher = line.substr(10);
            publisher.erase(0, publisher.find_first_not_of(" \t"));
            publisher.erase(publisher.find_last_not_of(" \n\r\t") + 1);
            inDesc = false;
        }
        else if (line.find("contact:") == 0 || line.find("links:") == 0) {
            homepage = line.substr(line.find(':') + 1);
            homepage.erase(0, homepage.find_first_not_of(" \t"));
            homepage.erase(homepage.find_last_not_of(" \n\r\t") + 1);
            inDesc = false;
        }
        else if (line.find("description:") == 0) {
            desc = line.substr(12); inDesc = true;
        }
        else if (line.find("installed:") == 0) {
            std::string val = line.substr(10);
            val.erase(0, val.find_first_not_of(" \t"));
            size_t sp = val.find(' ');
            version = (sp != std::string::npos) ? val.substr(0, sp) : val;
            version.erase(version.find_last_not_of(" \n\r\t") + 1);
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
                size_t pos = s.find("stable:");
                version = s.substr(pos+7);
                version.erase(0, version.find_first_not_of(" \t"));
                size_t sp = version.find(' ');
                if (sp != std::string::npos) version = version.substr(0, sp);
                version.erase(version.find_last_not_of(" \n\r\t") + 1);
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

    desc.erase(desc.find_last_not_of(" \n\r\t") + 1);

    std::cout << YELLOW << "[SNAP] " << GREEN << name << RESET;
    if (!version.empty()) std::cout << CYAN << " (" << version << ")" << RESET;
    if (installed) std::cout << BLUE << " [✓ Installed]"     << RESET;
    else           std::cout << BLUE << " [ ] Not installed" << RESET;
    std::cout << "\n\n";

    std::cout << YELLOW << "[I] " << GREEN << "Basic info\n" << RESET;
    std::cout << "  Publisher : " << publisher << "\n";
    std::cout << "  Summary   : " << summary   << "\n\n";

    if (!homepage.empty()) {
        std::cout << YELLOW << "[H] " << GREEN << "Homepage\n" << RESET;
        std::cout << "  " << homepage << "\n\n";
    }

    std::cout << YELLOW << "[d] " << GREEN << "Description\n" << RESET;
    std::cout << desc << "\n";
    std::cout << "----------------------------------------\n\n";
}

// ─── DISPATCHER ──────────────────────────────────────────────────────────────
void showPackageInfo(const std::string& pkg, const std::string& pm) {
    if (pm == "apt")
        showAptPackageInfo(pkg);
    else
        showRpmPackageInfo(pkg, pm); // zypper i dnf

        showFlatpakPackageInfo(pkg);
    showSnapPackageInfo(pkg);
}

// ─── MAIN ────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    zpm_update::checkForUpdates();
    using namespace std;

    bool showHelp         = false;
    bool showVersion      = false;
    bool packageSpecified = false;

    string pm = get_package_manager();

    if (pm == "unknown") {
        std::cerr << RED << "Error: Could not detect a supported package manager "
        << "(apt / zypper / dnf).\n" << RESET;
        return 1;
    }

    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        if      (arg == "--help"    || arg == "-h") showHelp    = true;
        else if (arg == "--version" || arg == "-v") showVersion = true;
        else { packageSpecified = true; showPackageInfo(argv[i], pm); }
    }

    if (showVersion && showHelp) {
        cout << YELLOW << "--version\n" << RESET;
        cout << RED << "zinfo component version: v" << zpm_version::version() << " of ZPM\n" << RESET;
        cout << "https://github.com/Zielina-Konrad-productions/ZPM\n";
        cout << "Copyright (c) 2026 Ignacyyy & Ry3ball\nLicense: MIT\n\n";
        cout << YELLOW << "--help\n" << RESET;
        cout << RED << "Usage: " << RESET << argv[0]
        << " [packages...] [options] or zpm info [packages...] [options]\n\n";
        cout << RED << "Options:\n" << RESET;
        cout << "  --version, -v  Show version information\n";
        cout << "  --help,    -h  Show this help message\n";
        return 0;
    }
    if (showVersion) {
        cout << RED << "zinfo component version: v" << zpm_version::version() << " of ZPM\n" << RESET;
        cout << "https://github.com/Zielina-Konrad-productions/ZPM\n";
        cout << "Copyright (c) 2026 Ignacyyy & Ry3ball\nLicense: MIT\n";
        return 0;
    }
    if (showHelp) {
        cout << RED << "Usage: " << RESET << argv[0]
        << " [packages...] [options] or zpm info [packages...] [options]\n\n";
        cout << RED << "Options:\n" << RESET;
        cout << "  --version, -v  Show version information\n";
        cout << "  --help,    -h  Show this help message\n";
        return 0;
    }

    // Brak pakietu i brak flag — pokaż komunikat
    if (!packageSpecified && !showHelp && !showVersion) {
        cerr << YELLOW << "No package specified!" << RESET << endl;
        return 1;
    }

    return 0;
}
