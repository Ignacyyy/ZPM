#pragma once
#include <string>
#include <fstream>
#include <iostream>
#include <cstdlib>

//usage #include arm-recompile.h or if you using main #include "main.h"
// arm::recompile()

namespace arm {
    inline void recompile() {
        std::ifstream conf("/opt/ZPM/zielina.conf");
        if (!conf.is_open()) return;

        std::string line;
        while (std::getline(conf, line)) {
            if (line.find("arm-recompile=true") != std::string::npos) {
                std::cout << "Recompiling for ARM systems...\n";
                std::system("cd /opt/ZPM/src && bash build.sh");
                return;
            }
        }
    }
}