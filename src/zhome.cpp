#include "main.h"

void PAGE1() {
    // add return 0 when using !!!
    //PAGE 1 info
    std::cout << YELLOW << "Current PAGE:" << RESET << std::endl;
    std::cout << BOLD << "PAGE 1" << RESET;
    std::cout << " (ARM support and config information)" << std::endl;
   
   
    // ARM support + config info
    std::cout << "" << std::endl;
    std::cout << BOLD << RED << "ZPM Configuration:" << RESET << std::endl;
    std::cout << "  Config file: /opt/ZPM/zielina.conf" << std::endl;
    std::cout << "  Used for runtime behavior and update settings" << std::endl;
    std::cout << "Config editing:" << std::endl;
    std::cout << "  zhome" << std::endl;
    std::cout << "  zpm home" << std::endl;
    std::cout << "  --edit-config, -ed" << std::endl;
    std::cout << "  Opens /opt/ZPM/zielina.conf in the default terminal text editor" << std::endl;
    std::cout << std::endl;
    std::cout << BOLD << RED << "ARM support:" << RESET << std::endl;
    std::cout << "  ZPM supports ARM (aarch64 / armhf) systems" << std::endl;
    std::cout << "  ARM mode does NOT affect normal usage" << std::endl;
    std::cout << std::endl;
   
    //zhome page info
    std::cout << BOLD << "Type zhome -p2 / zpm home -p2 to see PAGE2." << RESET << std::endl;
}

void printZpmCommandRow(const std::string& command,
                        const std::string& alias,
                        const std::string& component,
                        const std::string& description) {
      const std::string aliasText = alias.empty() ? "-" : alias;

      std::cout << "  "
                << GREEN << std::left << std::setw(16) << command << RESET
                << CYAN << std::left << std::setw(13) << aliasText << RESET
                << YELLOW << std::left << std::setw(12) << component << RESET
                << description << std::endl;
}

void PAGE2(){
      // add return 0, when using !!!
      // PAGE 2 info
      std::cout << YELLOW << "Current PAGE:" << RESET << std::endl;
      std::cout << BOLD << "PAGE 2" << RESET;
      std::cout << " (zpm wrapper commands)" << std::endl;
      std::cout << "" << std::endl;

      // zpm wrapper commands
      std::cout << RED << "zpm wrapper commands:" << RESET << std::endl;
      std::cout << "  "
                << BOLD << std::left << std::setw(16) << "Command"
                << std::setw(13) << "Alias"
                << std::setw(12) << "Runs"
                << "Description" << RESET << std::endl;
      std::cout << "  " << std::string(76, '-') << std::endl;
      printZpmCommandRow("zpm home", "-", "zhome", "Open the ZPM home pages");
      printZpmCommandRow("zpm update", "zpm upd", "zupd", "Update system packages");
      printZpmCommandRow("zpm upgrade", "zpm upgr", "zupgr", "Upgrade ZPM itself");
      printZpmCommandRow("zpm install", "zpm inst", "zinst", "Install packages");
      printZpmCommandRow("zpm remove", "zpm rm", "zrm", "Remove packages");
      printZpmCommandRow("zpm list", "zpm ls", "zlist", "List installed packages");
      printZpmCommandRow("zpm search", "-", "zsearch", "Search for packages");
      printZpmCommandRow("zpm clean", "-", "zclean", "Clean package cache");
      printZpmCommandRow("zpm info", "-", "zinfo", "Show package information");
      printZpmCommandRow("zpm uninstall", "-", "zuninstall", "Uninstall ZPM");
      printZpmCommandRow("zpm run", "-", "zrun", "Run programs using ZPM");

      // global options
      std::cout << "\n" << RED << "Wrapper options:" << RESET << std::endl;
      std::cout << "  " << GREEN << std::left << std::setw(20) << "zpm --help, -h" << RESET
                << "Show wrapper help" << std::endl;
      std::cout << "  " << GREEN << std::left << std::setw(20) << "zpm --version, -v" << RESET
                << "Show wrapper version" << std::endl;

      // examples
      std::cout << "\n" << RED << "Examples:" << RESET << std::endl;
      std::cout << "  " << BOLD << "zpm install firefox" << RESET << "        install a package" << std::endl;
      std::cout << "  " << BOLD << "sudo zpm update -full -y" << RESET << "   full system update" << std::endl;
      std::cout << "  " << BOLD << "zpm remove vlc -p" << RESET << "           remove with purge option" << std::endl;
      std::cout << "  " << BOLD << "sudo zpm upgrade -ex" << RESET << "      experimental ZPM upgrade" << std::endl;
      std::cout << "" << std::endl;

      //page 3 info
      std::cout << BOLD << "Type zhome -p3 / zpm home -p3 to see PAGE3." << RESET << std::endl;
}

void PAGE3(){
      // add return 0, when using !!!
      // PAGE 3 info
      std::cout << YELLOW << "Current PAGE:" << RESET << std::endl;
      std::cout << BOLD << "PAGE 3" << RESET;
      std::cout << " (zpm aliases commands)" << std::endl;
      std::cout << "" << std::endl;
      
      // commands
      std::cout << RED << "Aliases:" << RESET << std::endl;
      std::cout << "  " << GREEN << "zhome" << RESET << "      - Display home page" << std::endl;
      std::cout << "  " << GREEN << "zpm" << RESET << "        - Unified wrapper" << std::endl;
      std::cout << "  " << GREEN << "zupd" << RESET << "       - Update system packages" << std::endl;
      std::cout << "  " << GREEN << "zinst" << RESET << "      - Install package" << std::endl;
      std::cout << "  " << GREEN << "zrm" << RESET << "        - Remove package" << std::endl;
      std::cout << "  " << GREEN << "zlist" << RESET << "      - List installed packages" << std::endl;
      std::cout << "  " << GREEN << "zsearch" << RESET << "    - Search for package" << std::endl;
      std::cout << "  " << GREEN << "zinfo" << RESET << "      - Package information" << std::endl;
      std::cout << "  " << GREEN << "zclean" << RESET << "     - Clean package cache" << std::endl;
      std::cout << "  " << GREEN << "zupgr" << RESET << "      - Update ZPM itself (sudo)" << std::endl;
      std::cout << "  " << GREEN << "zuninstall" << RESET << " - Uninstall ZPM (sudo)" << std::endl;
      std::cout << "  " << GREEN << "zrun" << RESET << "       - Run programs" << std::endl;
      
      // global options
      std::cout << "\n" << BOLD << "Common options:" << RESET << std::endl;
      std::cout << "  --help, -h    Show help for a specific command" << std::endl;
      std::cout << "  --version, -v Show version" << std::endl;
    
      // examples
      std::cout << "\n" << BOLD << "Examples:" << RESET << std::endl;
      std::cout << "  zinst firefox" << std::endl;
      std::cout << "  sudo zupd -full -y" << std::endl;
      std::cout << "  zrm vlc -p" << std::endl;
      std::cout << "  sudo zupgr -ex" <<std::endl;
      
      // zpm wrapper info
      std::cout << "\n" << RED << "New way of using ZPM:" << RESET << std::endl;
      std::cout << "  " << BOLD << "zpm" << RESET << " <command> [options]   (e.g., zpm install firefox)" << std::endl;
      std::cout << "  Run " << BOLD << "zpm --help" << RESET << " for more details." << std::endl;
      std::cout << "" << std::endl;
  
      //page 1 and page 2 info
      std::cout << BOLD << "Type zhome -p1 / zpm home -p1 to see PAGE1." << RESET << std::endl;
      std::cout << BOLD << "Type zhome -p2 / zpm home -p2 to see PAGE2." << RESET << std::endl;
}

void PAGE_ALL(){
    PAGE1();
    std::cout << std::endl;
    PAGE2();
    std::cout << std::endl;
    PAGE3();
}

bool executableAt(const std::string& path) {
    return access(path.c_str(), X_OK) == 0;
}

bool commandExists(const std::string& command) {
    if (command.find('/') != std::string::npos) {
        return executableAt(command);
    }

    const char* pathEnv = getenv("PATH");
    const std::string path = pathEnv != nullptr
        ? pathEnv
        : "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";

    std::stringstream stream(path);
    std::string directory;
    while (std::getline(stream, directory, ':')) {
        if (directory.empty()) {
            directory = ".";
        }
        if (executableAt(directory + "/" + command)) {
            return true;
        }
    }

    return false;
}

std::string firstCommandWord(const std::string& command) {
    std::stringstream stream(command);
    std::string word;
    stream >> word;
    return word;
}

std::string defaultTextEditor() {
    const char* sudoEditor = getenv("SUDO_EDITOR");
    if (sudoEditor != nullptr && sudoEditor[0] != '\0' && commandExists(firstCommandWord(sudoEditor))) {
        return sudoEditor;
    }

    const char* visual = getenv("VISUAL");
    if (visual != nullptr && visual[0] != '\0' && commandExists(firstCommandWord(visual))) {
        return visual;
    }

    const char* editor = getenv("EDITOR");
    if (editor != nullptr && editor[0] != '\0' && commandExists(firstCommandWord(editor))) {
        return editor;
    }

    const std::array<std::string, 4> candidates {
        "sensible-editor",
        "editor",
        "vim",
        "vi",
    };

    for (const std::string& candidate : candidates) {
        if (commandExists(candidate)) {
            return candidate;
        }
    }

    return {};
}

int main(int argc, char* argv[]) {

    zpm_update::checkForUpdates();

    bool page1 = false;
    bool page2 = false;
    bool page3 = false;
    bool config = false;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-p1") page1 = true;
        if (arg == "-p2") page2 = true;
        if (arg == "-p3") page3 = true;
        if (arg == "--edit-config" || arg == "-ed") config = true;
    }

    if ((page1 && config) || (page2 && config) || (page3 && config)) {
        std::cerr << RED
        << "Error: --edit-config / -ed must be used alone"
        << RESET << std::endl;
        return 1;
    }

    if (config) {

        const std::string editor = defaultTextEditor();
        if (editor.empty()) {
            std::cerr << RED << "Error: no terminal text editor found!\n" << RESET;
            return 1;
        }

        if (geteuid() != 0) {
            std::cerr << RED << "Run with sudo!\n" << RESET;
            return 1;
        }
        const std::string command = editor + " /opt/ZPM/zielina.conf";
        execlp("sh", "sh", "-c", command.c_str(), nullptr);
        perror("execlp");
        return 1;
    }

    std::cout << RED << "Zielina Package Manager (ZPM) " << RESET << "v" << zpm_version::version() << std::endl;
    std::cout << "" << std::endl;
    std::cout << BOLD << "ZPM information:" << RESET <<std:: endl;
    std::cout << "Version: v" << zpm_version::version() << std::endl;
    std::cout << "https://github.com/Zielina-Konrad-productions/ZPM" << std::endl;
    std::cout << "Copyright (c) 2026 Ignacyyy & Ry3ball " << std::endl;
    std::cout << "License: MIT" << std::endl;
    std::cout << "" << std::endl;

    // page info
    std::cout << CYAN << "PAGE Information:" << RESET << std::endl;
    std::cout << BOLD << "PAGE 1" << RESET << " - ARM support and config information" <<std::endl;
    std::cout << BOLD << "PAGE 2" << RESET << " - zpm wrapper commands" <<std::endl;
    std::cout << BOLD << "PAGE 3" << RESET << " - zpm aliases" <<std::endl;
    std::cout << "" << std::endl;



    //--------------if page selected by args ----------------------------------------
    if (page1 == true || page2 == true || page3 == true) {
        bool printed = false;
        if (page1 == true) {
            PAGE1();
            printed = true;
        }
        if (page2 == true) {
            if (printed) std::cout << std::endl;
            PAGE2();
            printed = true;
        }
        if (page3 == true) {
            if (printed) std::cout << std::endl;
            PAGE3();
        }
        return 0;
    }

    //----------------------if all pages---------------------------------------------
    if (zhome::showallpages()) {
        PAGE_ALL();
        return 0;
    }

    //-------------------------------if default page = 3-----------------------------
    if (zhome::defaultpage(3)){
        PAGE3();
        return 0;
    }

    //-------------------------------if default page = 2-----------------------------
    if (zhome::defaultpage(2)){
        PAGE2();
        return 0; 
    }

    //---------------------------if default page = 1----------------------------------
    if (zhome::defaultpage(1)){
        PAGE1();
        return 0;
    }
}
