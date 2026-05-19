#pragma once
#include <string>
#include <fstream>
#include <algorithm>

// ── zhelp-default-page.h ──────────────────────────────────────────────────────
//
// Checks whether a specific zhelp default page is enabled in zielina.conf.
//
// USAGE:
//   #include "zhelp-default-page.h"   (standalone)
//   #include "main.h"                 (via main header)
//
//   if (zhelp::defaultpage(2)) {......program........ }
//   if (zhelp::defaultpage(3)) {........ program........ }
//
// CONFIG (zielina.conf):
//   zhelp-default-page-2=true    <- enables page 2
//   zhelp-default-page-3=true    <- enables page 3
//   # lines starting with # are ignored
//
// EXTENDING:
//   To add a new page, simply add a new entry in zielina.conf:
//     zhelp-default-page-N=true
//   Then handle it in your code:
//     if (zhelp::defaultpage(N)) { /* your logic here */ }
//
//   No changes to this header are needed when adding new pages.
//
// RETURNS:
//   true  — if "zhelp-default-page-N=true" is found in zielina.conf
//   false — if not found, config missing, or config cannot be opened
//
// ─────────────────────────────────────────────────────────────────────────────

namespace zhelp {
    inline bool defaultpage(int page) {
        std::ifstream conf("/opt/ZPM/zielina.conf");
        if (!conf.is_open()) return false;

        std::string key = "zhelp-default-page-" + std::to_string(page) + "=true";
        std::string line;

        while (std::getline(conf, line)) {
            // strip whitespace
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
}