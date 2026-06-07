#include "main.h"

using namespace std;

//zmienne globalne---------------------------------------------------------------
bool help    = false;
bool version = false;
//koniec zmiennych globalnych----------------------------------------------------

//struktura step-----------------------------------------------------------------
struct Step {
    string           label;
    function<void()> action;
};
//koniec struktury step----------------------------------------------------------

//funkcje pomocnicze-------------------------------------------------------------

void helpmessage(const char* progName) {
    cout << RED << "Usage: " << RESET << progName
         << " [options] or zpm clean [options]\n";
    cout << RED << "Options:\n" << RESET;
    cout << "  --version, -v  Show version information\n";
    cout << "  --help,    -h  Show this help message\n";
}

void versionmessage() {
    cout << RED << "zclean component version: v" << zpm_version::version() << " of ZPM\n" << RESET;
    cout << "https://github.com/Zielina-Konrad-productions/ZPM\n";
    cout << "Copyright (c) 2026 Ignacyyy & Ry3ball\nLicense: MIT\n";
}

void info() {
    cout << RED << "cleaning system...." << RESET << "\n\n";
}

//-----------------------------------------------------------------------------
// Wspólna funkcja uruchamiająca kroki z paskiem postępu
//-----------------------------------------------------------------------------
void runSteps(vector<Step>& steps) {
    int total = static_cast<int>(steps.size());

    progressbar_start(0.0f, "0/" + to_string(total) + " | starting...");

    for (int i = 0; i < total; ++i) {
        float pct = (100.0f * i) / total;
        progressbar_update(pct, to_string(i + 1) + "/" + to_string(total)
                           + " | " + steps[i].label);
        steps[i].action();
    }

    progressbar_finish(to_string(total) + "/" + to_string(total) + " | DONE!");
    cout << YELLOW << "[RAPORT] " << RESET << "/tmp/zclean.log\n";
}

//=============================================================================
// APT clean
//=============================================================================
void aptClean() {
    bool hasflatpak = (system("command -v flatpak >/dev/null 2>&1") == 0);
    bool hassnap    = (system("command -v snap    >/dev/null 2>&1") == 0);

    vector<Step> steps;

    // KROK 1: Naprawa spójności systemu
    steps.push_back({"Preparing system...", [](){
        system("echo -----checking_system_consistency----- > /tmp/zclean.log");
        system("DEBIAN_FRONTEND=noninteractive dpkg --configure -a >> /tmp/zclean.log 2>&1");
    }});

    // KROK 2: APT
    steps.push_back({"APT: autoremove + autoclean", [](){
        system("echo ----apt_cleaning---- >> /tmp/zclean.log");
        system("apt-get autoremove -y >> /tmp/zclean.log 2>&1");
        system("apt-get autoclean    >> /tmp/zclean.log 2>&1");
    }});

    // KROK 3: Flatpak
    if (hasflatpak) {
        steps.push_back({"Flatpak: remove unused", [](){
            system("echo ----flatpak_cleaning---- >> /tmp/zclean.log");
            system("flatpak uninstall --unused -y    >> /tmp/zclean.log 2>&1");
            system("rm -rf /var/tmp/flatpak-cache-*  >> /tmp/zclean.log 2>&1");
        }});
    }

    // KROK 4: Snap
    if (hassnap) {
        steps.push_back({"Snap: remove disabled revisions", [](){
            system("echo ----snap_cleaning---- >> /tmp/zclean.log");
            system("snap list --all 2>/dev/null | awk '/disabled/{print $1, $3}' "
                   "| while read name rev; do snap remove \"$name\" --revision=\"$rev\"; done "
                   ">> /tmp/zclean.log 2>&1");
            system("rm -rf /var/lib/snapd/cache/* >> /tmp/zclean.log 2>&1");
        }});
    }

    runSteps(steps);
}

//=============================================================================
// ZYPPER clean
//=============================================================================
void zypperClean() {
    bool hasflatpak = (system("command -v flatpak >/dev/null 2>&1") == 0);
    bool hassnap    = (system("command -v snap    >/dev/null 2>&1") == 0);

    vector<Step> steps;

    // KROK 1: Naprawa bazy RPM
    steps.push_back({"Preparing system...", [](){
        system("echo -----checking_system_consistency----- > /tmp/zclean.log");
        system("rpm --rebuilddb >> /tmp/zclean.log 2>&1");
    }});

    // KROK 2: Zypper
    steps.push_back({"Zypper: clean all caches", [](){
        system("echo ----zypper_cleaning---- >> /tmp/zclean.log");
        system("zypper clean -a >> /tmp/zclean.log 2>&1");
    }});

    // KROK 3: Pakiety-sieroty (zypper nie ma autoremove, używamy packages-not-used)
    steps.push_back({"Zypper: remove unused packages", [](){
        system("echo ----zypper_unused---- >> /tmp/zclean.log");
        // zypper packages --unneeded — lista, potem usuwamy
        system("zypper packages --unneeded 2>/dev/null"
               " | awk -F'|' 'NR>4 && $1~/i/{print $3}'"
               " | xargs -r zypper remove -y >> /tmp/zclean.log 2>&1");
    }});

    // KROK 4: Flatpak
    if (hasflatpak) {
        steps.push_back({"Flatpak: remove unused", [](){
            system("echo ----flatpak_cleaning---- >> /tmp/zclean.log");
            system("flatpak uninstall --unused -y   >> /tmp/zclean.log 2>&1");
            system("rm -rf /var/tmp/flatpak-cache-* >> /tmp/zclean.log 2>&1");
        }});
    }

    // KROK 5: Snap
    if (hassnap) {
        steps.push_back({"Snap: remove disabled revisions", [](){
            system("echo ----snap_cleaning---- >> /tmp/zclean.log");
            system("snap list --all 2>/dev/null | awk '/disabled/{print $1, $3}' "
                   "| while read name rev; do snap remove \"$name\" --revision=\"$rev\"; done "
                   ">> /tmp/zclean.log 2>&1");
            system("rm -rf /var/lib/snapd/cache/* >> /tmp/zclean.log 2>&1");
        }});
    }

    runSteps(steps);
}

//=============================================================================
// DNF clean
//=============================================================================
void dnfClean() {
    bool hasflatpak = (system("command -v flatpak >/dev/null 2>&1") == 0);
    bool hassnap    = (system("command -v snap    >/dev/null 2>&1") == 0);

    vector<Step> steps;

    // KROK 1: Naprawa bazy RPM
    steps.push_back({"Preparing system...", [](){
        system("echo -----checking_system_consistency----- > /tmp/zclean.log");
        system("rpm --rebuilddb >> /tmp/zclean.log 2>&1");
    }});

    // KROK 2: DNF autoremove
    steps.push_back({"DNF: autoremove", [](){
        system("echo ----dnf_autoremove---- >> /tmp/zclean.log");
        system("dnf autoremove -y >> /tmp/zclean.log 2>&1");
    }});

    // KROK 3: DNF clean
    steps.push_back({"DNF: clean packages + metadata", [](){
        system("echo ----dnf_clean---- >> /tmp/zclean.log");
        system("dnf clean packages  >> /tmp/zclean.log 2>&1");
        system("dnf clean metadata  >> /tmp/zclean.log 2>&1");
        system("dnf clean dbcache   >> /tmp/zclean.log 2>&1");
    }});

    // KROK 4: Flatpak
    if (hasflatpak) {
        steps.push_back({"Flatpak: remove unused", [](){
            system("echo ----flatpak_cleaning---- >> /tmp/zclean.log");
            system("flatpak uninstall --unused -y   >> /tmp/zclean.log 2>&1");
            system("rm -rf /var/tmp/flatpak-cache-* >> /tmp/zclean.log 2>&1");
        }});
    }

    // KROK 5: Snap
    if (hassnap) {
        steps.push_back({"Snap: remove disabled revisions", [](){
            system("echo ----snap_cleaning---- >> /tmp/zclean.log");
            system("snap list --all 2>/dev/null | awk '/disabled/{print $1, $3}' "
                   "| while read name rev; do snap remove \"$name\" --revision=\"$rev\"; done "
                   ">> /tmp/zclean.log 2>&1");
            system("rm -rf /var/lib/snapd/cache/* >> /tmp/zclean.log 2>&1");
        }});
    }

    runSteps(steps);
}

//koniec funkcji----------------------------------------------------------------

//main--------------------------------------------------------------------------
int main(int argc, char* argv[]) {

    zpm_update::checkForUpdates();

    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        if      (arg == "--help"    || arg == "-h") help    = true;
        else if (arg == "--version" || arg == "-v") version = true;
    }

    if (help && version) {
        cout << YELLOW << "--version" << RESET << "\n";
        versionmessage();
        cout << "\n" << YELLOW << "--help" << RESET << "\n";
        helpmessage(argv[0]);
        return 0;
    }

    if (help)    { helpmessage(argv[0]); return 0; }
    if (version) { versionmessage();     return 0; }

    if (geteuid() != 0) {
        cout << RED << "Run with sudo!\n" << RESET;
        return 1;
    }

    string pm = get_package_manager();

    if (pm == "unknown") {
        cout << RED << "Error: Could not detect a supported package manager "
             << "(apt / zypper / dnf).\n" << RESET;
        return 1;
    }

    info();

    if      (pm == "apt")    aptClean();
    else if (pm == "zypper") zypperClean();
    else if (pm == "dnf")    dnfClean();

    return 0;
}
