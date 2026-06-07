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
    std::cout << std::endl;
    std::cout << BOLD << " Options:" << RESET << std::endl;
    std::cout << "  update-info=true/false" << std::endl;
    std::cout << "     shows update notifications in ZPM tools" << std::endl;
    std::cout << "  zhelp-default-page-1=true/false" << std::endl;
    std::cout << "      zhelp default page 1" << std::endl;
    std::cout << "  zhelp-default-page-2=true/false" << std::endl;
    std::cout << "      zhelp default page 2" << std::endl;
    std::cout << "  zhelp-show-all-pages=true/false" << std::endl;
    std::cout << "      zhelp will show all pages at onice" << std::endl;
    std::cout << std::endl;
    std::cout << BOLD << RED << "ARM support:" << RESET << std::endl;
    std::cout << "  ZPM supports ARM (aarch64 / armhf) systems" << std::endl;
    std::cout << "  ARM mode does NOT affect normal usage" << std::endl;
    std::cout << std::endl;
   
    //zhome page info
    std::cout << BOLD << "Type zhome -p2 to see PAGE2." << std::endl;
}

void PAGE2(){
      // add return 0, when using !!!
      // PAGE 2 info
      std::cout << YELLOW << "Current PAGE:" << RESET << std::endl;
      std::cout << BOLD << "PAGE 2" << RESET;
      std::cout << " (Commands, options and zpm wrapper info)" << std::endl;
      std::cout << "" << std::endl;
      
      // commands
      std::cout << RED << "Commands:" << RESET << std::endl;
      std::cout << "  " << GREEN << "zhelp" << RESET << "      - Show this help message" << std::endl;
      std::cout << "  " << GREEN << "zpm" << RESET << "        - Unified wrapper (e.g., zpm install, zpm update)" << std::endl;
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
  
      //page 1 info
      std::cout << BOLD << "Type zhome -p1 to see PAGE1." << std::endl;
}

void PAGE1_PAGE2(){
    // add return 0, when using !!!
    // ARM support + config info
    std::cout << "" << std::endl;
    std::cout << BOLD << RED << "ZPM Configuration:" << RESET << std::endl;
    std::cout << "  Config file: /opt/ZPM/zielina.conf" << std::endl;
    std::cout << "  Used for runtime behavior and update settings" << std::endl;
    std::cout << std::endl;
    std::cout << BOLD << " Options:" << RESET << std::endl;
    std::cout << "  update-info=true/false" << std::endl;
    std::cout << "     shows update notifications in ZPM tools" << std::endl;
    std::cout << "  zhelp-default-page-1=true/false" << std::endl;
    std::cout << "      zhelp default page 1" << std::endl;
    std::cout << "  zhelp-default-page-2=true/false" << std::endl;
    std::cout << "      zhelp default page 2" << std::endl;
    std::cout << "  zhelp-show-all-pages=true/false" << std::endl;
    std::cout << "      zhelp will show all pages at onice" << std::endl;
    std::cout << std::endl;
    std::cout << BOLD << RED << "ARM support:" << RESET << std::endl;
    std::cout << "  ZPM supports ARM (aarch64 / armhf) systems" << std::endl;
    std::cout << "  ARM mode does NOT affect normal usage" << std::endl;
    std::cout << std::endl;

    // commands
    std::cout << RED << "Commands:" << RESET << std::endl;
    std::cout << "  " << GREEN << "zhelp" << RESET << "      - Show this help message" << std::endl;
    std::cout << "  " << GREEN << "zpm" << RESET << "        - Unified wrapper (e.g., zpm install, zpm update)" << std::endl;
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
}

int main(int argc, char* argv[]) {

    bool page2 = false;
    bool page1 = false;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-p2") page2 = true;
        if (arg == "-p1") page1 = true;
    }

    zpm_update::checkForUpdates();

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
    std::cout << BOLD << "PAGE 2" << RESET << " - commands, options and zpm wrapper info" <<std::endl;
    std::cout << "" << std::endl;



    //--------------if page 1 and page 2 --------------------------------------------
    if(page1 == true && page2 == true){
        PAGE1_PAGE2();
        return 0;
    }

    //---------------------------if page 2 = true (arg)------------------------------
    if (page2 == true){
        PAGE2();
        return 0;
    }

    //------------------------------if page 1 = true (arg)---------------------------
    if ( page1 == true) {
        PAGE1();
        return 0;
    }

    //----------------------if all pages---------------------------------------------
    if (zhelp::showallpages()) {
        PAGE1_PAGE2();
        return 0;
    }

    //-------------------------------if default page = 2-----------------------------
    if (zhelp::defaultpage(2)){
        PAGE2();
        return 0; 
    }

    //---------------------------if default page = 1----------------------------------
    if (zhelp::defaultpage(1)){
        PAGE1();
        return 0;
    }
}
