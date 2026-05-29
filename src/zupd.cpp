#include "main.h"

using namespace std;

//zmienne globalne-------------------------------------------------------------
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

    pclose(pipe);
    return result;
}

//wiadomosc pomocy
void helpmessage(const char* progName) {
    cout << RED << "Usage: " << RESET << progName << " [options]" << " or zpm upd/update [options]\n\n";
    cout << RED << "Options:" << RESET << endl;
    cout << "  --full, -f     Perform a full system upgrade (dist-upgrade)" << endl;
    cout << "  -r             Reboot the system after update" << endl;
    cout << "  -s             Shutdown the system after update" << endl;
    cout << "  --yes, -y      Automatic system update" << endl;
    cout << "  --help, -h     Show this help message" << endl;
    cout << "  --version, -v  Show version information" << endl;
}

//wiadomosc wersji
void versionmessage() {
    cout << RED << "zupd component version: v" << zpm_version::version() << " of ZPM\n" << RESET;
    cout << "https://github.com/Zielina-Konrad-productions/ZPM" << endl;
    cout << "Copyright (c) 2026 Ignacyyy & Ry3ball " << endl;
    cout << "License: MIT" << endl;
}

//wiadomosc system i repo
void repo() {

    //zmienne
    bool hasflatpak = (system("command -v flatpak >/dev/null 2>&1") == 0);
    bool hassnap = (system("command -v snap >/dev/null 2>&1") == 0);

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

// Struktura przechowująca stan aktualizacji
struct UpdateStatus {
    bool apt      = false;
    bool flatpak  = false;
    bool snap     = false;
    bool hasflatpak = false;
    bool hassnap    = false;

    bool any() const { return apt || flatpak || snap; }
};

// Jedno miejsce gdzie robimy apt-get update i sprawdzamy wszystkie systemy
UpdateStatus checkUpdates() {
    UpdateStatus s;

    s.hasflatpak = (system("command -v flatpak >/dev/null 2>&1") == 0);
    s.hassnap    = (system("command -v snap >/dev/null 2>&1") == 0);

    cout << "" << endl;
    cout << YELLOW << "[*] Refreshing package cache..." << RESET << endl;
    system("apt-get update -qq 2>/dev/null");
    s.apt = (system("apt-get dist-upgrade -s 2>/dev/null | grep -q '^Inst '") == 0);

    if (s.hasflatpak)
        s.flatpak = (system("flatpak remote-ls --updates 2>/dev/null | grep -q .") == 0);

    if (s.hassnap)
        s.snap = (system("snap refresh --list 2>/dev/null | grep -qvE '^(Name|All snaps)'") == 0);

    return s;
}

//aktualizacja — przyjmuje gotowy stan, nie liczy go ponownie
void update(const UpdateStatus& status) {
    cout << RED << "\nPackages to update (APT):\n" << RESET;
    string aptCmd = "apt-get dist-upgrade -s 2>/dev/null | grep '^Inst ' | awk '{print \"" + YELLOW + "[+] " + RESET + "\" $2}'";
    system(aptCmd.c_str());

    if (status.hasflatpak && status.flatpak) {
        cout << RED << "\nPackages to update (Flatpak):\n" << RESET;
        string flatpakCmd = "flatpak remote-ls --updates --columns=name 2>/dev/null | awk '{print \"" + YELLOW + "[+] " + RESET + "\" $0}'";
        system(flatpakCmd.c_str());
    }

    if (status.hassnap && status.snap) {
        cout << RED << "\nPackages to update (Snap):\n" << RESET;
        string snapCmd = "snap refresh --list 2>/dev/null | grep -vE '^(Name|All snaps)' | awk '{print \"" + YELLOW + "[+] " + RESET + "\" $1}'";
        system(snapCmd.c_str());
    }

    if (!yes) {
        cout << "\n" << YELLOW << "Proceed with update?" << RESET << " [y/n]: ";
        if (!(cin >> ans)) {
            ans = "x";
        }
    }

    if (yes || ans == "y" || ans == "yes") {
        bool updatedone = true;

        // DYNAMICZNE LICZENIE KROKÓW
        int totalSteps = 2; // przygotowanie + czyszczenie zawsze są
        if (status.apt) totalSteps++;
        if (status.hasflatpak && status.flatpak) totalSteps++;
        if (status.hassnap && status.snap) totalSteps++;

        int currentStep = 0;
        auto updateProgress = [&](const string& message) {
            float percent = (static_cast<float>(currentStep) / totalSteps) * 100.0f;
            string stepInfo = to_string(currentStep) + "/" + to_string(totalSteps) + " | " + message;
            progressbar(percent, stepInfo);
        };

        // KROK 1: Przygotowanie
        currentStep++;
        updateProgress("Preparing system...");
        system("echo -----checking_system_consistency----- > /tmp/zupd.log");
        system("DEBIAN_FRONTEND=noninteractive dpkg --configure -a >> /tmp/zupd.log 2>&1");
        sleep(1);

        // KROK: APT
        if (status.apt) {
            currentStep++;
            if (fullupdate) {
                updateProgress("Performing FULL APT upgrade...");
                system("echo -----performing_full_APT_upgrade----- >> /tmp/zupd.log");
                if (system("DEBIAN_FRONTEND=noninteractive apt-get dist-upgrade -y "
                    "--with-new-pkgs "
                    "-o APT::Get::Always-Include-Phased-Updates=true "
                    "-o Dpkg::Options::=\"--force-confdef\" "
                    "-o Dpkg::Options::=\"--force-confold\" >> /tmp/zupd.log 2>&1") != 0) {
                    updatedone = false;
                    }
            } else {
                updateProgress("Updating APT packages...");
                system("echo -----updating_APT----- >> /tmp/zupd.log");
                if (system("DEBIAN_FRONTEND=noninteractive apt-get dist-upgrade -y "
                    "-o Dpkg::Options::=\"--force-confdef\" "
                    "-o Dpkg::Options::=\"--force-confold\" >> /tmp/zupd.log 2>&1") != 0) {
                    updatedone = false;
                    }
            }
        }

        // KROK: Flatpak
        if (status.hasflatpak && status.flatpak) {
            currentStep++;
            updateProgress("Updating Flatpak remotes...");
            system("echo ----updating_flatpak---- >> /tmp/zupd.log");
            if (system("flatpak update -y >> /tmp/zupd.log 2>&1") != 0) {
                updatedone = false;
            }
        }

        // KROK: Snap
        if (status.hassnap && status.snap) {
            currentStep++;
            updateProgress("Updating Snap packages...");
            system("echo ----updating_snap---- >> /tmp/zupd.log");
            if (system("snap refresh >> /tmp/zupd.log 2>&1") != 0) {
                updatedone = false;
            }
        }

        // KROK OSTATNI: Czyszczenie
        currentStep++;
        updateProgress("Cleaning up cache and unused packages...");
        system("echo ----cleaning---- >> /tmp/zupd.log");
        system("apt-get autoremove -y >> /tmp/zupd.log 2>&1");
        system("apt-get autoclean >> /tmp/zupd.log 2>&1");

        if (status.hasflatpak) {
            system("flatpak uninstall --unused -y >> /tmp/zupd.log 2>&1");
            system("rm -rf /var/tmp/flatpak-cache-* >> /tmp/zupd.log 2>&1");
        }

        if (status.hassnap) {
            system("snap list --all 2>/dev/null | awk '/disabled/{print $1, $3}' | while read name rev; do snap remove \"$name\" --revision=\"$rev\"; done >> /tmp/zupd.log 2>&1");
            system("rm -rf /var/lib/snapd/cache/* >> /tmp/zupd.log 2>&1");
        }

        // Finał
        if (updatedone) {
            progressbar_finish("DONE!");
            cout << YELLOW << "[RAPORT]" << RESET << " /tmp/zupd.log" << endl;

            if (reboot) {
                cout << YELLOW << "[*] Rebooting system in 3 seconds..." << RESET << endl;
                system("sleep 3 && reboot");
            } else if (shutdown) {
                cout << YELLOW << "[*] Shutting down system in 3 seconds..." << RESET << endl;
                system("sleep 3 && shutdown -h now");
            }
        } else {
            progressbar_finish("ERROR!");
            cout << RED << "ERROR," << RESET << " check /tmp/zupd.log for details." << endl;
        }
    } else {
        cout << YELLOW << "[*] Update cancelled by user." << endl;
    }
}
//koniec funkcji---------------------------------------------------------------

//main--------------------------------------------------------------------------
int main(int argc, char* argv[]) {

    //sprawdzanie aktualizacji komponentu ZPM
    zpm_update::checkForUpdates();

    //pętla do argumentów
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if      (arg == "--full"     || arg == "-f") fullupdate = true;
        else if (arg == "--reboot"   || arg == "-r") reboot     = true;
        else if (arg == "--shutdown" || arg == "-s") shutdown   = true;
        else if (arg == "--help"     || arg == "-h") help       = true;
        else if (arg == "--version"  || arg == "-v") version    = true;
        else if (arg == "--yes"      || arg == "-y") yes        = true;
    }

    //poprawność argumentów
    if ((reboot && shutdown) ||
        (help && reboot) || (help && shutdown) ||
        (version && yes) || (help && yes) ||
        (version && reboot) || (version && shutdown) ||
        (fullupdate && version) || (fullupdate && help)) {
        cout << RED << "Error: -r and -s are mutually exclusive. --help and --version cannot be combined with other options." << RESET << endl;

    return 1;
        }

        if (version && help) {
            cout << YELLOW << "--version" << RESET << endl;
            versionmessage();
            cout << "" << endl;
            cout << YELLOW << "--help" << RESET << endl;
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

        //sprawdzanie sudo
        if (geteuid() != 0) {
            cout << RED << "Run with sudo!\n" << RESET;
            return 1;
        }

        //repozytoria i system
        repo();

        // Sprawdzamy aktualizacje RAZ — wynik trafia do obu funkcji
        UpdateStatus status = checkUpdates();

        if (status.any()) {
            if (fullupdate) {
                cout << YELLOW << "FULL UPDATE MODE" << RESET << endl;
                sleep(1);
            }
            update(status);
        } else {
            cout << "\n" << RED << "System is up to date!" << RESET << endl;
        }

        return 0;
}
