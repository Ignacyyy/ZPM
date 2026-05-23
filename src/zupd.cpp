#include "main.h"

using namespace std;

//zmienne globalne-------------------------------------------------------------
bool hasflatpak = (system("command -v flatpak >/dev/null 2>&1") == 0);
bool hassnap = (system("command -v snap >/dev/null 2>&1") == 0);
bool reboot = false;
bool shutdown = false;
bool help = false;
bool version = false;
bool yes = false;
bool fullupdate = false;
string ans;

//koniec zmiennych globalnych----------------------------------------------------


//funckcje pomocnicze------------------------------------------------------------

// Nowa funkcja, która przechwytuje tekst wyjściowy z komend terminala
string execCommand(const char* cmd) {
    array<char, 128> buffer;
    string result;
    
    FILE* pipe = popen(cmd, "r");
    if (!pipe) {
        return "";
    }
    
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }
    
    pclose(pipe); // Klasyczne, bezpieczne zamknięcie strumienia
    return result;
}

//wiadomosc pomocy
void helpmessage(const char* progName) {
    cout << RED << "Usage: " << RESET << progName << " [options]" << " or zpm upd/update [options]"  "\n\n";
    cout << RED << "Options:" << RESET << endl;
    cout << "  --full. -f     Perform a full system upgrade (dist-upgrade)" << endl;
    cout << "  -r        Reboot the system after update" << endl;
    cout << "  -s        Shutdown the system after update" << endl;
    cout << "  --yes, -y    Automatic system update" << endl;
    cout << "  --help, -h    Show this help message" << endl;
    cout << "  --version, -v    Show version information" << endl;
}

//wiadomosc wersji
void versionmessage() {
    cout << RED << "zupd component version: " << zpm_version::version() << " of ZPM\n" << RESET;
    cout << "https://github.com/Zielina-Konrad-productions/ZPM" << endl;
    cout << "Copyright (c) 2026 Ignacyyy & Ry3ball " << endl;
    cout << "License: MIT" << endl;
}

//wiadomosc system i repo
void repo() {
    cout << YELLOW << "[SYS] " << RESET << flush;
    system("lsb_release -ds 2>/dev/null || cat /etc/debian_version");

    cout << "\n" << YELLOW << "[D]" << RESET << GREEN << " APT Repositories:\n" << RESET;
    string repoCmd =
    "{ "
    "grep -rh '^deb ' /etc/apt/sources.list /etc/apt/sources.list.d/ 2>/dev/null; "
    "grep -rh '^URIs:' /etc/apt/sources.list.d/ /usr/lib/apt/sources.list.d/ 2>/dev/null"
    "  | sed 's/^URIs:[[:space:]]*/deb /'; "
    "} | sort -u | grep -v '^[[:space:]]*$'"
    "  | sed 's|^|" + YELLOW + "- " + RESET + "|'";
    system(repoCmd.c_str());
    
    {
        string checkCmd =
            "{ grep -rh '^deb ' /etc/apt/sources.list /etc/apt/sources.list.d/ 2>/dev/null; "
            "grep -rh '^URIs:' /etc/apt/sources.list.d/ 2>/dev/null; }"
            " | grep -qv '^[[:space:]]*$'";
        if (system(checkCmd.c_str()) != 0)
            cout << YELLOW << "- (no repos found in standard locations)" << RESET << "\n";
    }

    if (hasflatpak) {
        cout << "\n" << YELLOW << "[F]" << RESET << GREEN << " Flatpak Remotes:\n" << RESET;
        system("flatpak remotes --columns=name 2>/dev/null | sed 's/^/\033[33m- \033[0m/'");
    }
    if (hassnap) {
        cout << "\n" << YELLOW << "[S]" << RESET << GREEN << " Snap is available.\n" << RESET;
    }
}

//czy ma aktualizacje
bool hasupdates() {
    bool flatpakupdates = false;
    bool snapupdates = false;

    // Stabilne sprawdzenie APT
    bool aptupdates = (system("apt-get -s upgrade 2>/dev/null | grep -q '^Inst '") == 0);

    // czy flatpak ma aktualizacje
    if (hasflatpak)
        flatpakupdates = system("flatpak remote-ls --updates 2>/dev/null | grep -q .") == 0;

    // czy snap ma aktualizacje
    if (hassnap)
        snapupdates = system("snap refresh --list 2>/dev/null | grep -qvE '^(Name|All snaps)'") == 0;

    return aptupdates || flatpakupdates || snapupdates;
}

//aktualizacja
void update() {
    cout << RED << "\nPackages to update (APT):\n" << RESET;

    // Pakiety do aktualizacji APT
    string aptCmd = "apt-get -s upgrade 2>/dev/null | grep '^Inst ' | awk '{print \"" + YELLOW + "[+] " + RESET + "\" $2}'";
    system(aptCmd.c_str());

    //pakiety do aktualizacji FLATPAK
    if (hasflatpak) {
        cout << RED << "\nPackages to update (Flatpak):\n" << RESET;
        string flatpakCmd = "flatpak remote-ls --updates --columns=name 2>/dev/null | awk '{print \"" + YELLOW + "[+] " + RESET + "\" $0}'";
        system(flatpakCmd.c_str());
    } 
        
    //pakiety do aktualizacji SNAP
    if (hassnap) {
        cout << RED << "\nPackages to update (Snap):\n" << RESET;
        string snapCmd = "snap refresh --list 2>/dev/null | grep -vE '^(Name|All snaps)' | awk '{print \"" + YELLOW + "[+] " + RESET + "\" $1}'";
        system(snapCmd.c_str());
    }
    //autmatyczne aktualizacje
    if (!yes){
        cout << "\n" << YELLOW << "Proceed with update?" << RESET << " [y/n]: ";
        if (!(cin >> ans)) {
            ans = "x";
        }
    }
    //rozpoczecie aktualizacji-----------------------------------------------------
    if (yes || ans == "y" || ans == "yes") {
        //zakladamy ze aktualizacja zadziala
        bool updatedone = true;

        //system ratunkowy, po cichu, dla stabilnosci
        system("echo -----checking_system_consistency----- >> /tmp/zupd.log");
        system("dpkg --configure -a >> /tmp/zupd.log 2>&1");

        //poczatek progress bara
        progressbar(0.0f,   "0/6 | starting...");

        //odswierzenie repozytoriow
        progressbar(10.0f,  "1/6 | refreshing repositories...");
        system("echo -----refreshing_repositories----- >> /tmp/zupd.log");
        int stanrepo = system("apt-get update >> /tmp/zupd.log 2>&1");
        if (stanrepo != 0) {
            updatedone = false;
        }

        //aktualizacja APT (zwykła lub FULL)
        int stanapt;
        if (fullupdate) {
            progressbar(25.0f,  "2/6 | performing FULL upgrade...");
            system("echo -----performing_full_APT_upgrade----- >> /tmp/zupd.log");
            stanapt = system("DEBIAN_FRONTEND=noninteractive apt-get dist-upgrade -y -o Dpkg::Options::=\"--force-confdef\" -o Dpkg::Options::=\"--force-confold\" >> /tmp/zupd.log 2>&1");
        } else {
            progressbar(25.0f,  "2/6 | updating APT...");
            system("echo -----updating_APT----- >> /tmp/zupd.log");
            stanapt = system("DEBIAN_FRONTEND=noninteractive apt-get upgrade -y -o Dpkg::Options::=\"--force-confdef\" -o Dpkg::Options::=\"--force-confold\" >> /tmp/zupd.log 2>&1");
        }
        
        if (stanapt != 0) {
            updatedone = false;
        }

        //aktualizacja flatpak 
        if (hasflatpak) {
            progressbar(50.0f,  "3/6 | updating Flatpak...");
            system("echo ----updating_flatpak---- >> /tmp/zupd.log");
            int stanflatpak = system("flatpak update -y >> /tmp/zupd.log 2>&1");
            if (stanflatpak != 0) {
                updatedone = false;
            }
        }

        //aktualizacja snap
        if (hassnap) {
            progressbar(75.0f,  "4/6 | updating Snap...");
            system("echo ----updating_snap---- >> /tmp/zupd.log");
            int stansnap = system("snap refresh >> /tmp/zupd.log 2>&1");
            if (stansnap != 0) {
                updatedone = false;
            }
        }

        //sprzątanie po aktualizacji------------------------------------
        progressbar(90.0f,  "5/6 | cleaning...");
        system("echo ----cleaning---- >> /tmp/zupd.log");
        system("apt-get autoremove -y >> /tmp/zupd.log 2>&1");
        system("apt-get autoclean >> /tmp/zupd.log 2>&1");

        if (hasflatpak) {
            system("flatpak uninstall --unused -y >> /tmp/zupd.log 2>&1");
            system("rm -rf /var/tmp/flatpak-cache-* >> /tmp/zupd.log 2>&1");
        }
        
        if (hassnap) {
            system("snap list --all 2>/dev/null | awk '/disabled/{print $1, $3}' | while read name rev; do snap remove \"$name\" --revision=\"$rev\"; done >> /tmp/zupd.log 2>&1");
            system("rm -rf /var/lib/snapd/cache/* >> /tmp/zupd.log 2>&1");
        }

        //koniec aktualizacji i koniec progressbara-------------------------
        if (updatedone) {
            progressbar_finish("6/6 | DONE!");
            cout << YELLOW << "[RAPORT]" << RESET << " /tmp/zupd.log" << endl;

            if (reboot) {
                cout << YELLOW << "[*] Rebooting system in 3 seconds..." << RESET << endl;
                system("sleep 3 && reboot");
            }
            else if (shutdown) {
                cout << YELLOW << "[*] Shutting down system in 3 seconds..." << RESET << endl;
                system("sleep 3 && shutdown -h now");
            }
        }
        else {
            progressbar_finish("ERROR!"); 
            cout << RED << "ERROR," << RESET << " check /tmp/zupd.log for details." << endl;
        }
    }
    else {
        cout << YELLOW << "[*] Update cancelled by user." << RESET << endl;
    }
}
//koniec funkcji---------------------------------------------------------------


//main--------------------------------------------------------------------------
int main (int argc, char* argv[]){

    //sprawdzanie aktualizacji komponentu ZPM
    zpm_update::checkForUpdates();

    //pętla do argumentów--------------------------------------------------------
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "--full" || arg == "-f") fullupdate = true;
        else if (arg == "--reboot" || arg == "-r") reboot = true;
        else if (arg == "--shutdown" || arg == "-s") shutdown = true;
        else if (arg == "--help" || arg == "-h") help = true;
        else if (arg == "--version" || arg == "-v") version = true;
        else if (arg == "--yes" || arg == "-y") yes = true;
    }
    //koniec pętli---------------------------------------------------------------

    //argumenty------------------------------------------------------------------

    //poprawosc argumentow

    if ((reboot && shutdown) || (help && reboot) || (help && shutdown) || (version && yes) || (help && yes) || (version && reboot) || (version && shutdown) || (fullupdate && version) || (fullupdate && help)){
        cout << RED << "Error: -r and -s are mutually exclusive. " << "--help and --version cannot be combined with other options." << RESET << endl;
        return 1;
    }

    //informacje o wersji i wyswietlanie pomocy
    if (version && help){
        cout << YELLOW <<"--version" << RESET << endl;
        versionmessage();
        cout << "" << endl;
        cout << YELLOW << "--help" << RESET << endl;
        helpmessage(argv[0]);
        return 0;
    }

    //informacje o wersji
    if (version){
        versionmessage();
        return 0;
    }

    //wyswieitlanie pomocy
    if (help){
        helpmessage(argv[0]);
        return 0;
    }

    //sprawdzanie sudo, dopiero teraz, bo wiadomosci help i version
    if (geteuid() != 0) {
        cout << RED << "Run with sudo!\n" << RESET;
        return 1;
    }
    //potem bedzie wiecej argumentow narazie koniec----------------------------------
    
    //podstawowe ui------------------------------------------------------------------

    //repozytoria i system, flatpak, snap i apt
    repo();
    
    //test
    system("apt-get update -y > /dev/null 2>&1");
    
    // Zapisujemy wynik do zmiennej, żeby nie odpytywać sieci/dysku dwa razy!
    bool systemHasUpdates = hasupdates();

    //spawdzane czy są dostepne aktualizacje
    if (systemHasUpdates == false){
        cout << "\n" << RED << "System is up to date!" << RESET << endl;
    } 
    //koniec podstawowego ui----------------------------------------------------------

    //aktualizacja-----------------------------------------------------
    if(systemHasUpdates){  
        if(fullupdate){
            cout << YELLOW << "FULL UPDATE MODE" << RESET << endl;
            sleep (1);
        }
        update();
    }
    //koniec aktualizacji-----------------------------------------------

    return 0;
}