#pragma once
#include <string>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <algorithm>

// Usage:
//   #include "arm-recompile.h"   (standalone)
//   #include "main.h"            (via main header)
//   arm::recompile();

namespace arm {
    inline void recompile() {

        std::cout << "\033[33m[ARM-DEBUG] ARM-recompile\033[0m\n";

        std::ifstream conf("/opt/ZPM/zielina.conf");
        if (!conf.is_open()) {
            std::cout << "\033[31m[ARM-DEBUG] ERROR: can't open /opt/ZPM/zielina.conf!\033[0m\n";
            return;
        }

        std::string line;
        int line_number = 0;
        while (std::getline(conf, line)) {
            line_number++;


            std::string clean_line = line;
            clean_line.erase(
                std::remove_if(clean_line.begin(), clean_line.end(), [](unsigned char c) {
                    return std::isspace(c) || c == '\r' || c == '\n';
                }),
                clean_line.end()
            );

            if (clean_line.empty() || clean_line[0] == '#') {
                continue;
            }

            if (clean_line.find("arm-recompile=true") != std::string::npos) {
                std::cout << "\033[32m[ARM] recompiling...\033[0m";
                std::cout << "(it may take a while...)\n";
                int status = std::system("cd /opt/ZPM/src && bash build.sh >> /dev/null");
                 std::cout << "\033[32m[ARM] DONE\033[0m\n";
                return;
            }
        }


    }
}
