#include "main.h"

using namespace std;
using zpm_update::exec;

// Zmienne globalne
bool help = false;
bool version = false;
bool force = false;
bool experimental = false;
string odp;

// Wersja zainstalowana (stabilna)
string ZPM_ver() {
    ifstream plik("/opt/ZPM/VERSION.txt");
    string line;

    if (plik.is_open() && getline(plik, line) && !line.empty()) {
        cout << "ZPM installed version:" << YELLOW << " v" << line << RESET << endl;
    } else {
        line = "unknown";
        cout << "ZPM installed version: " << YELLOW << "unknown" << RESET << endl;
    }
    return line;
}

// Wersja zainstalowana (prerelease)
string ZPM_prever() {
    ifstream plik("/opt/ZPM/PREVERSION.txt");
    string line;

    if (plik.is_open() && getline(plik, line) && !line.empty()) {
        cout << "ZPM installed preversion:" << YELLOW << " v" << line << RESET << endl;
    } else {
        line = "unknown";
        cout << "ZPM installed preversion: " << YELLOW << "none" << RESET << endl;
    }

    return line;
}

// Komunikat pomocy
void helpmessage(const char* progName) {
    cout << RED << "Usage: " << RESET << progName << " [options] or zpm upgr/upgrade [options]\n\n";
    cout << "Options:\n";
    cout << "  -h, --help           Show help\n";
    cout << "  -v, --version        Show version\n";
    cout << "  -f, --force          Force reinstall even if already up to date\n";
    cout << "  --experimental, -ex  Update ZPM to prerelease versions\n";
}

// Komunikat wersji
void versionmessage() {
    cout << RED << "zupgr component version: v" << zpm_version::version() << " of ZPM\n" << RESET;
    cout << "https://github.com/Zielina-Konrad-productions/ZPM" << endl;
    cout << "Copyright (c) 2026 Ignacyyy & Ry3ball" << endl;
    cout << "License: MIT" << endl;
}

// Najnowsza wersja stabilna z GitHub (zwraca sam numer, bez 'v')
string ZPM_repover() {
    string cmd =
        "curl -fsSL -H 'User-Agent: ZPM' "
        "https://api.github.com/repos/Zielina-Konrad-productions/ZPM/releases/latest"
        " | python3 -c \""
        "import sys, json; "
        "r = json.load(sys.stdin); "
        "print(r['tag_name'].lstrip('v'))\"";

    string v = exec(cmd.c_str());
    v.erase(remove(v.begin(), v.end(), '\n'), v.end());
    v.erase(remove(v.begin(), v.end(), ' '),  v.end());

    if (!v.empty()) {
        cout << "ZPM latest version:" << YELLOW << " v" << v << RESET << endl;
    } else {
        cout << "ZPM latest version: " << YELLOW << "unknown (no network?)" << RESET << endl;
    }
    return v;
}

// Najnowsza wersja prerelease z GitHub (zwraca sam numer, bez 'v')
string ZPM_repoprever() {
    string cmd =
        "curl -fsSL -H 'User-Agent: ZPM' "
        "https://api.github.com/repos/Zielina-Konrad-productions/ZPM/releases"
        " | python3 -c \""
        "import sys, json; "
        "releases = json.load(sys.stdin); "
        "pre = [r for r in releases if r.get('prerelease', False)]; "
        "print(pre[0]['tag_name'].lstrip('v') if pre else '')\"";

    string v = exec(cmd.c_str());
    v.erase(remove(v.begin(), v.end(), '\n'), v.end());
    v.erase(remove(v.begin(), v.end(), ' '),  v.end());

    if (!v.empty()) {
        cout << "ZPM latest preversion:" << YELLOW << " v" << v << RESET << endl;
    } else {
        cout << "ZPM latest preversion: " << YELLOW << "none available" << RESET << endl;
    }
    return v;
}

// Parsowanie wersji do wektora int-ów (obsługa "-preX")
vector<int> parseVersion(string ver) {
    size_t pos = ver.find("-pre");
    if (pos != string::npos) {
        string suffix = ver.substr(pos + 4);
        ver.replace(pos, 4 + suffix.length(), suffix.empty() ? ".0" : "." + suffix);
    }

    vector<int> segments;
    stringstream ss(ver);
    string segment;
    while (getline(ss, segment, '.')) {
        try {
            segments.push_back(stoi(segment));
        } catch (...) {
            segments.push_back(0);
        }
    }
    return segments;
}

// Zwraca true jeśli v1 < v2 (porównanie semantyczne)
bool isVersionOlder(const string& v1, const string& v2) {
    vector<int> ver1 = parseVersion(v1);
    vector<int> ver2 = parseVersion(v2);

    while (ver1.size() < ver2.size()) ver1.push_back(0);
    while (ver2.size() < ver1.size()) ver2.push_back(0);

    for (size_t i = 0; i < ver1.size(); i++) {
        if (ver1[i] < ver2[i]) return true;
        if (ver1[i] > ver2[i]) return false;
    }
    return false;
}

// Wspólna logika aktualizacji
// tag_name - np. "1.9" (bez 'v')
// zip_name - nazwa pliku zip w /tmp
// is_pre   - true dla experimental (nadpisuje też PREVERSION.txt)
bool run_update(const string& tag_name, const string& zip_name, bool is_pre = false) {
    bool fail = false;
    string zip_url = "https://github.com/Zielina-Konrad-productions/ZPM/archive/refs/tags/v" + tag_name + ".zip";
    string dl      = "curl -fsSL \"" + zip_url + "\" -o /tmp/" + zip_name + " >> /tmp/zupgr.log 2>&1";
    string unzp    = "unzip -q -o /tmp/" + zip_name + " -d /tmp/ >> /tmp/zupgr.log 2>&1";
    string rmtmp   = "rm -rf /tmp/ZPM-built /tmp/" + zip_name + " >> /tmp/zupgr.log 2>&1";

    progressbar_start(0.0f, "0/5 | Starting update...");
    system("echo -------------------starting update---------------------- > /tmp/zupgr.log");
    system("rm -rf /tmp/ZPM* /tmp/zielina.conf >> /tmp/zupgr.log 2>&1");
    system("mv /opt/ZPM/zielina.conf /tmp/zielina.conf >> /tmp/zupgr.log 2>&1");
    system("rm -rf /opt/ZPM >> /tmp/zupgr.log 2>&1");

    progressbar_update(20.0f, "1/5 | Downloading newest version...");
    system("echo -------------------downloading ZPM---------------------- >> /tmp/zupgr.log");
    system(dl.c_str());
    system(unzp.c_str());
    system("mv /tmp/ZPM-*/ /tmp/ZPM-built >> /tmp/zupgr.log 2>&1");
    system("mkdir -p /opt/ZPM >> /tmp/zupgr.log 2>&1");
    system("cp -r /tmp/ZPM-built/. /opt/ZPM/ >> /tmp/zupgr.log 2>&1");

    string write_ver = "echo \"" + tag_name + "\" > /opt/ZPM/VERSION.txt 2>> /tmp/zupgr.log";
    system(write_ver.c_str());

    if (is_pre) {
        string write_prever = "echo \"" + tag_name + "\" > /opt/ZPM/PREVERSION.txt 2>> /tmp/zupgr.log";
        system(write_prever.c_str());
    }

    progressbar_update(40.0f, "2/5 | Building ZPM...");
    system("echo -------------------building ZPM---------------------- >> /tmp/zupgr.log");
    if (system("bash /opt/ZPM/src/build.sh >> /tmp/zupgr.log 2>&1") != 0) {
        fail = true;
    }
    system("chmod +x /opt/ZPM/bin/* >> /tmp/zupgr.log 2>&1");

    progressbar_update(70.0f, "3/5 | Updating symlinks...");
    system("echo -------------------updating symlinks---------------------- >> /tmp/zupgr.log");
    system("for f in /opt/ZPM/bin/z*; do [ -f \"$f\" ] && ln -sf \"$f\" /usr/bin/$(basename \"$f\"); done >> /tmp/zupgr.log 2>&1");

    progressbar_update(90.0f, "4/5 | Cleaning up...");
    system("echo -------------------cleaning---------------------- >> /tmp/zupgr.log");
    system("rm -f /opt/ZPM/zielina.conf >> /tmp/zupgr.log 2>&1");
    system("mv /tmp/zielina.conf /opt/ZPM/zielina.conf >> /tmp/zupgr.log 2>&1");
    system(rmtmp.c_str());

    if (!fail) {
        progressbar_finish("5/5 | DONE!");
        cout << YELLOW << "[RAPORT]" << RESET << " /tmp/zupgr.log" << endl;
    } else {
        progressbar_finish("5/5 | ERROR!");
        cout << RED << "ERROR," << RESET << " check /tmp/zupgr.log for details." << endl;
        cout << RED << "R.I.P ZPM, please reinstall, with whis command:" << RESET << endl;
        cout << BOLD << "sudo bash -c ""$(curl -fsSL https://raw.githubusercontent.com/Zielina-Konrad-productions/ZPM/main/INETINSTALL.sh)" << RESET << endl;
    }

    return fail;
}

int main(int argc, char* argv[]) {

    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "--help"         || arg == "-h")  help         = true;
        if (arg == "--version"      || arg == "-v")  version      = true;
        if (arg == "--force"        || arg == "-f")  force        = true;
        if (arg == "--experimental" || arg == "-ex") experimental = true;
    }

    if ((help || version) && (force || experimental)) {
        cout << RED << "Error: --help/--version cannot be combined with --force/--experimental." << RESET << endl;
        return 1;
    }

    if (version && help) {
        cout << YELLOW << "--version" << RESET << endl;
        versionmessage();
        cout << "\n" << YELLOW << "--help" << RESET << endl;
        helpmessage(argv[0]);
        return 0;
    }

    if (version) { versionmessage(); return 0; }
    if (help)    { helpmessage(argv[0]); return 0; }

    if (geteuid() != 0) {
        cout << RED << "Run with sudo!\n" << RESET;
        return 1;
    }

    cout << RED << "ZPM Update program" << RESET << "\n\n";

    cout << BOLD << "INSTALLED ZPM VERSIONS" << RESET << endl;
    cout << CYAN << "------------------------------------------------------" << RESET << endl;
    string local_v   = ZPM_ver();
    string local_pre = ZPM_prever();
    cout << CYAN << "------------------------------------------------------" << RESET << "\n\n";

    cout << BOLD << "INTERNET ZPM VERSIONS" << RESET << endl;
    cout << CYAN << "------------------------------------------------------" << RESET << endl;
    string repo_v   = ZPM_repover();
    string repo_pre = ZPM_repoprever();
    cout << CYAN << "------------------------------------------------------" << RESET << "\n\n";

    if (experimental) {
        if (repo_pre.empty()) {
            cout << RED << "No prerelease version available." << RESET << endl;
            return 0;
        }
        if (local_pre == "unknown" || isVersionOlder(local_pre, repo_pre) || force) {
            cout << GREEN << "ZPM prerelease update available" << RESET << ", continue? [y/n] ";
            cin >> odp;
            cout << "\n";
            if (odp != "y" && odp != "Y") { cout << "Update cancelled." << endl; return 0; }
            cout << RED << "Updating ZPM..." << RESET << endl;
            return run_update(repo_pre, "ZPM-experimental.zip", true) ? 1 : 0;
        } else {
            cout << "ZPM prerelease is up to date." << endl;
        }
    } else {
        if (repo_v.empty()) {
            cout << RED << "Could not fetch latest version (no network?)." << RESET << endl;
            return 1;
        }
        if (local_v == "unknown" || isVersionOlder(local_v, repo_v) || force) {
            cout << GREEN << "ZPM update available" << RESET << ", continue? [y/n]: ";
            cin >> odp;
            if (odp != "y" && odp != "Y") { cout << "Update cancelled." << endl; return 0; }
            if (!experimental) system("rm -rf /opt/ZPM/PREVERSION.txt");
            return run_update(repo_v, "ZPM.zip", false) ? 1 : 0;
        } else {
            cout << RED << "ZPM is up to date." << RESET << endl;
        }
    }

    return 0;
}