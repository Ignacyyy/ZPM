#include "main.h"
#include "progressbar.h"
#include <functional>

using namespace std;

// ─── zmienne globalne ─────────────────────────────────────────────────────────
bool hasflatpak = (system("command -v flatpak >/dev/null 2>&1") == 0);
bool hassnap    = (system("command -v snap    >/dev/null 2>&1") == 0);
bool help       = false;
bool version    = false;

// ─── Step musi być poza funkcją żeby działało push_back z lambdą ─────────────
struct Step {
    string           label;
    function<void()> action;
};

// ─── wiadomość pomocy ─────────────────────────────────────────────────────────
void helpmessage(const char* progName) {
    cout << RED << "Usage: " << RESET << progName
         << " [options]  or  zpm clean [options]\n\n";
    cout << RED << "Options:\n" << RESET;
    cout << "  --version, -v  Show version information\n";
    cout << "  --help,    -h  Show this help message\n";
}

// ─── wiadomość wersji ─────────────────────────────────────────────────────────
void versionmessage() {
    cout << RED << "zclean component version: " << zpm_version::version() << " of ZPM\n" << RESET;
    cout << "https://github.com/Zielina-Konrad-productions/ZPM\n";
    cout << "Copyright (c) 2026 Ignacyyy & Ry3ball\nLicense: MIT\n";
}

// ─── główna funkcja ───────────────────────────────────────────────────────────
void clean() {
    vector<Step> steps;

    steps.push_back({"APT: autoremove + autoclean", [](){
        system("echo ----apt_cleaning---- >> /tmp/zclean.log");
        system("apt-get autoremove -y >> /tmp/zclean.log 2>&1");
        system("apt-get autoclean   >> /tmp/zclean.log 2>&1");
    }});

    if (hasflatpak) {
        steps.push_back({"Flatpak: remove unused", [](){
            system("echo ----flatpak_cleaning---- >> /tmp/zclean.log");
            system("flatpak uninstall --unused -y >> /tmp/zclean.log 2>&1");
            system("rm -rf /var/tmp/flatpak-cache-* >> /tmp/zclean.log 2>&1");
        }});
    }

    if (hassnap) {
        steps.push_back({"Snap: remove disabled revisions", [](){
            system("echo ----snap_cleaning---- >> /tmp/zclean.log");
            system("snap list --all 2>/dev/null | awk '/disabled/{print $1, $3}' "
                   "| while read name rev; do snap remove \"$name\" --revision=\"$rev\"; done "
                   ">> /tmp/zclean.log 2>&1");
            system("rm -rf /var/lib/snapd/cache/* >> /tmp/zclean.log 2>&1");
        }});
    }

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

void info(){
    cout << RED << "cleaning system...." << RESET << endl;
    cout << "" << endl;
    
}

// ─── main ─────────────────────────────────────────────────────────────────────
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

    info();

    clean();
    return 0;
}