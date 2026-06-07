#pragma once
#include <string>
#include <cstdio>
#include <unistd.h>

// ============================================================
//  get_package_manager()
//  Zwraca: "apt" | "zypper" | "dnf" | "unknown"
// ============================================================
inline std::string get_package_manager() {
    FILE* f = fopen("/etc/os-release", "r");
    if (f) {
        char line[256];
        std::string id, id_like;

        while (fgets(line, sizeof(line), f)) {
            std::string s(line);
            if (!s.empty() && s.back() == '\n') s.pop_back();

            auto stripQuotes = [](const std::string& v) {
                std::string r = v;
                if (!r.empty() && r.front() == '"') r.erase(0, 1);
                if (!r.empty() && r.back()  == '"') r.pop_back();
                return r;
            };

            if      (s.rfind("ID=",      0) == 0) id      = stripQuotes(s.substr(3));
            else if (s.rfind("ID_LIKE=", 0) == 0) id_like = stripQuotes(s.substr(8));
        }
        fclose(f);

        // Sprawdza czy needle występuje jako osobne słowo LUB jako prefiks słowa
        // (obsługuje "opensuse-leap", "rhel", "centos-stream" itp.)
        auto containsFamily = [](const std::string& hay, const std::string& needle) {
            size_t pos = 0;
            while ((pos = hay.find(needle, pos)) != std::string::npos) {
                bool leftOk  = (pos == 0 || hay[pos-1] == ' ');
                // prawa strona: koniec stringa, spacja, lub '-' (opensuse-leap)
                size_t end = pos + needle.size();
                bool rightOk = (end == hay.size() || hay[end] == ' ' || hay[end] == '-');
                if (leftOk && rightOk) return true;
                pos++;
            }
            return false;
        };

        for (const std::string& src : {id_like, id}) {
            if (containsFamily(src, "debian") || containsFamily(src, "ubuntu"))
                return "apt";
            if (containsFamily(src, "suse") || containsFamily(src, "opensuse"))
                return "zypper";
            if (containsFamily(src, "fedora") || containsFamily(src, "rhel")
             || containsFamily(src, "centos") || containsFamily(src, "rocky")
             || containsFamily(src, "alma")   || containsFamily(src, "nobara"))
                return "dnf";
        }
    }

    // Fallback — binarne ścieżki
    if (access("/usr/bin/apt-get", X_OK) == 0 || access("/bin/apt-get", X_OK) == 0)
        return "apt";
    if (access("/usr/bin/zypper", X_OK) == 0)
        return "zypper";
    if (access("/usr/bin/dnf", X_OK) == 0)
        return "dnf";

    return "unknown";
}
