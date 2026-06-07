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
string execCommand(const char* cmd) {
    array<char, 128> buffer;
    string result;
    FILE* pipe = popen(cmd, "r");
    if (!pipe) return "";
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr)
        result += buffer.data();
    pclose(pipe);
    return result;
}

void helpmessage(const char* progName) {
    cout << RED << "Usage: " << RESET << progName << " [options]"
    << " or zpm upd/update [options]\n";
    cout << RED << "Options:" << RESET << endl;
    cout << "  --full, -f     Perform a full system upgrade" << endl;
    cout << "  -r             Reboot the system after update" << endl;
    cout << "  -s             Shutdown the system after update" << endl;
    cout << "  --yes, -y      Automatic system update" << endl;
    cout << "  --help, -h     Show this help message" << endl;
    cout << "  --version, -v  Show version information" << endl;
}

void versionmessage() {
    cout << RED << "zupd component version: v" << zpm_version::version()
    << " of ZPM\n" << RESET;
    cout << "https://github.com/Zielina-Konrad-productions/ZPM" << endl;
    cout << "Copyright (c) 2026 Ignacyyy & Ry3ball" << endl;
    cout << "License: MIT" << endl;
}

//-----------------------------------------------------------------------------
// repo()
//-----------------------------------------------------------------------------
void repo(const string& pm) {
    bool hasflatpak = (system("command -v flatpak >/dev/null 2>&1") == 0);
    bool hassnap    = (system("command -v snap    >/dev/null 2>&1") == 0);

    cout << YELLOW << "[SYS] " << RESET << flush;
    system("cat /etc/os-release 2>/dev/null | grep PRETTY_NAME | cut -d= -f2 | tr -d '\"'");

    // --- APT ---
    if (pm == "apt") {
        cout << "\n" << YELLOW << "[D]" << RESET << GREEN << " APT Repositories:\n" << RESET;
        string apt_cmd = "grep -rhE '^deb ' /etc/apt/sources.list* 2>/dev/null | sort -u | sed 's|^|" + YELLOW + "- " + RESET + "|'";
        system(apt_cmd.c_str());
    }
    // --- ZYPPER ---
    else if (pm == "zypper") {
        cout << "\n" << YELLOW << "[Z]" << RESET << GREEN << " Zypper Repositories:\n" << RESET;
        // Szukamy linii zaczynających się od cyfry (ID repozytorium), dzielimy po '|', bierzemy 2 kolumnę (Alias) i usuwamy z niej białe znaki
        string zypper_cmd = "zypper lr 2>/dev/null | awk -F'|' '/^[[:space:]]*[0-9]+/ {print $2}' | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' | sed 's|^|" + YELLOW + "- " + RESET + "|'";
        system(zypper_cmd.c_str());
    }
    // --- DNF ---
    else if (pm == "dnf") {
        cout << "\n" << YELLOW << "[R]" << RESET << GREEN << " DNF Repositories:\n" << RESET;
        // Wyciszamy dnf flagą -q (brak komunikatów o aktualizacji metadanych), ignorujemy nagłówek (NR>1) i bierzemy pierwszą kolumnę (ID repo)
        string dnf_cmd = "dnf repolist -q 2>/dev/null | awk 'NR>1 {print $1}' | grep -v '^[[:space:]]*$' | sed 's|^|" + YELLOW + "- " + RESET + "|'";
        system(dnf_cmd.c_str());
    }

    // --- FLATPAK ---
    if (hasflatpak) {
        cout << "\n" << YELLOW << "[F]" << RESET << GREEN << " Flatpak Remotes:\n" << RESET;
        string flatpak_cmd = "flatpak remotes --columns=name 2>/dev/null | sed 's|^|" + YELLOW + "- " + RESET + "|'";
        system(flatpak_cmd.c_str());
    }

    // --- SNAP ---
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

    bool any() const { return native || flatpak || snap; }
};

static int countSteps(const UpdateStatus& s) {
    int n = 2;
    if (s.native)                    n++;
    if (s.hasflatpak && s.flatpak)   n++;
    if (s.hassnap    && s.snap)      n++;
    return n;
}

static void checkUniversalManagers(UpdateStatus& s) {
    s.hasflatpak = (system("command -v flatpak >/dev/null 2>&1") == 0);
    s.hassnap    = (system("command -v snap    >/dev/null 2>&1") == 0);
    if (s.hasflatpak)
        s.flatpak = (system("flatpak remote-ls --updates 2>/dev/null | grep -q .") == 0);
    if (s.hassnap)
        s.snap = (system("snap refresh --list 2>/dev/null | grep -qvE '^(Name|All snaps)'") == 0);
}

//=============================================================================
// APT
//=============================================================================
UpdateStatus aptCheckUpdates() {
    UpdateStatus s;
    cout << endl;
    cout << YELLOW << "[*] Refreshing package cache..." << RESET << endl;
    system("apt-get update -qq 2>/dev/null");
    s.native = (system("apt-get dist-upgrade -s 2>/dev/null | grep -q '^Inst '") == 0);
    checkUniversalManagers(s);
    return s;
}

void aptUpdate(const UpdateStatus& status) {
    cout << RED << "\nPackages to update (APT):\n" << RESET;
    system(("apt-get dist-upgrade -s 2>/dev/null | grep '^Inst '"
    " | awk '{print \"" + YELLOW + "[+] " + RESET + "\" $2}'").c_str());

    if (status.hasflatpak && status.flatpak) {
        cout << RED << "\nPackages to update (Flatpak):\n" << RESET;
        system(("flatpak remote-ls --updates --columns=name 2>/dev/null"
        " | awk '{print \"" + YELLOW + "[+] " + RESET + "\" $0}'").c_str());
    }
    if (status.hassnap && status.snap) {
        cout << RED << "\nPackages to update (Snap):\n" << RESET;
        system(("snap refresh --list 2>/dev/null | grep -vE '^(Name|All snaps)'"
        " | awk '{print \"" + YELLOW + "[+] " + RESET + "\" $1}'").c_str());
    }

    if (!yes) {
        cout << "\n" << YELLOW << "Proceed with update?" << RESET << " [y/n]: ";
        if (!(cin >> ans)) ans = "x";
    }
    if (!(yes || ans == "y" || ans == "yes")) {
        cout << YELLOW << "[*] Update cancelled by user." << RESET << endl;
        return;
    }

    const int total = countSteps(status);
    int step = 0;
    bool ok = true;

    progressbar_start(total);

    progressbar_set_state(UiState::CHECKING, ++step);
    system("echo -----checking_system_consistency----- > /tmp/zupd.log");
    system("DEBIAN_FRONTEND=noninteractive dpkg --configure -a >> /tmp/zupd.log 2>&1");
    sleep(1);

    if (status.native) {
        progressbar_set_state(UiState::APT, ++step);
        system("echo -----updating_APT----- >> /tmp/zupd.log");
        const char* cmd = fullupdate
        ? "DEBIAN_FRONTEND=noninteractive apt-get dist-upgrade -y "
        "--with-new-pkgs "
        "-o APT::Get::Always-Include-Phased-Updates=true "
        "-o Dpkg::Options::=\"--force-confdef\" "
        "-o Dpkg::Options::=\"--force-confold\" >> /tmp/zupd.log 2>&1"
        : "DEBIAN_FRONTEND=noninteractive apt-get dist-upgrade -y "
        "-o Dpkg::Options::=\"--force-confdef\" "
        "-o Dpkg::Options::=\"--force-confold\" >> /tmp/zupd.log 2>&1";
        if (system(cmd) != 0) ok = false;
    }

    if (status.hasflatpak && status.flatpak) {
        progressbar_set_state(UiState::FLATPAK, ++step);
        system("echo ----updating_flatpak---- >> /tmp/zupd.log");
        if (system("flatpak update -y >> /tmp/zupd.log 2>&1") != 0) ok = false;
    }
    if (status.hassnap && status.snap) {
        progressbar_set_state(UiState::SNAP, ++step);
        system("echo ----updating_snap---- >> /tmp/zupd.log");
        if (system("snap refresh >> /tmp/zupd.log 2>&1") != 0) ok = false;
    }

    progressbar_set_state(UiState::CLEANUP, ++step);
    system("echo ----cleaning---- >> /tmp/zupd.log");
    system("apt-get autoremove -y >> /tmp/zupd.log 2>&1");
    system("apt-get autoclean    >> /tmp/zupd.log 2>&1");
    if (status.hasflatpak) {
        system("flatpak uninstall --unused -y   >> /tmp/zupd.log 2>&1");
        system("rm -rf /var/tmp/flatpak-cache-* >> /tmp/zupd.log 2>&1");
    }
    if (status.hassnap) {
        system("snap list --all 2>/dev/null "
        "| awk '/disabled/{print $1, $3}' "
        "| while read name rev; do snap remove \"$name\" --revision=\"$rev\"; done "
        ">> /tmp/zupd.log 2>&1");
        system("rm -rf /var/lib/snapd/cache/* >> /tmp/zupd.log 2>&1");
    }

    if (ok) { progressbar_set_state(UiState::DONE, total); progressbar_finish("DONE!"); }
    else    { progressbar_set_state(UiState::ERROR, step); progressbar_finish("ERROR!");
        cout << RED << "ERROR," << RESET << " check /tmp/zupd.log for details." << endl; return; }

        cout << YELLOW << "[RAPORT]" << RESET << " /tmp/zupd.log" << endl;
        if (reboot)        { cout << YELLOW << "[*] Rebooting in 3s..."    << RESET << endl; system("sleep 3 && reboot"); }
        else if (shutdown) { cout << YELLOW << "[*] Shutting down in 3s..." << RESET << endl; system("sleep 3 && shutdown -h now"); }
}

//=============================================================================
// ZYPPER
//=============================================================================
UpdateStatus zypperCheckUpdates() {
    UpdateStatus s;
    cout << endl;
    cout << YELLOW << "[*] Refreshing package cache..." << RESET << endl;
    system("zypper refresh -q 2>/dev/null");
    s.native = (system("zypper list-updates 2>/dev/null | grep -q '^v '") == 0);
    checkUniversalManagers(s);
    return s;
}

void zypperUpdate(const UpdateStatus& status) {
    cout << RED << "\nPackages to update (Zypper):\n" << RESET;
    system(("zypper list-updates 2>/dev/null | grep '^v '"
    " | awk -F'|' '{print \"" + YELLOW + "[+] " + RESET + "\" $3}'").c_str());

    if (status.hasflatpak && status.flatpak) {
        cout << RED << "\nPackages to update (Flatpak):\n" << RESET;
        system(("flatpak remote-ls --updates --columns=name 2>/dev/null"
        " | awk '{print \"" + YELLOW + "[+] " + RESET + "\" $0}'").c_str());
    }
    if (status.hassnap && status.snap) {
        cout << RED << "\nPackages to update (Snap):\n" << RESET;
        system(("snap refresh --list 2>/dev/null | grep -vE '^(Name|All snaps)'"
        " | awk '{print \"" + YELLOW + "[+] " + RESET + "\" $1}'").c_str());
    }

    if (!yes) {
        cout << "\n" << YELLOW << "Proceed with update?" << RESET << " [y/n]: ";
        if (!(cin >> ans)) ans = "x";
    }
    if (!(yes || ans == "y" || ans == "yes")) {
        cout << YELLOW << "[*] Update cancelled by user." << RESET << endl;
        return;
    }

    const int total = countSteps(status);
    int step = 0;
    bool ok = true;

    progressbar_start(total);

    progressbar_set_state(UiState::CHECKING, ++step);
    system("echo -----checking_system_consistency----- > /tmp/zupd.log");
    system("rpm --rebuilddb >> /tmp/zupd.log 2>&1");
    sleep(1);

    if (status.native) {
        progressbar_set_state(UiState::ZYPPER, ++step);
        system("echo -----updating_zypper----- >> /tmp/zupd.log");
        const char* cmd = fullupdate
        ? "zypper dup -y --no-confirm >> /tmp/zupd.log 2>&1"
        : "zypper update -y >> /tmp/zupd.log 2>&1";
        if (system(cmd) != 0) ok = false;
    }

    if (status.hasflatpak && status.flatpak) {
        progressbar_set_state(UiState::FLATPAK, ++step);
        system("echo ----updating_flatpak---- >> /tmp/zupd.log");
        if (system("flatpak update -y >> /tmp/zupd.log 2>&1") != 0) ok = false;
    }
    if (status.hassnap && status.snap) {
        progressbar_set_state(UiState::SNAP, ++step);
        system("echo ----updating_snap---- >> /tmp/zupd.log");
        if (system("snap refresh >> /tmp/zupd.log 2>&1") != 0) ok = false;
    }

    progressbar_set_state(UiState::CLEANUP, ++step);
    system("echo ----cleaning---- >> /tmp/zupd.log");
    system("zypper clean -a >> /tmp/zupd.log 2>&1");
    if (status.hasflatpak) {
        system("flatpak uninstall --unused -y   >> /tmp/zupd.log 2>&1");
        system("rm -rf /var/tmp/flatpak-cache-* >> /tmp/zupd.log 2>&1");
    }

    if (ok) { progressbar_set_state(UiState::DONE, total); progressbar_finish("DONE!"); }
    else    { progressbar_set_state(UiState::ERROR, step); progressbar_finish("ERROR!");
        cout << RED << "ERROR," << RESET << " check /tmp/zupd.log for details." << endl; return; }

        cout << YELLOW << "[RAPORT]" << RESET << " /tmp/zupd.log" << endl;
        if (reboot)        { cout << YELLOW << "[*] Rebooting in 3s..."    << RESET << endl; system("sleep 3 && reboot"); }
        else if (shutdown) { cout << YELLOW << "[*] Shutting down in 3s..." << RESET << endl; system("sleep 3 && shutdown -h now"); }
}

//=============================================================================
// DNF
//=============================================================================
UpdateStatus dnfCheckUpdates() {
    UpdateStatus s;
    cout << endl;
    cout << YELLOW << "[*] Refreshing package cache..." << RESET << endl;

    // dnf check-update: kod 100 = są updates, 0 = brak, inne = błąd
    // Wywołujemy raz — wynik idzie do /dev/null, kod wyjścia zachowujemy
    int ret = system("dnf check-update -q --refresh >/dev/null 2>&1");
    s.native = (WEXITSTATUS(ret) == 100);

    checkUniversalManagers(s);
    return s;
}

void dnfUpdate(const UpdateStatus& status) {
    cout << RED << "\nPackages to update (DNF):\n" << RESET;

    // Używamy popen zamiast system() — żeby kolory ANSI nie psuły output
    FILE* p = popen("dnf list updates 2>/dev/null | tail -n +2", "r");
    if (p) {
        char buf[512];
        bool any = false;
        while (fgets(buf, sizeof(buf), p)) {
            string line(buf);
            line.erase(line.find_last_not_of(" \n\r\t") + 1);
            if (line.empty()) continue;
            // Wytnij arch z nazwy (vim-enhanced.x86_64 → vim-enhanced)
            string name = line.substr(0, line.find(' '));
            size_t dot = name.rfind('.');
            if (dot != string::npos) name = name.substr(0, dot);
            cout << YELLOW << "[+] " << RESET << name << "\n";
            any = true;
        }
        pclose(p);
        if (!any) cout << YELLOW << "(no packages listed)" << RESET << "\n";
    }

    if (status.hasflatpak && status.flatpak) {
        cout << RED << "\nPackages to update (Flatpak):\n" << RESET;
        system(("flatpak remote-ls --updates --columns=name 2>/dev/null"
        " | awk '{print \"" + YELLOW + "[+] " + RESET + "\" $0}'").c_str());
    }
    if (status.hassnap && status.snap) {
        cout << RED << "\nPackages to update (Snap):\n" << RESET;
        system(("snap refresh --list 2>/dev/null | grep -vE '^(Name|All snaps)'"
        " | awk '{print \"" + YELLOW + "[+] " + RESET + "\" $1}'").c_str());
    }

    if (!yes) {
        cout << "\n" << YELLOW << "Proceed with update?" << RESET << " [y/n]: ";
        if (!(cin >> ans)) ans = "x";
    }
    if (!(yes || ans == "y" || ans == "yes")) {
        cout << YELLOW << "[*] Update cancelled by user." << RESET << endl;
        return;
    }

    const int total = countSteps(status);
    int step = 0;
    bool ok = true;

    progressbar_start(total);

    progressbar_set_state(UiState::CHECKING, ++step);
    system("echo -----checking_system_consistency----- > /tmp/zupd.log");
    system("rpm --rebuilddb >> /tmp/zupd.log 2>&1");
    sleep(1);

    if (status.native) {
        progressbar_set_state(UiState::DNF, ++step);
        system("echo -----updating_DNF----- >> /tmp/zupd.log");
        const char* cmd = fullupdate
        ? "dnf distro-sync -y >> /tmp/zupd.log 2>&1"
        : "dnf upgrade -y     >> /tmp/zupd.log 2>&1";
        if (system(cmd) != 0) ok = false;
    }

    if (status.hasflatpak && status.flatpak) {
        progressbar_set_state(UiState::FLATPAK, ++step);
        system("echo ----updating_flatpak---- >> /tmp/zupd.log");
        if (system("flatpak update -y >> /tmp/zupd.log 2>&1") != 0) ok = false;
    }
    if (status.hassnap && status.snap) {
        progressbar_set_state(UiState::SNAP, ++step);
        system("echo ----updating_snap---- >> /tmp/zupd.log");
        if (system("snap refresh >> /tmp/zupd.log 2>&1") != 0) ok = false;
    }

    progressbar_set_state(UiState::CLEANUP, ++step);
    system("echo ----cleaning---- >> /tmp/zupd.log");
    system("dnf autoremove -y  >> /tmp/zupd.log 2>&1");
    system("dnf clean packages >> /tmp/zupd.log 2>&1");
    if (status.hasflatpak) {
        system("flatpak uninstall --unused -y   >> /tmp/zupd.log 2>&1");
        system("rm -rf /var/tmp/flatpak-cache-* >> /tmp/zupd.log 2>&1");
    }

    if (ok) { progressbar_set_state(UiState::DONE, total); progressbar_finish("DONE!"); }
    else    { progressbar_set_state(UiState::ERROR, step); progressbar_finish("ERROR!");
        cout << RED << "ERROR," << RESET << " check /tmp/zupd.log for details." << endl; return; }

        cout << YELLOW << "[RAPORT]" << RESET << " /tmp/zupd.log" << endl;
        if (reboot)        { cout << YELLOW << "[*] Rebooting in 3s..."    << RESET << endl; system("sleep 3 && reboot"); }
        else if (shutdown) { cout << YELLOW << "[*] Shutting down in 3s..." << RESET << endl; system("sleep 3 && shutdown -h now"); }
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

    if ((reboot && shutdown) ||
        (help   && reboot)   || (help    && shutdown) ||
        (version && yes)     || (help    && yes)      ||
        (version && reboot)  || (version && shutdown) ||
        (fullupdate && version) || (fullupdate && help)) {
        cerr << RED << "Error: -r and -s are mutually exclusive. "
        << "--help and --version cannot be combined with other options."
        << RESET << endl;
    return 1;
        }

        if (version && help) {
            cout << YELLOW << "--version" << RESET << endl; versionmessage();
            cout << endl;
            cout << YELLOW << "--help"    << RESET << endl; helpmessage(argv[0]);
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
            if (s.any()) {
                if (fullupdate) { cout << YELLOW << "FULL UPDATE MODE" << RESET << endl; sleep(1); }
                aptUpdate(s);
            } else {
                cout << "\n" << RED << "System is up to date!" << RESET << endl;
            }
        }
        else if (pm == "zypper") {
            UpdateStatus s = zypperCheckUpdates();
            if (s.any()) {
                if (fullupdate) { cout << YELLOW << "FULL UPDATE MODE (dup)" << RESET << endl; sleep(1); }
                zypperUpdate(s);
            } else {
                cout << "\n" << RED << "System is up to date!" << RESET << endl;
            }
        }
        else if (pm == "dnf") {
            UpdateStatus s = dnfCheckUpdates();
            if (s.any()) {
                if (fullupdate) { cout << YELLOW << "FULL UPDATE MODE (distro-sync)" << RESET << endl; sleep(1); }
                dnfUpdate(s);
            } else {
                cout << "\n" << RED << "System is up to date!" << RESET << endl;
            }
        }

        return 0;
}
