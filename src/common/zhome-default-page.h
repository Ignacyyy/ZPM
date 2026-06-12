#pragma once
#include "main.h"
// ── zhome-default-page.h ──────────────────────────────────────────────────────
//
// Checks whether a specific zhome default page is enabled in zielina.conf,
// or whether "show all pages" mode is active.
//
// USAGE:
//   #include "main.h"                 (via main header)
//
//   if (zhome::defaultpage(2)) { /* show page 2 */ }
//   if (zhome::showallpages())  { /* show all pages */ }
//
// CONFIG (zielina.conf):
//   zhome-default-page-2=true    <- enables page 2 as default
//   zhome-default-page-3=true    <- enables page 3 as default
//   zhome-show-all-pages=true    <- show all pages at once
//   # lines starting with # are ignored
//
// EXTENDING:
//   To add a new page, simply add a new entry in zielina.conf:
//     zhome-default-page-N=true
//   Then handle it in your code:
//     if (zhome::defaultpage(N)) { /* your logic here */ }
//
//   No changes to this header are needed when adding new pages.
//
// RETURNS:
//   defaultpage(N) — true if "zhome-default-page-N=true" found in zielina.conf
//   showallpages() — true if "zhome-show-all-pages=true" found in zielina.conf
//   false in both cases if config is missing or cannot be opened
//
// ─────────────────────────────────────────────────────────────────────────────

namespace zhome {

    inline bool defaultpage(int page) {
        std::ifstream conf("/opt/ZPM/zielina.conf");
        if (!conf.is_open()) return false;

        std::string key = "zhome-default-page-" + std::to_string(page) + "=true";
        std::string line;

        while (std::getline(conf, line)) {
            line.erase(
                std::remove_if(line.begin(), line.end(), [](unsigned char c) {
                    return std::isspace(c) || c == '\r' || c == '\n';
                }),
                line.end()
            );

            if (line.empty() || line[0] == '#') continue;
            if (line.find(key) != std::string::npos) return true;
        }
        return false;
    }

    inline bool showallpages() {
        std::ifstream conf("/opt/ZPM/zielina.conf");
        if (!conf.is_open()) return false;

        std::string line;

        while (std::getline(conf, line)) {
            line.erase(
                std::remove_if(line.begin(), line.end(), [](unsigned char c) {
                    return std::isspace(c) || c == '\r' || c == '\n';
                }),
                line.end()
            );

            if (line.empty() || line[0] == '#') continue;
            if (line.find("zhome-show-all-pages=true") != std::string::npos) return true;
        }
        return false;
    }

}
