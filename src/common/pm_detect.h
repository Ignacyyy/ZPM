#pragma once
#include <string>
#include <cstdio>
#include <unistd.h>

// ============================================================
//  get_package_manager()
//  Wykrywa natywny PM przez /etc/os-release (ID_LIKE / ID),
//  fallback przez binarne ścieżki.
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
                if (r.size() >= 2 && r.front() == '"' && r.back() == '"')
                    r = r.substr(1, r.size() - 2);
                return r;
            };

            if      (s.rfind("ID=",      0) == 0) id      = stripQuotes(s.substr(3));
            else if (s.rfind("ID_LIKE=", 0) == 0) id_like = stripQuotes(s.substr(8));
        }
        fclose(f);

        auto containsWord = [](const std::string& hay, const std::string& needle) {
            size_t pos = hay.find(needle);
            if (pos == std::string::npos) return false;
            bool l = (pos == 0 || hay[pos-1] == ' ');
            bool r = (pos + needle.size() == hay.size()
                      || hay[pos + needle.size()] == ' ');
            return l && r;
        };

        for (const std::string& src : {id_like, id}) {
            if (containsWord(src, "debian") || containsWord(src, "ubuntu"))
                return "apt";
            if (containsWord(src, "suse") || containsWord(src, "opensuse"))
                return "zypper";
            if (containsWord(src, "fedora") || containsWord(src, "rhel")
             || containsWord(src, "centos") || containsWord(src, "rocky")
             || containsWord(src, "alma"))
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
