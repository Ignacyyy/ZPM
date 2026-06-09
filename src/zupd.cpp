#include "main.h"

using namespace std;

//zmienne globalne-------------------------------------------------------------
bool reboot     = false;
bool shutdown   = false;
bool help       = false;
bool version    = false;
bool yes        = false;
bool fullupdate = false;
string ans;
//koniec zmiennych globalnych--------------------------------------------------

//funkcje pomocnicze-----------------------------------------------------------
static string runCmd(const string& cmd) {
    string result;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return result;
    char buf[256];
    while (fgets(buf, sizeof(buf), p))
        result += buf;
    pclose(p);
    return result;
}

static void printCmdLines(const string& cmd, const string& prefix) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return;
    char buf[512];
    while (fgets(buf, sizeof(buf), p)) {
        string line(buf);
        size_t end = line.find_last_not_of(" \n\r\t");
        if (end == string::npos) continue;
        line = line.substr(0, end + 1);
        cout << prefix << line << "\n";
    }
    pclose(p);
}

void helpmessage(const char* progName) {
    cout << RED << "Usage: " << RESET << progName << " [options]"
         << " or zpm upd/update [options]\n";
    cout << RED << "Options:" << RESET << "\n"
         << "  --full, -f     Perform a full system upgrade\n"
         << "  -r             Reboot the system after update\n"
         << "  -s             Shutdown the system after update\n"
         << "  --yes, -y      Automatic system update\n"
         << "  --help, -h     Show this help message\n"
         << "  --version, -v  Show version information\n";
}

void versionmessage() {
    cout << RED << "zupd component version: v" << zpm_version::version()
         << " of ZPM\n" << RESET
         << "https://github.com/Zielina-Konrad-productions/ZPM\n"
         << "Copyright (c) 2026 Ignacyyy & Ry3ball\n"
         << "License: MIT\n";
}

//-----------------------------------------------------------------------------
// repo()
//-----------------------------------------------------------------------------
void repo(const string& pm) {
    bool hasflatpak = (access("/usr/bin/flatpak", X_OK) == 0 ||
                       access("/usr/local/bin/flatpak", X_OK) == 0);
    bool hassnap    = (access("/usr/bin/snap", X_OK) == 0);

    cout << YELLOW << "[SYS] " << RESET;
    string osname = runCmd("grep PRETTY_NAME /etc/os-release 2>/dev/null | cut -d= -f2 | tr -d '\"'");
    if (!osname.empty() && osname.back() == '\n') osname.pop_back();
    cout << osname << "\n";

    const string pfx = string(YELLOW) + "- " + RESET;

    if (pm == "apt") {
        cout << "\n" << YELLOW << "[D]" << RESET << GREEN << " APT Repositories:\n" << RESET;
        printCmdLines(
            "{ grep -rhE '^deb ' /etc/apt/sources.list /etc/apt/sources.list.d/ 2>/dev/null;"
            "  grep -rhE '^URIs:' /etc/apt/sources.list.d/*.sources 2>/dev/null"
            "    | sed 's/^URIs:[[:space:]]*/deb /'; } | sort -u",
            pfx);
    }
    else if (pm == "zypper") {
        cout << "\n" << YELLOW << "[Z]" << RESET << GREEN << " Zypper Repositories:\n" << RESET;
        printCmdLines(
            "zypper lr 2>/dev/null"
            " | awk -F'|' '/^[[:space:]]*[0-9]+/{gsub(/^[[:space:]]+|[[:space:]]+$/, \"\", $2); print $2}'",
            pfx);
    }
    else if (pm == "dnf") {
        cout << "\n" << YELLOW << "[R]" << RESET << GREEN << " DNF Repositories:\n" << RESET;
        bool hasdnf5 = (access("/usr/bin/dnf5", X_OK) == 0);
        printCmdLines(
            hasdnf5
                ? "dnf5 repolist -q 2>/dev/null | awk 'NR>1 && NF{print $1}'"
                : "dnf repolist -q 2>/dev/null | awk 'NR>1 && NF{print $1}'",
            pfx);
    }

    if (hasflatpak) {
        cout << "\n" << YELLOW << "[F]" << RESET << GREEN << " Flatpak Remotes:\n" << RESET;
        printCmdLines("flatpak remotes --columns=name 2>/dev/null", pfx);
    }
    if (hassnap) {
        cout << "\n" << YELLOW << "[S]" << RESET << GREEN << " Snap is available.\n" << RESET;
    }
}

//-----------------------------------------------------------------------------
// UpdateStatus
//-----------------------------------------------------------------------------
struct UpdateStatus {
    bool native     = false;
    bool flatpak    = false;
    bool snap       = false;
    bool hasflatpak = false;
    bool hassnap    = false;
    bool dnf5       = false;

    bool any() const { return native || flatpak || snap; }
};

static int countSteps(const UpdateStatus& s) {
    int n = 2; // CHECKING + CLEANUP zawsze
    if (s.native)                  n++;
    if (s.hasflatpak && s.flatpak) n++;
    if (s.hassnap    && s.snap)    n++;
    return n;
}

static void checkUniversalManagers(UpdateStatus& s) {
    s.hasflatpak = (access("/usr/bin/flatpak", X_OK) == 0 ||
                    access("/usr/local/bin/flatpak", X_OK) == 0);
    s.hassnap    = (access("/usr/bin/snap", X_OK) == 0);

    if (s.hasflatpak || s.hassnap) {
        string check_cmd;
        if (s.hasflatpak)
            check_cmd += "flatpak remote-ls --updates 2>/dev/null | grep -q . && echo HAS_FLATPAK_UPDATES;";
        if (s.hassnap)
            check_cmd += "snap refresh --list 2>/dev/null | grep -qvE '^(Name|All snaps)' && echo HAS_SNAP_UPDATES;";

        string out = runCmd(check_cmd);
        s.flatpak = (out.find("HAS_FLATPAK_UPDATES") != string::npos);
        s.snap    = (out.find("HAS_SNAP_UPDATES")    != string::npos);
    }
}

static void printUniversalUpdateLists(const UpdateStatus& s) {
    const string pfx = string(YELLOW) + "[+] " + RESET;
    if (s.hasflatpak && s.flatpak) {
        cout << RED << "\nPackages to update (Flatpak):\n" << RESET;
        printCmdLines("flatpak remote-ls --updates --columns=name 2>/dev/null", pfx);
    }
    if (s.hassnap && s.snap) {
        cout << RED << "\nPackages to update (Snap):\n" << RESET;
        printCmdLines("snap refresh --list 2>/dev/null | grep -vE '^(Name|All snaps)' | awk '{print $1}'", pfx);
    }
}

static bool askConfirm() {
    if (yes) return true;
    cout << "\n" << YELLOW << "Proceed with update?" << RESET << " [y/n]: ";
    if (!(cin >> ans)) return false;
    return (ans == "y" || ans == "yes");
}

static void cleanupUniversal(const UpdateStatus& s) {
    if (s.hasflatpak) {
        system("flatpak uninstall --unused -y >> /tmp/zupd.log 2>&1"
               " ; rm -rf /var/tmp/flatpak-cache-* >> /tmp/zupd.log 2>&1");
    }
    if (s.hassnap) {
        system("snap list --all 2>/dev/null"
               " | awk '/disabled/{print $1, $3}'"
               " | while read name rev; do snap remove \"$name\" --revision=\"$rev\"; done"
               " >> /tmp/zupd.log 2>&1"
               " ; rm -rf /var/lib/snapd/cache/* >> /tmp/zupd.log 2>&1");
    }
}

static void finishAndReport(bool ok, int total, int step) {
    if (ok) {
        progressbar_set_state(UiState::DONE, total);
        progressbar_finish("DONE!");
    } else {
        progressbar_set_state(UiState::ERROR, step);
        progressbar_finish("ERROR!");
        cout << RED << "ERROR," << RESET << " check /tmp/zupd.log for details.\n";
        return;
    }
    cout << YELLOW << "[RAPORT]" << RESET << " /tmp/zupd.log\n";
    if (reboot)        { cout << YELLOW << "[*] Rebooting in 3s..."     << RESET << "\n"; system("sleep 3 && reboot"); }
    else if (shutdown) { cout << YELLOW << "[*] Shutting down in 3s..." << RESET << "\n"; system("sleep 3 && shutdown -h now"); }
}

//=============================================================================
// APT
//=============================================================================
UpdateStatus aptCheckUpdates() {
    UpdateStatus s;
    cout << "\n" << YELLOW << "[*] Refreshing package cache..." << RESET << "\n";

    system("apt-get update -qq 2>/dev/null");
    string out = runCmd("apt-get dist-upgrade -s 2>/dev/null");
    s.native = (out.find("Inst ") != string::npos);

    checkUniversalManagers(s);
    return s;
}

void aptUpdate(const UpdateStatus& status) {
    const string pfx = string(YELLOW) + "[+] " + RESET;

    cout << RED << "\nPackages to update (APT):\n" << RESET;
    printCmdLines("apt-get dist-upgrade -s 2>/dev/null | grep '^Inst ' | awk '{print $2}'", pfx);
    printUniversalUpdateLists(status);

    if (!askConfirm()) {
        cout << YELLOW << "[*] Update cancelled by user." << RESET << "\n";
        return;
    }

    const int total = countSteps(status);
    int step = 0;
    bool ok = true;

    progressbar_start(total);

    progressbar_set_state(UiState::CHECKING, ++step);
    system("{ echo '-----checking_system_consistency-----';"
           "  DEBIAN_FRONTEND=noninteractive dpkg --configure -a; } > /tmp/zupd.log 2>&1");
    sleep(1);

    if (status.native) {
        progressbar_set_state(UiState::APT, ++step);
        if (system("{ echo '-----updating_APT-----';"
                   "  DEBIAN_FRONTEND=noninteractive apt-get dist-upgrade -y"
                   "    -o Dpkg::Options::='--force-confdef'"
                   "    -o Dpkg::Options::='--force-confold'; } >> /tmp/zupd.log 2>&1") != 0)
            ok = false;
    }

    if (status.hasflatpak && status.flatpak) {
        progressbar_set_state(UiState::FLATPAK, ++step);
        if (system("{ echo '----updating_flatpak----'; flatpak update -y; } >> /tmp/zupd.log 2>&1") != 0)
            ok = false;
    }
    if (status.hassnap && status.snap) {
        progressbar_set_state(UiState::SNAP, ++step);
        if (system("{ echo '----updating_snap----'; snap refresh; } >> /tmp/zupd.log 2>&1") != 0)
            ok = false;
    }

    progressbar_set_state(UiState::CLEANUP, ++step);
    system("{ echo '----cleaning----';"
           "  apt-get autoremove -y; apt-get autoclean; } >> /tmp/zupd.log 2>&1");
    cleanupUniversal(status);

    finishAndReport(ok, total, step);
}

//=============================================================================
// ZYPPER
//=============================================================================
UpdateStatus zypperCheckUpdates() {
    UpdateStatus s;
    cout << "\n" << YELLOW << "[*] Refreshing package cache..." << RESET << "\n";

    system("zypper refresh -q 2>/dev/null");
    string out = runCmd("zypper list-updates 2>/dev/null");
    s.native = (out.find("v |") != string::npos);

    checkUniversalManagers(s);
    return s;
}

void zypperUpdate(const UpdateStatus& status) {
    const string pfx = string(YELLOW) + "[+] " + RESET;

    cout << RED << "\nPackages to update (Zypper):\n" << RESET;
    printCmdLines(
        "zypper list-updates 2>/dev/null | awk -F'|' '/v \\|/{gsub(/^[[:space:]]+|[[:space:]]+$/,\"\",$3); print $3}'",
        pfx);
    printUniversalUpdateLists(status);

    if (!askConfirm()) {
        cout << YELLOW << "[*] Update cancelled by user." << RESET << "\n";
        return;
    }

    const int total = countSteps(status);
    int step = 0;
    bool ok = true;

    progressbar_start(total);

    progressbar_set_state(UiState::CHECKING, ++step);
    system("{ echo '-----checking_system_consistency-----'; rpm --rebuilddb; } > /tmp/zupd.log 2>&1");
    sleep(1);

    if (status.native) {
        progressbar_set_state(UiState::ZYPPER, ++step);
        const char* zypper_cmd = fullupdate
            ? "{ echo '-----updating_zypper_DUP-----'; zypper dup -y --no-confirm; } >> /tmp/zupd.log 2>&1"
            : "{ echo '-----updating_zypper-----';     zypper update -y; }            >> /tmp/zupd.log 2>&1";
        if (system(zypper_cmd) != 0) ok = false;
    }

    if (status.hasflatpak && status.flatpak) {
        progressbar_set_state(UiState::FLATPAK, ++step);
        if (system("{ echo '----updating_flatpak----'; flatpak update -y; } >> /tmp/zupd.log 2>&1") != 0)
            ok = false;
    }
    if (status.hassnap && status.snap) {
        progressbar_set_state(UiState::SNAP, ++step);
        if (system("{ echo '----updating_snap----'; snap refresh; } >> /tmp/zupd.log 2>&1") != 0)
            ok = false;
    }

    progressbar_set_state(UiState::CLEANUP, ++step);
    system("{ echo '----cleaning----'; zypper clean -a; } >> /tmp/zupd.log 2>&1");
    cleanupUniversal(status);

    finishAndReport(ok, total, step);
}

//=============================================================================
// DNF / DNF5
//=============================================================================
UpdateStatus dnfCheckUpdates() {
    UpdateStatus s;
    cout << "\n" << YELLOW << "[*] Refreshing package cache..." << RESET << "\n";

    s.dnf5 = (access("/usr/bin/dnf5", X_OK) == 0);

    if (s.dnf5) {
        system("dnf5 check-upgrade -q >/dev/null 2>&1");
        string out = runCmd("dnf5 list --upgrades 2>/dev/null");
        s.native = (out.find('\n') != string::npos &&
                    out.find('\n') != out.rfind('\n'));
    } else {
        system("dnf check-update -q --refresh >/dev/null 2>&1");
        string out = runCmd("dnf list updates 2>/dev/null");
        s.native = (out.find('\n') != string::npos &&
                    out.find('\n') != out.rfind('\n'));
    }

    checkUniversalManagers(s);
    return s;
}

void dnfUpdate(const UpdateStatus& status) {
    const string pfx = string(YELLOW) + "[+] " + RESET;

    cout << RED << "\nPackages to update (DNF" << (status.dnf5 ? "5" : "") << "):\n" << RESET;
    {
        const string list_cmd = status.dnf5
            ? "dnf5 list --upgrades 2>/dev/null | tail -n +2"
            : "dnf list updates 2>/dev/null | tail -n +2";
        FILE* p = popen(list_cmd.c_str(), "r");
        if (p) {
            char buf[512];
            bool any = false;
            while (fgets(buf, sizeof(buf), p)) {
                string line(buf);
                size_t end = line.find_last_not_of(" \n\r\t");
                if (end == string::npos) continue;
                string name = line.substr(0, line.find(' '));
                size_t dot = name.rfind('.');
                if (dot != string::npos) name = name.substr(0, dot);
                cout << pfx << name << "\n";
                any = true;
            }
            pclose(p);
            if (!any) cout << YELLOW << "(no packages listed)" << RESET << "\n";
        }
    }
    printUniversalUpdateLists(status);

    if (!askConfirm()) {
        cout << YELLOW << "[*] Update cancelled by user." << RESET << "\n";
        return;
    }

    const int total = countSteps(status);
    int step = 0;
    bool ok = true;

    progressbar_start(total);

    progressbar_set_state(UiState::CHECKING, ++step);
    system("{ echo '-----checking_system_consistency-----'; rpm --rebuilddb; } > /tmp/zupd.log 2>&1");
    sleep(1);

    if (status.native) {
        progressbar_set_state(UiState::DNF, ++step);
        const char* dnf_cmd;
        if (status.dnf5) {
            dnf_cmd = fullupdate
                ? "{ echo '-----updating_DNF5_distro-sync-----'; dnf5 distro-sync -y; } >> /tmp/zupd.log 2>&1"
                : "{ echo '-----updating_DNF5-----';             dnf5 upgrade -y;      } >> /tmp/zupd.log 2>&1";
        } else {
            dnf_cmd = fullupdate
                ? "{ echo '-----updating_DNF_distro-sync-----'; dnf distro-sync -y; } >> /tmp/zupd.log 2>&1"
                : "{ echo '-----updating_DNF-----';             dnf upgrade -y;      } >> /tmp/zupd.log 2>&1";
        }
        if (system(dnf_cmd) != 0) ok = false;
    }

    if (status.hasflatpak && status.flatpak) {
        progressbar_set_state(UiState::FLATPAK, ++step);
        if (system("{ echo '----updating_flatpak----'; flatpak update -y; } >> /tmp/zupd.log 2>&1") != 0)
            ok = false;
    }
    if (status.hassnap && status.snap) {
        progressbar_set_state(UiState::SNAP, ++step);
        if (system("{ echo '----updating_snap----'; snap refresh; } >> /tmp/zupd.log 2>&1") != 0)
            ok = false;
    }

    progressbar_set_state(UiState::CLEANUP, ++step);
    if (status.dnf5) {
        system("{ echo '----cleaning----';"
               "  dnf5 autoremove -y; dnf5 clean packages; } >> /tmp/zupd.log 2>&1");
    } else {
        system("{ echo '----cleaning----';"
               "  dnf autoremove -y; dnf clean packages; } >> /tmp/zupd.log 2>&1");
    }
    cleanupUniversal(status);

    finishAndReport(ok, total, step);
}

//=============================================================================
// main
//=============================================================================
int main(int argc, char* argv[]) {

    string pm = get_package_manager();
    zpm_update::checkForUpdates();

    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if      (arg == "--full"     || arg == "-f") fullupdate = true;
        else if (arg == "--reboot"   || arg == "-r") reboot     = true;
        else if (arg == "--shutdown" || arg == "-s") shutdown   = true;
        else if (arg == "--help"     || arg == "-h") help       = true;
        else if (arg == "--version"  || arg == "-v") version    = true;
        else if (arg == "--yes"      || arg == "-y") yes        = true;
    }

    if ((reboot && shutdown)         ||
        (help    && reboot)          || (help    && shutdown) ||
        (version && yes)             || (help    && yes)      ||
        (version && reboot)          || (version && shutdown) ||
        (fullupdate && version)      || (fullupdate && help)) {
        cerr << RED << "Error: -r and -s are mutually exclusive. "
             << "--help and --version cannot be combined with other options."
             << RESET << "\n";
        return 1;
    }

    if (version && help) {
        cout << YELLOW << "--version" << RESET << "\n"; versionmessage();
        cout << "\n";
        cout << YELLOW << "--help"    << RESET << "\n"; helpmessage(argv[0]);
        return 0;
    }
    if (version) { versionmessage();     return 0; }
    if (help)    { helpmessage(argv[0]); return 0; }

    if (geteuid() != 0) {
        cerr << RED << "Run with sudo!\n" << RESET;
        return 1;
    }
    if (pm == "unknown") {
        cerr << RED << "Error: Could not detect a supported package manager "
             << "(apt / zypper / dnf).\n" << RESET;
        return 1;
    }

    repo(pm);

    if (pm == "apt") {
        UpdateStatus s = aptCheckUpdates();
        if (!s.any()) { cout << "\n" << RED << "System is up to date!" << RESET << "\n"; return 0; }
        if (fullupdate) { cout << YELLOW << "FULL UPDATE MODE" << RESET << "\n"; sleep(1); }
        aptUpdate(s);
    }
    else if (pm == "zypper") {
        UpdateStatus s = zypperCheckUpdates();
        if (!s.any()) { cout << "\n" << RED << "System is up to date!" << RESET << "\n"; return 0; }
        if (fullupdate) { cout << YELLOW << "FULL UPDATE MODE (dup)" << RESET << "\n"; sleep(1); }
        zypperUpdate(s);
    }
    else if (pm == "dnf") {
        UpdateStatus s = dnfCheckUpdates();
        if (!s.any()) { cout << "\n" << RED << "System is up to date!" << RESET << "\n"; return 0; }
        if (fullupdate) { cout << YELLOW << "FULL UPDATE MODE (distro-sync)" << RESET << "\n"; sleep(1); }
        dnfUpdate(s);
    }

    return 0;
}