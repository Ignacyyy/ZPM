#include "main.h"

using namespace std;

// Komunikat pomocy
void helpmessage(const char* progName) {
    cout << RED << "Usage: " << RESET << progName << " [options] or zpm uninstall [options]\n";
    cout << RED << "Options:\n" << RESET;
    cout << "  -h, --help           Show help\n";
    cout << "  -v, --version        Show version\n";
}

// Komunikat wersji
void versionmessage() {
    cout << RED << "zupgr component version: v" << zpm_version::version() << " of ZPM\n" << RESET;
    cout << "https://github.com/Zielina-Konrad-productions/ZPM" << endl;
    cout << "Copyright (c) 2026 Ignacyyy & Ry3ball" << endl;
    cout << "License: MIT" << endl;
}

static void run(const string& cmd) {
    system(cmd.c_str());
}

int main(int argc, char* argv[]) {

    bool showHelp    = false;
    bool showVersion = false;

    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        if      (arg == "--version" || arg == "-v") showVersion = true;
        else if (arg == "--help"    || arg == "-h") showHelp    = true;
    }

    if (showVersion && showHelp) {
        cout << YELLOW << "--version" << RESET << endl;
        versionmessage();
        cout << "\n" << YELLOW << "--help" << RESET << endl;
        helpmessage(argv[0]);
        return 0;
    }

    if (showVersion) { versionmessage(); return 0; }
    if (showHelp)    { helpmessage(argv[0]); return 0; }

    if (geteuid() != 0) {
        cout << RED << "Run with sudo!\n" << RESET;
        return 1;
    }

    cout << RED << "ZPM Uninstall program," << RESET;
    cout << " continue? [y/n]: ";

    string answer;
    getline(cin, answer);

    if (answer != "y" && answer != "Y") {
        cout << YELLOW << "Uninstall canceled.\n" << RESET;
        return 0;
    }
    cout << "\n";
    cout << RED << "Uninstalling...\n" << RESET;
    progressbar_start(0.0f, "0/3 | Starting...");

    vector<string> files = {
        "zhelp",
        "zinst",
        "zr",
        "zs",
        "zuninstall",
        "zupgr",
        "zclean",
        "zinfo",
        "zlist",
        "zrm",
        "zsearch",
        "zupd",
        "ZPM",
        "zpm",
        "zrun",
        "zhome"
    };

    vector<string> dirs = {
        "/usr/bin/",
        "/usr/local/bin/",
        "/opt/ZPM/"
    };

    for (const auto& dir : dirs) {
        for (const auto& file : files) {
            string path = dir + file;
            string cmd = "[ -f " + path + " ] && rm -f " + path + " 2>/dev/null";
            run(cmd);
        }
    }
    progressbar_start(60.0f, "1/3 | Uninstalling...");
    run("echo -------------------uninstalling ZPM---------------------- > /tmp/zuninstall.log");
    run("rm -rf /opt/ZPM 2>/dev/null >> /tmp/zupgr.log");
    run("rm -rf /opt/ZPM 2>/dev/null >> /tmp/zupgr.log");
    run("rm -f /etc/profile.d/ZPM.sh /etc/profile.d/ZPM.sh 2>/dev/null >> /tmp/zuninstall.log");
    run("echo -------------------cleaning---------------------- >> /tmp/zuninstall.log");
    progressbar_update(90.0f, "2/3 | Cleaning...");
    progressbar_finish("3/3 | DONE!");
    cout << YELLOW << "[RAPORT]" << RESET << " /tmp/zuninstall.log" << endl;
    return 0;
}
