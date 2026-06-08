#include "main.h"

using namespace std;
using zpm_update::exec;

// Zmienne globalne
bool help         = false;
bool version      = false;
bool force        = false;
bool experimental = false;
string odp;

static const char* LOG = "/tmp/zupgr.log";
static const char* LOG_APPEND = " >> /tmp/zupgr.log 2>&1";

// ─── helper: buduje "cmd >> /tmp/zupgr.log 2>&1" ─────────────────────────────
static string L(const string& cmd) { return cmd + LOG_APPEND; }

// ─── wersja zainstalowana ─────────────────────────────────────────────────────
// Zwraca {stable, pre} — jeśli VERSION.txt zawiera "-pre", traktuje jako prerelease
// i wypełnia oba pola (stable = "unknown", pre = wersja z pliku)
pair<string,string> ZPM_localVersions() {
    string ver_file, prever_file;

    // Czytaj VERSION.txt
    {
        ifstream f("/opt/ZPM/VERSION.txt");
        if (f.is_open()) getline(f, ver_file);
    }
    // Czytaj PREVERSION.txt
    {
        ifstream f("/opt/ZPM/PREVERSION.txt");
        if (f.is_open()) getline(f, prever_file);
    }

    string local_v   = "unknown";
    string local_pre = "unknown";

    if (!ver_file.empty()) {
        // Jeśli VERSION.txt zawiera "-pre" → to jest prerelease install
        if (ver_file.find("-pre") != string::npos) {
            local_pre = ver_file;
            // local_v pozostaje "unknown" — nie mamy zainstalowanej stabilnej
        } else {
            local_v = ver_file;
        }
    }

    // PREVERSION.txt ma wyższy priorytet dla pola pre
    if (!prever_file.empty())
        local_pre = prever_file;

    // Wyświetl
    if (local_v != "unknown")
        cout << "ZPM installed version:"    << YELLOW << " v" << local_v   << RESET << "\n";
    else
        cout << "ZPM installed version: "   << YELLOW << "none"             << RESET << "\n";

    if (local_pre != "unknown")
        cout << "ZPM installed preversion:" << YELLOW << " v" << local_pre << RESET << "\n";
    else
        cout << "ZPM installed preversion: " << YELLOW << "none"            << RESET << "\n";

    return {local_v, local_pre};
}

// ─── komunikaty ───────────────────────────────────────────────────────────────
void helpmessage(const char* progName) {
    cout << RED << "Usage: " << RESET << progName << " [options] or zpm upgr/upgrade [options]\n"
         << RED << "Options:\n" << RESET
         << "  -h, --help           Show help\n"
         << "  -v, --version        Show version\n"
         << "  -f, --force          Force reinstall even if already up to date\n"
         << "  --experimental, -ex  Update ZPM to prerelease versions\n";
}

void versionmessage() {
    cout << RED << "zupgr component version: v" << zpm_version::version() << " of ZPM\n" << RESET
         << "https://github.com/Zielina-Konrad-productions/ZPM\n"
         << "Copyright (c) 2026 Ignacyyy & Ry3ball\n"
         << "License: MIT\n";
}

// ─── GitHub API: pobiera stable i prerelease w jednym curl ────────────────────
// Zwraca {stable_ver, prerelease_ver} bez prefiksu 'v'
static pair<string,string> fetchGitHubVersions() {
    // Pobieramy listę releases (zawiera i stable i pre), jeden request zamiast dwóch
    string cmd =
        "curl -fsSL -H 'User-Agent: ZPM' "
        "https://api.github.com/repos/Zielina-Konrad-productions/ZPM/releases 2>/dev/null "
        "| python3 -c \"\n"
        "import sys, json\n"
        "try:\n"
        "    releases = json.load(sys.stdin)\n"
        "    stable = next((r for r in releases if isinstance(r,dict) and not r.get('prerelease') and not r.get('draft')), None)\n"
        "    pre    = next((r for r in releases if isinstance(r,dict) and r.get('prerelease')), None)\n"
        "    print(stable['tag_name'].lstrip('v') if stable else '')\n"
        "    print(pre   ['tag_name'].lstrip('v') if pre    else '')\n"
        "except:\n"
        "    print('')\n"
        "    print('')\n"
        "\" 2>/dev/null";

    string out = exec(cmd.c_str());

    // Podziel na dwie linie
    string stable, pre;
    size_t nl = out.find('\n');
    if (nl != string::npos) {
        stable = out.substr(0, nl);
        pre    = out.substr(nl + 1);
        // trim
        auto trimStr = [](string& s) {
            s.erase(remove(s.begin(), s.end(), '\n'), s.end());
            s.erase(remove(s.begin(), s.end(), '\r'), s.end());
            s.erase(remove(s.begin(), s.end(), ' '),  s.end());
        };
        trimStr(stable);
        trimStr(pre);
    }
    return {stable, pre};
}

// ─── parsowanie wersji ────────────────────────────────────────────────────────
static vector<int> parseVersion(string ver) {
    size_t dash = ver.find('-');
    if (dash != string::npos)
        ver = ver.substr(0, dash);

    vector<int> seg;
    stringstream ss(ver);
    string s;
    while (getline(ss, s, '.')) {
        try { seg.push_back(stoi(s)); } catch (...) { seg.push_back(0); }
    }
    while (seg.size() < 3) seg.push_back(0);
    return seg;
}

static bool isVersionOlder(const string& v1, const string& v2) {
    auto a = parseVersion(v1), b = parseVersion(v2);
    while (a.size() < b.size()) a.push_back(0);
    while (b.size() < a.size()) b.push_back(0);
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i] < b[i]) return true;
        if (a[i] > b[i]) return false;
    }
    return false;
}

// ─── wspólna logika aktualizacji ──────────────────────────────────────────────
bool run_update(const string& tag_name, const string& zip_name, bool is_pre = false) {
    bool fail = false;
    string zip_url = "https://github.com/Zielina-Konrad-productions/ZPM/archive/refs/tags/v"
                     + tag_name + ".zip";

    // 0/5 — init: backup configa, wyczyść stare pliki
    progressbar_start(0.0f, "0/5 | Starting update...");
    system(("{ echo '---starting update---';"
            "  rm -rf /tmp/ZPM* /tmp/zielina.conf;"
            "  mv /opt/ZPM/zielina.conf /tmp/zielina.conf;"
            "  rm -rf /opt/ZPM; } > " + string(LOG) + " 2>&1").c_str());

    // 1/5 — download + rozpakowanie
    progressbar_update(20.0f, "1/5 | Downloading newest version...");
    system(("{ echo '---downloading ZPM---';"
            "  curl -fsSL \"" + zip_url + "\" -o /tmp/" + zip_name + ";"
            "  unzip -q -o /tmp/" + zip_name + " -d /tmp/;"
            "  mv /tmp/ZPM-*/ /tmp/ZPM-built;"
            "  mkdir -p /opt/ZPM;"
            "  cp -r /tmp/ZPM-built/. /opt/ZPM/;"
            "  echo \"" + tag_name + "\" > /opt/ZPM/VERSION.txt;" +
            (is_pre ? "  echo \"" + tag_name + "\" > /opt/ZPM/PREVERSION.txt;" : "") +
            "} " + LOG_APPEND).c_str());

    // 2/5 — build
    progressbar_update(40.0f, "2/5 | Building ZPM...");
    if (system(L("{ echo '---building ZPM---'; bash /opt/ZPM/src/build.sh; }").c_str()) != 0)
        fail = true;
    system(L("chmod +x /opt/ZPM/bin/*").c_str());

    // 3/5 — symlinki
    progressbar_update(70.0f, "3/5 | Updating symlinks...");
    system(L("{ echo '---updating symlinks---';"
             "  for f in /opt/ZPM/bin/z*; do"
             "    [ -f \"$f\" ] && ln -sf \"$f\" /usr/bin/$(basename \"$f\");"
             "  done; }").c_str());

    // 4/5 — cleanup
    progressbar_update(90.0f, "4/5 | Cleaning up...");
    system(("{ echo '---cleaning---';"
            "  rm -f /opt/ZPM/zielina.conf;"
            "  mv /tmp/zielina.conf /opt/ZPM/zielina.conf;"
            "  rm -rf /tmp/ZPM-built /tmp/" + zip_name + "; } " + LOG_APPEND).c_str());

    // 5/5 — wynik
    if (!fail) {
        progressbar_finish("5/5 | DONE!");
        cout << YELLOW << "[RAPORT]" << RESET << " " << LOG << "\n";
    } else {
        progressbar_finish("5/5 | ERROR!");
        cout << RED    << "ERROR," << RESET << " check " << LOG << " for details.\n"
             << RED    << "R.I.P ZPM, please reinstall with this command:\n" << RESET
             << BOLD   << "sudo bash -c \"$(curl -fsSL https://raw.githubusercontent.com/"
                          "Zielina-Konrad-productions/ZPM/main/INETINSTALL.sh)\"\n" << RESET;
    }
    return fail;
}

// ─── main ─────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {

    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "--help"         || arg == "-h")  help         = true;
        if (arg == "--version"      || arg == "-v")  version      = true;
        if (arg == "--force"        || arg == "-f")  force        = true;
        if (arg == "--experimental" || arg == "-ex") experimental = true;
    }

    if ((help || version) && (force || experimental)) {
        cout << RED << "Error: --help/--version cannot be combined with --force/--experimental.\n" << RESET;
        return 1;
    }
    if (version && help) {
        cout << YELLOW << "--version\n" << RESET; versionmessage();
        cout << "\n" << YELLOW << "--help\n" << RESET; helpmessage(argv[0]);
        return 0;
    }
    if (version) { versionmessage();     return 0; }
    if (help)    { helpmessage(argv[0]); return 0; }

    if (geteuid() != 0) {
        cout << RED << "Run with sudo!\n" << RESET;
        return 1;
    }

    cout << RED << "ZPM Update program\n\n" << RESET;

    // Wersje lokalne — jedna funkcja wykrywa też prerelease w VERSION.txt
    cout << BOLD << "INSTALLED ZPM VERSIONS\n" << RESET
         << CYAN << "------------------------------------------------------\n" << RESET;
    auto [local_v, local_pre] = ZPM_localVersions();
    cout << CYAN << "------------------------------------------------------\n\n" << RESET;

    // Wersje z GitHub — jeden request
    cout << BOLD << "INTERNET ZPM VERSIONS\n" << RESET
         << CYAN << "------------------------------------------------------\n" << RESET;
    auto [repo_v, repo_pre] = fetchGitHubVersions();

    if (!repo_v.empty())
        cout << "ZPM latest version:"     << YELLOW << " v" << repo_v   << RESET << "\n";
    else
        cout << "ZPM latest version: "    << YELLOW << "unknown (no network?)" << RESET << "\n";

    if (!repo_pre.empty())
        cout << "ZPM latest preversion:"  << YELLOW << " v" << repo_pre << RESET << "\n";
    else
        cout << "ZPM latest preversion: " << YELLOW << "none available" << RESET << "\n";

    cout << CYAN << "------------------------------------------------------\n\n" << RESET;

    // Logika aktualizacji
    if (experimental) {
        if (repo_pre.empty()) {
            cout << RED << "No prerelease version available.\n" << RESET;
            return 0;
        }
        if (local_pre == "unknown" || isVersionOlder(local_pre, repo_pre) || force) {
            cout << GREEN << "ZPM prerelease update available" << RESET << ", continue? [y/n]: ";
            cin >> odp; cout << "\n";
            if (odp != "y" && odp != "Y") { cout << "Update cancelled.\n"; return 0; }
            cout << RED << "Updating ZPM...\n" << RESET;
            return run_update(repo_pre, "ZPM-experimental.zip", true) ? 1 : 0;
        } else {
            cout << "ZPM prerelease is up to date.\n";
        }
    } else {
        if (repo_v.empty()) {
            cout << RED << "Could not fetch latest version (no network?).\n" << RESET;
            return 1;
        }
        bool localIsPrerelease = (local_v == "unknown" && local_pre != "unknown");

        // 1. Jeśli jesteś na prerelease, tylko informujemy i wychodzimy (lub lecimy dalej)
        if (localIsPrerelease && !force) {
            cout << YELLOW << "Currently on prerelease (" << local_pre << "), stable " << repo_v << " available.\n" << RESET;
            cout << RED << "To update ZPM to normal release use -f or --force" << RESET << endl;
            return 0; // Albo break/return w zależności od reszty funkcji, żeby nie pytał o update
        }

        // 2. Standardowa logika dla normalnej aktualizacji lub wymuszenia (force)
        if (local_v == "unknown" || isVersionOlder(local_v, repo_v) || force) {
            cout << GREEN << "ZPM update available" << RESET << "\n";
            cout << "Continue? [y/n]: ";
            cin >> odp;
            if (odp != "y" && odp != "Y") { 
                cout << "Update cancelled.\n"; 
                return 0; 
            }
            return run_update(repo_v, "ZPM.zip", false) ? 1 : 0;
        } else {
            cout << RED << "ZPM is up to date.\n" << RESET;
        }
    }

    return 0;
}
