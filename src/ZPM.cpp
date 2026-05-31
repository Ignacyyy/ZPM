#include "main.h"

using namespace std;

void show_version() {
    cout << RED << "zpm component version: v" << zpm_version::version() << "\n" << RESET;
    cout << "https://github.com/Zielina-Konrad-productions/ZPM" << endl;
    cout << "Copyright (c) 2026 Ignacyyy & Ry3ball " << endl;
    cout << "License: MIT" << endl;
}

void show_help() {
    cout << RED << "Usage: " << RESET << "zpm <command> [options]" << endl;
    cout << RED << "Commands:" << RESET << endl;
    cout << "  update, upd     Perform a system upgrade (zupd)" << endl;
    cout << "  upgrade, upgr   Upgrade ZPM itself (zupgr)" << endl;
    cout << "  install, inst   Install package (zinst)" << endl;
    cout << "  remove, rm      Remove package (zrm)" << endl;
    cout << "  list, ls        List installed packages (zlist)" << endl;
    cout << "  search          Search for package (zsearch)" << endl;
    cout << "  clean           Clean system cache (zclean)" << endl;
    cout << "  info            Package information (zinfo)" << endl;
    cout << "  uninstall       Uninstall ZPM (zuninstall)" << endl;
    cout << "  run             Run programs using ZPM" << endl;
    cout << endl;
    cout << RED << "Options:" << RESET << endl;
    cout << "  --help, -h      Show this help message" << endl;
    cout << "  --version, -v   Show version information" << endl;
    cout << "" << endl;
    cout << BOLD << "type zhelp, for detailed info about ZPM." << RESET << endl;
}

int main(int argc, char* argv[]) {

    int cmd_index = -1;
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg != "--help" && arg != "-h" && arg != "--version" && arg != "-v") {
            cmd_index = i;
            break;
        }
    }


    if (cmd_index != -1) {
        string cmd = argv[cmd_index];
        string target;


        if (cmd == "install" || cmd == "inst") target = "zinst";
        else if (cmd == "remove" || cmd == "rm") target = "zrm";
        else if (cmd == "update" || cmd == "upd") target = "zupd";
        else if (cmd == "upgrade" || cmd == "upgr") target = "zupgr";
        else if (cmd == "list" || cmd == "ls") target = "zlist";
        else if (cmd == "search") target = "zsearch";
        else if (cmd == "clean") target = "zclean";
        else if (cmd == "info") target = "zinfo";
        else if (cmd == "uninstall") target = "zuninstall";
        else if (cmd == "help") target = "zhelp";
        else if (cmd == "run") target = "zrun";
        else {
            cerr << "unknown command: " << cmd << "\n";
            cerr << "run 'zpm --help' to list all commands.\n";
            return 1;
        }


        string full_cmd = target;
        for (int i = 1; i < argc; i++) {
            if (i == cmd_index) continue;
            full_cmd += " ";
            full_cmd += argv[i];
        }

        int result = system(full_cmd.c_str());
        return (result != 0);
    }


    bool version = false;
    bool help = false;
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "--version" || arg == "-v") version = true;
        if (arg == "--help" || arg == "-h") help = true;
    }


    if (version && help) {
        cout << YELLOW << "--version" << RESET << endl;
        show_version();
        cout << "" << endl;
        cout << YELLOW << "--help" << endl;
        void show_help();
        return 0;
    }

    if (version) {
        show_version();
        return 0;
    }

    if (help) {
        show_help();
        return 0;
    }


    cout << "no actions detected" << endl;
    return 0;
}
