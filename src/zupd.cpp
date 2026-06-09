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

static int g_lockFd = -1;
volatile sig_atomic_t g_interrupted = 0;
//koniec zmiennych globalnych--------------------------------------------------

static void handleSigint(int) { g_interrupted = 1; }

//funkcje pomocnicze-----------------------------------------------------------
static int decodeExitStatus(int rc) {
    if (rc == -1) return 127;
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    return 127;
}

static string runCmd(const string& cmd, int* exitCode = nullptr) {
    string result;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) {
        if (exitCode) *exitCode = 127;
        return result;
    }

    char buf[256];
    while (fgets(buf, sizeof(buf), p))
        result += buf;

    if (exitCode) *exitCode = decodeExitStatus(pclose(p));
    else            pclose(p);

    return result;
}

static int runCmdExit(const string& cmd) {
    return decodeExitStatus(system(cmd.c_str()));
}

static bool systemOk(const char* cmd) {
    return decodeExitStatus(system(cmd)) == 0;
}

static bool checkInterrupted(bool& ok) {
    if (!g_interrupted) return false;

    cout << "\n" << YELLOW << "Cancelled by user (Ctrl+C).\n" << RESET;
    ok = false;
    return true;
}

static bool acquireLock() {
    const char* paths[] = { "/run/zupd.lock", "/tmp/zupd.lock" };
    bool lockBusy = false;

    for (const char* path : paths) {
        g_lockFd = open(path, O_CREAT | O_RDWR, 0644);
        if (g_lockFd < 0) continue;

        if (flock(g_lockFd, LOCK_EX | LOCK_NB) == 0)
            return true;

        lockBusy = true;
        close(g_lockFd);
        g_lockFd = -1;
    }

    if (lockBusy) {
        cerr << RED << "Error: Another zupd instance is already running.\n"
             << RESET;
    } else {
        cerr << RED << "Error: Cannot create lock file.\n" << RESET;
    }
    return false;
}

static void releaseLock() {
    if (g_lockFd < 0) return;

    flock(g_lockFd, LOCK_UN);
    close(g_lockFd);
    g_lockFd = -1;
}

struct ScopedLock {
    bool active = false;

    ScopedLock() { active = acquireLock(); }
    ~ScopedLock() { if (active) releaseLock(); }
};

static bool commandExists(const string& path) {
    return access(path.c_str(), X_OK) == 0;
}

static bool isTumbleweedLike() {
    string os = runCmd("cat /etc/os-release 2>/dev/null");

    transform(os.begin(), os.end(), os.begin(),
              [](unsigned char c) { return static_cast<char>(tolower(c)); });

    return os.find("tumbleweed") != string::npos ||
           os.find("slowroll")   != string::npos;
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
    bool hasflatpak = (commandExists("/usr/bin/flatpak") ||
                       commandExists("/usr/local/bin/flatpak"));
    bool hassnap    = commandExists("/usr/bin/snap");

    cout << YELLOW << "[SYS] " << RESET;

    string osname = runCmd(
        "grep PRETTY_NAME /etc/os-release 2>/dev/null | cut -d= -f2 | tr -d '\"'"
    );

    if (!osname.empty() && osname.back() == '\n')
        osname.pop_back();

    cout << osname << "\n";

    const string pfx = string(YELLOW) + "- " + RESET;

    if (pm == "apt") {
        cout << "\n" << YELLOW << "[D]" << RESET
             << GREEN << " APT Repositories:\n" << RESET;

        printCmdLines(
            "{ grep -rhE '^deb ' /etc/apt/sources.list /etc/apt/sources.list.d/ 2>/dev/null;"
            "  grep -rhE '^URIs:' /etc/apt/sources.list.d/*.sources 2>/dev/null"
            "    | sed 's/^URIs:[[:space:]]*/deb /'; } | sort -u",
            pfx
        );
    }
    else if (pm == "zypper") {
        cout << "\n" << YELLOW << "[Z]" << RESET
             << GREEN << " Zypper Repositories:\n" << RESET;

        printCmdLines(
            "zypper lr 2>/dev/null"
            " | awk -F'|' '/^[[:space:]]*[0-9]+/{"
            "gsub(/^[[:space:]]+|[[:space:]]+$/, \"\", $2); print $2}'",
            pfx
        );
    }
    else if (pm == "dnf") {
        cout << "\n" << YELLOW << "[R]" << RESET
             << GREEN << " DNF Repositories:\n" << RESET;

        bool hasdnf5 = commandExists("/usr/bin/dnf5");

        printCmdLines(
            hasdnf5
                ? "dnf5 repolist -q 2>/dev/null | awk 'NR>1 && NF{print $1}'"
                : "dnf repolist -q 2>/dev/null | awk 'NR>1 && NF{print $1}'",
            pfx
        );
    }

    if (hasflatpak) {
        cout << "\n" << YELLOW << "[F]" << RESET
             << GREEN << " Flatpak Remotes:\n" << RESET;

        printCmdLines("flatpak remotes --columns=name 2>/dev/null", pfx);
    }

    if (hassnap) {
        cout << "\n" << YELLOW << "[S]" << RESET
             << GREEN << " Snap is available.\n" << RESET;
    }
}

//-----------------------------------------------------------------------------
// UpdateStatus
//-----------------------------------------------------------------------------
struct UpdateStatus {
    bool native       = false;
    bool flatpak      = false;
    bool snap         = false;
    bool hasflatpak   = false;
    bool hassnap      = false;
    bool dnf5         = false;

    bool check_error  = false;
    bool zypper_dup   = false;

    bool any() const {
        return native || flatpak || snap;
    }
};

static int countSteps(const UpdateStatus& s) {
    int n = 2; // CHECKING + CLEANUP zawsze

    if (s.native)                  n++;
    if (s.hasflatpak && s.flatpak) n++;
    if (s.hassnap    && s.snap)    n++;

    return n;
}

static void checkUniversalManagers(UpdateStatus& s) {
    s.hasflatpak = (commandExists("/usr/bin/flatpak") ||
                    commandExists("/usr/local/bin/flatpak"));
    s.hassnap    = commandExists("/usr/bin/snap");

    if (s.hasflatpak || s.hassnap) {
        string check_cmd;

        if (s.hasflatpak) {
            check_cmd +=
                "flatpak remote-ls --updates 2>/dev/null "
                "| grep -q . && echo HAS_FLATPAK_UPDATES;";
        }

        if (s.hassnap) {
            check_cmd +=
                "snap refresh --list 2>/dev/null "
                "| grep -qvE '^(Name|All snaps)' && echo HAS_SNAP_UPDATES;";
        }

        string out = runCmd(check_cmd);

        s.flatpak = (out.find("HAS_FLATPAK_UPDATES") != string::npos);
        s.snap    = (out.find("HAS_SNAP_UPDATES")    != string::npos);
    }
}

static void printUniversalUpdateLists(const UpdateStatus& s) {
    const string pfx = string(YELLOW) + "[+] " + RESET;

    if (s.hasflatpak && s.flatpak) {
        cout << RED << "\nPackages to update (Flatpak):\n" << RESET;
        printCmdLines(
            "flatpak remote-ls --updates --columns=name 2>/dev/null",
            pfx
        );
    }

    if (s.hassnap && s.snap) {
        cout << RED << "\nPackages to update (Snap):\n" << RESET;
        printCmdLines(
            "snap refresh --list 2>/dev/null "
            "| grep -vE '^(Name|All snaps)' | awk '{print $1}'",
            pfx
        );
    }
}

static bool askConfirm() {
    if (yes) return true;

    cout << "\n" << YELLOW << "Proceed with update?" << RESET << " [y/n]: ";

    if (!(cin >> ans)) return false;

    return (ans == "y" || ans == "yes");
}

static bool cleanupUniversal(const UpdateStatus& s) {
    bool ok = true;

    if (s.hasflatpak) {
        if (!systemOk(
            "flatpak uninstall --unused -y >> /tmp/zupd.log 2>&1"
            " ; rm -rf /var/tmp/flatpak-cache-* >> /tmp/zupd.log 2>&1"
        )) {
            ok = false;
        }
    }

    if (s.hassnap) {
        if (!systemOk(
            "snap list --all 2>/dev/null"
            " | awk '/disabled/{print $1, $3}'"
            " | while read name rev; do snap remove \"$name\" --revision=\"$rev\"; done"
            " >> /tmp/zupd.log 2>&1"
            " ; rm -rf /var/lib/snapd/cache/* >> /tmp/zupd.log 2>&1"
        )) {
            ok = false;
        }
    }

    return ok;
}

static void finishAndReport(bool ok, int total, int step) {
    if (ok) {
        progressbar_set_state(UiState::DONE, total);
        progressbar_finish("DONE!");
    } else {
        progressbar_set_state(UiState::ERROR, step);
        progressbar_finish("ERROR!");

        cout << RED << "ERROR," << RESET
             << " check /tmp/zupd.log for details.\n";
        return;
    }

    cout << YELLOW << "[RAPORT]" << RESET << " /tmp/zupd.log\n";

    if (reboot) {
        cout << YELLOW << "[*] Rebooting in 3s..." << RESET << "\n";
        system("sleep 3 && reboot");
    }
    else if (shutdown) {
        cout << YELLOW << "[*] Shutting down in 3s..." << RESET << "\n";
        system("sleep 3 && shutdown -h now");
    }
}

//=============================================================================
// APT
//=============================================================================
UpdateStatus aptCheckUpdates() {
    UpdateStatus s;

    cout << "\n" << YELLOW << "[*] Refreshing package cache..."
         << RESET << "\n";

    int rc = runCmdExit(
        "{ echo '-----apt_update-----'; "
        "  apt-get update -qq; "
        "} > /tmp/zupd.log 2>&1"
    );

    if (rc != 0) {
        s.check_error = true;
        checkUniversalManagers(s);
        return s;
    }

    int sim_rc = 0;
    string out = runCmd("apt-get dist-upgrade -s 2>>/tmp/zupd.log", &sim_rc);

    if (sim_rc != 0) {
        s.check_error = true;
        checkUniversalManagers(s);
        return s;
    }

    s.native = (out.find("Inst ") != string::npos);

    checkUniversalManagers(s);
    return s;
}

static bool aptUpdate(const UpdateStatus& status) {
    const string pfx = string(YELLOW) + "[+] " + RESET;

    cout << RED << "\nPackages to update (APT):\n" << RESET;

    // POPRAWKA JĘZYKOWA: Dodano LC_ALL=C, aby wymusić standardowy format POSIX 
    // dla wyjścia komendy 'apt-get', eliminując problemy z lokalizacją językową.
    printCmdLines(
        "LC_ALL=C apt-get dist-upgrade -s 2>/dev/null "
        "| grep '^Inst ' | awk '{print $2}'",
        pfx
    );

    printUniversalUpdateLists(status);

    if (!askConfirm()) {
        cout << YELLOW << "[*] Update cancelled by user." << RESET << "\n";
        return true;
    }

    const int total = countSteps(status);
    int step = 0;
    bool ok = true;

    progressbar_start(total);

    progressbar_set_state(UiState::CHECKING, ++step);

    if (!systemOk(
        "{ echo '-----checking_system_consistency-----';"
        "  DEBIAN_FRONTEND=noninteractive dpkg --configure -a; "
        "} > /tmp/zupd.log 2>&1"
    )) {
        ok = false;
    }

    if (checkInterrupted(ok)) goto apt_finish;

    sleep(1);

    if (status.native && ok) {
        progressbar_set_state(UiState::APT, ++step);

        if (!systemOk(
            "{ echo '-----updating_APT-----';"
            "  DEBIAN_FRONTEND=noninteractive apt-get dist-upgrade -y"
            "    -o Dpkg::Options::='--force-confdef'"
            "    -o Dpkg::Options::='--force-confold'; "
            "} >> /tmp/zupd.log 2>&1"
        )) {
            ok = false;
        }
    }

    if (checkInterrupted(ok)) goto apt_finish;

    if (status.hasflatpak && status.flatpak && ok) {
        progressbar_set_state(UiState::FLATPAK, ++step);

        if (!systemOk(
            "{ echo '----updating_flatpak----'; flatpak update -y; } "
            ">> /tmp/zupd.log 2>&1"
        )) {
            ok = false;
        }
    }

    if (checkInterrupted(ok)) goto apt_finish;

    if (status.hassnap && status.snap && ok) {
        progressbar_set_state(UiState::SNAP, ++step);

        if (!systemOk(
            "{ echo '----updating_snap----'; snap refresh; } "
            ">> /tmp/zupd.log 2>&1"
        )) {
            ok = false;
        }
    }

    if (checkInterrupted(ok)) goto apt_finish;

    progressbar_set_state(UiState::CLEANUP, ++step);

    if (!systemOk(
        "{ echo '----cleaning----';"
        "  apt-get autoremove -y; apt-get autoclean; "
        "} >> /tmp/zupd.log 2>&1"
    )) {
        ok = false;
    }

    if (!cleanupUniversal(status))
        ok = false;

apt_finish:
    finishAndReport(ok, total, step);
    return ok && !g_interrupted;
}

//=============================================================================
// ZYPPER
//=============================================================================
UpdateStatus zypperCheckUpdates() {
    UpdateStatus s;

    cout << "\n" << YELLOW << "[*] Refreshing package cache..."
         << RESET << "\n";

    s.zypper_dup = fullupdate || isTumbleweedLike();

    int refresh_rc = runCmdExit(
        "{ echo '-----zypper_refresh-----'; "
        "  zypper --non-interactive refresh; "
        "} > /tmp/zupd.log 2>&1"
    );

    if (refresh_rc != 0) {
        s.check_error = true;
        checkUniversalManagers(s);
        return s;
    }

    string pkg_cmd;

    if (s.zypper_dup) {
        pkg_cmd =
            "zypper --no-refresh list-updates --all -t package 2>>/tmp/zupd.log "
            "| awk -F'|' '$1 ~ /v/ {"
            "gsub(/^[[:space:]]+|[[:space:]]+$/, \"\", $3); print $3"
            "}'";
    } else {
        pkg_cmd =
            "zypper --no-refresh list-updates -t package 2>>/tmp/zupd.log "
            "| awk -F'|' '$1 ~ /v/ {"
            "gsub(/^[[:space:]]+|[[:space:]]+$/, \"\", $3); print $3"
            "}'";
    }

    int pkg_rc = 0;
    string pkg_updates = runCmd(pkg_cmd, &pkg_rc);

    if (pkg_rc != 0) {
        s.check_error = true;
        checkUniversalManagers(s);
        return s;
    }

    int patch_rc = runCmdExit(
        "zypper --no-refresh patch-check "
        "> /tmp/zupd_patchcheck.log 2>>/tmp/zupd.log"
    );

    bool has_patches = (patch_rc == 100 || patch_rc == 101);

    if (patch_rc != 0 && patch_rc != 100 && patch_rc != 101) {
        s.check_error = true;
    }

    s.native = !pkg_updates.empty() || has_patches;

    checkUniversalManagers(s);
    return s;
}

static bool zypperUpdate(const UpdateStatus& status) {
    const string pfx = string(YELLOW) + "[+] " + RESET;

    cout << RED << "\nPackages to update (Zypper):\n" << RESET;

    // POPRAWKA JĘZYKOWA I PARSOWANIA: Dodano LC_ALL=C oraz rygorystyczne kotwice ^ i $ w awk.
    // Dzięki temu skrypt zadziała na każdym języku systemu i nie złapie losowych tekstów.
    if (status.zypper_dup) {
        printCmdLines(
            "LC_ALL=C zypper --no-refresh list-updates --all -t package 2>/dev/null "
            "| awk -F'|' '$1 ~ /^[[:space:]]*v[[:space:]]*$/ {"
            "gsub(/^[[:space:]]+|[[:space:]]+$/, \"\", $3); print $3"
            "}'",
            pfx
        );
    } else {
        printCmdLines(
            "LC_ALL=C zypper --no-refresh list-updates -t package 2>/dev/null "
            "| awk -F'|' '$1 ~ /^[[:space:]]*v[[:space:]]*$/ {"
            "gsub(/^[[:space:]]+|[[:space:]]+$/, \"\", $3); print $3"
            "}'",
            pfx
        );

        printCmdLines(
            "LC_ALL=C zypper --no-refresh list-patches 2>/dev/null "
            "| awk -F'|' '$1 ~ /^[[:space:]]*(needed|security|recommended|optional)[[:space:]]*$/ {"
            "gsub(/^[[:space:]]+|[[:space:]]+$/, \"\", $3); print $3"
            "}'",
            pfx
        );
    }

    printUniversalUpdateLists(status);

    if (!askConfirm()) {
        cout << YELLOW << "[*] Update cancelled by user." << RESET << "\n";
        return true;
    }

    const int total = countSteps(status);
    int step = 0;
    bool ok = true;

    progressbar_start(total);

    progressbar_set_state(UiState::CHECKING, ++step);

    system("{ echo '-----checking_system_consistency-----'; } > /tmp/zupd.log 2>&1");

    if (checkInterrupted(ok)) goto zypper_finish;

    sleep(1);

    if (status.native && ok) {
        progressbar_set_state(UiState::ZYPPER, ++step);

        const char* zypper_cmd = status.zypper_dup
            ? "{ echo '-----updating_zypper_DUP-----'; "
              "  zypper dup -y --auto-agree-with-licenses; "
              "} >> /tmp/zupd.log 2>&1"
            : "{ echo '-----updating_zypper_PATCH_WITH_UPDATE-----'; "
              "  zypper patch --with-update -y --auto-agree-with-licenses; "
              "} >> /tmp/zupd.log 2>&1";

        int zypper_exit = decodeExitStatus(system(zypper_cmd));

        if (zypper_exit == 103 || zypper_exit == 8) {
            progressbar_finish("RESTART NEEDED");

            cout << "\n" << YELLOW
                 << "[*] Zypper is adjusting its stack manager and has aborted the download.\n"
                 << "[*] The remaining system packages are NOT updated.\n"
                 << "[*] Restart command to update system.\n"
                 << RESET << "\n";

            system("{ echo '----cleaning----'; zypper clean -a; } >> /tmp/zupd.log 2>&1");
            cleanupUniversal(status);
            return false;
        }
        else if (zypper_exit != 0) {
            ok = false;
        }
    }

    if (checkInterrupted(ok)) goto zypper_finish;

    if (status.hasflatpak && status.flatpak && ok) {
        progressbar_set_state(UiState::FLATPAK, ++step);

        if (!systemOk(
            "{ echo '----updating_flatpak----'; flatpak update -y; } "
            ">> /tmp/zupd.log 2>&1"
        )) {
            ok = false;
        }
    }

    if (checkInterrupted(ok)) goto zypper_finish;

    if (status.hassnap && status.snap && ok) {
        progressbar_set_state(UiState::SNAP, ++step);

        if (!systemOk(
            "{ echo '----updating_snap----'; snap refresh; } "
            ">> /tmp/zupd.log 2>&1"
        )) {
            ok = false;
        }
    }

    if (checkInterrupted(ok)) goto zypper_finish;

    progressbar_set_state(UiState::CLEANUP, ++step);

    if (!systemOk(
        "{ echo '----cleaning----'; zypper clean -a; } "
        ">> /tmp/zupd.log 2>&1"
    )) {
        ok = false;
    }

    if (!cleanupUniversal(status))
        ok = false;

zypper_finish:
    finishAndReport(ok, total, step);
    return ok && !g_interrupted;
}

//=============================================================================
// DNF / DNF5
//=============================================================================
UpdateStatus dnfCheckUpdates() {
    UpdateStatus s;

    cout << "\n" << YELLOW << "[*] Refreshing package cache..."
         << RESET << "\n";

    s.dnf5 = commandExists("/usr/bin/dnf5");

    int rc;

    if (s.dnf5) {
        rc = runCmdExit(
            "{ echo '-----dnf5_check_upgrade-----'; "
            "  dnf5 check-upgrade -q; "
            "} > /tmp/zupd.log 2>&1"
        );
    } else {
        rc = runCmdExit(
            "{ echo '-----dnf_check_update-----'; "
            "  dnf check-update -q --refresh; "
            "} > /tmp/zupd.log 2>&1"
        );
    }

    // DNF zwraca 100, gdy są aktualizacje.
    if (rc == 100) {
        s.native = true;
    } else if (rc != 0) {
        s.check_error = true;
    }

    checkUniversalManagers(s);
    return s;
}

static bool dnfUpdate(const UpdateStatus& status) {
    const string pfx = string(YELLOW) + "[+] " + RESET;

    cout << RED << "\nPackages to update (DNF"
         << (status.dnf5 ? "5" : "") << "):\n" << RESET;

    {
        // POPRAWKA JĘZYKOWA: Dodano LC_ALL=C, aby DNF zawsze zwracał dane 
        // w standardowym angielskim formacie, bez względu na język systemu.
        const string list_cmd = status.dnf5
            ? "LC_ALL=C dnf5 list --upgrades -q 2>/dev/null"
            : "LC_ALL=C dnf list updates -q 2>/dev/null";

        FILE* p = popen(list_cmd.c_str(), "r");

        if (p) {
            char buf[512];
            bool any = false;

            while (fgets(buf, sizeof(buf), p)) {
                string line(buf);
                
                // UODPORNIENIE PARSOWANIA: Używamy strumienia (upewnij się, że masz #include <sstream>)
                // Prawdziwy wpis pakietu w DNF zawsze składa się z 3 kolumn (Nazwa.arch Wersja Repozytorium).
                stringstream ss(line);
                string pkg, version, repo;

                // Jeśli linia nie ma 3 kolumn (np. pusta linia lub krótki komunikat), pomijamy ją
                if (!(ss >> pkg >> version >> repo)) continue;

                // Szukamy kropki oddzielającej architekturę (np. .x86_64, .noarch)
                size_t dot = pkg.rfind('.');
                if (dot == string::npos) continue;

                // Dodatkowe odfiltrowanie angielskich nagłówków tabeli DNF
                if (pkg == "Available" || pkg == "Package") continue;

                string name = pkg.substr(0, dot);
                cout << pfx << name << "\n";
                any = true;
            }

            pclose(p);

            if (!any)
                cout << YELLOW << "(no packages listed)" << RESET << "\n";
        }
    }

    printUniversalUpdateLists(status);

    if (!askConfirm()) {
        cout << YELLOW << "[*] Update cancelled by user." << RESET << "\n";
        return true;
    }

    const int total = countSteps(status);
    int step = 0;
    bool ok = true;

    progressbar_start(total);
    progressbar_set_state(UiState::CHECKING, ++step);

    system("{ echo '-----checking_system_consistency-----'; } > /tmp/zupd.log 2>&1");

    if (checkInterrupted(ok)) goto dnf_finish;

    sleep(1);

    if (status.native && ok) {
        progressbar_set_state(UiState::DNF, ++step);
        const char* dnf_cmd;

        if (status.dnf5) {
            dnf_cmd = fullupdate
                ? "{ echo '-----updating_DNF5_distro-sync-----'; dnf5 distro-sync -y; } >> /tmp/zupd.log 2>&1"
                : "{ echo '-----updating_DNF5-----'; dnf5 upgrade -y; } >> /tmp/zupd.log 2>&1";
        } else {
            dnf_cmd = fullupdate
                ? "{ echo '-----updating_DNF_distro-sync-----'; dnf distro-sync -y; } >> /tmp/zupd.log 2>&1"
                : "{ echo '-----updating_DNF-----'; dnf upgrade -y; } >> /tmp/zupd.log 2>&1";
        }

        if (decodeExitStatus(system(dnf_cmd)) != 0)
            ok = false;
    }

    if (checkInterrupted(ok)) goto dnf_finish;

    if (status.hasflatpak && status.flatpak && ok) {
        progressbar_set_state(UiState::FLATPAK, ++step);

        if (!systemOk(
            "{ echo '----updating_flatpak----'; flatpak update -y; } "
            ">> /tmp/zupd.log 2>&1"
        )) {
            ok = false;
        }
    }

    if (checkInterrupted(ok)) goto dnf_finish;

    if (status.hassnap && status.snap && ok) {
        progressbar_set_state(UiState::SNAP, ++step);

        if (!systemOk(
            "{ echo '----updating_snap----'; snap refresh; } "
            ">> /tmp/zupd.log 2>&1"
        )) {
            ok = false;
        }
    }

    if (checkInterrupted(ok)) goto dnf_finish;

    progressbar_set_state(UiState::CLEANUP, ++step);

    if (status.dnf5) {
        if (!systemOk(
            "{ echo '----cleaning----';"
            "  dnf5 autoremove -y; dnf5 clean packages; "
            "} >> /tmp/zupd.log 2>&1"
        )) {
            ok = false;
        }
    } else {
        if (!systemOk(
            "{ echo '----cleaning----';"
            "  dnf autoremove -y; dnf clean packages; "
            "} >> /tmp/zupd.log 2>&1"
        )) {
            ok = false;
        }
    }

    if (!cleanupUniversal(status))
        ok = false;

dnf_finish:
    finishAndReport(ok, total, step);
    return ok && !g_interrupted;
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
        (help    && reboot)          ||
        (help    && shutdown)        ||
        (version && yes)             ||
        (help    && yes)             ||
        (version && reboot)          ||
        (version && shutdown)        ||
        (fullupdate && version)      ||
        (fullupdate && help)) {
        cerr << RED
             << "Error: -r and -s are mutually exclusive. "
             << "--help and --version cannot be combined with other options."
             << RESET << "\n";
        return 1;
    }

    if (version && help) {
        cout << YELLOW << "--version" << RESET << "\n";
        versionmessage();

        cout << "\n";

        cout << YELLOW << "--help" << RESET << "\n";
        helpmessage(argv[0]);

        return 0;
    }

    if (version) {
        versionmessage();
        return 0;
    }

    if (help) {
        helpmessage(argv[0]);
        return 0;
    }

    if (geteuid() != 0) {
        cerr << RED << "Run with sudo!\n" << RESET;
        return 1;
    }

    if (pm == "unknown") {
        cerr << RED
             << "Error: Could not detect a supported package manager "
             << "(apt / zypper / dnf).\n"
             << RESET;
        return 1;
    }

    ScopedLock lock;
    if (!lock.active)
        return 1;

    signal(SIGINT, handleSigint);

    repo(pm);

    bool updateOk = true;

    if (pm == "apt") {
        UpdateStatus s = aptCheckUpdates();

        if (s.check_error) {
            cerr << RED
                 << "Error: Could not check APT updates. "
                 << "Check /tmp/zupd.log"
                 << RESET << "\n";
            return 1;
        }

        if (!s.any()) {
            cout << "\n" << GREEN << "System is up to date!" << RESET << "\n";
            return 0;
        }

        if (fullupdate) {
            cout << YELLOW << "FULL UPDATE MODE" << RESET << "\n";
            sleep(1);
        }

        updateOk = aptUpdate(s);
    }
    else if (pm == "zypper") {
        UpdateStatus s = zypperCheckUpdates();

        if (s.check_error) {
            cerr << RED
                 << "Error: Could not check Zypper updates. "
                 << "Check /tmp/zupd.log and /tmp/zupd_patchcheck.log"
                 << RESET << "\n";
            return 1;
        }

        if (!s.any()) {
            cout << "\n" << GREEN << "System is up to date!" << RESET << "\n";
            return 0;
        }

        if (s.zypper_dup) {
            cout << YELLOW << "FULL UPDATE MODE (dup)" << RESET << "\n";
            sleep(1);
        }

        updateOk = zypperUpdate(s);
    }
    else if (pm == "dnf") {
        UpdateStatus s = dnfCheckUpdates();

        if (s.check_error) {
            cerr << RED
                 << "Error: Could not check DNF updates. "
                 << "Check /tmp/zupd.log"
                 << RESET << "\n";
            return 1;
        }

        if (!s.any()) {
            cout << "\n" << GREEN << "System is up to date!" << RESET << "\n";
            return 0;
        }

        if (fullupdate) {
            cout << YELLOW << "FULL UPDATE MODE (distro-sync)" << RESET << "\n";
            sleep(1);
        }

        updateOk = dnfUpdate(s);
    }

    if (g_interrupted) return 130;
    return updateOk ? 0 : 1;
}
