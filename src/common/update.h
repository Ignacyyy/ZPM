#pragma once
#include "main.h"
#include <chrono>
#include <fstream>

namespace zpm_update {

// ───────────────────────── EXEC ─────────────────────────

static inline std::string exec(const char* cmd) {
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, int(*)(FILE*)> pipe(popen(cmd, "r"), pclose);
    if (!pipe)
        return "";
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr)
        result += buffer.data();
    return result;
}

// ───────────────────────── CONFIG ─────────────────────────

static inline bool updateInfoEnabled() {
    std::ifstream conf("/opt/ZPM/zielina.conf");
    if (!conf.is_open())
        return true;

    std::string line;
    while (std::getline(conf, line)) {
        line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
        line.erase(std::remove(line.begin(), line.end(), '\n'), line.end());
        if (line.empty() || line[0] == '#')
            continue;
        if (line.rfind("update-info=", 0) == 0) {
            std::string value = line.substr(12);
            std::transform(value.begin(), value.end(), value.begin(),
                [](unsigned char c){ return std::tolower(c); });
            return (value == "true" || value == "1" || value == "yes");
        }
    }
    return true;
}

// ───────────────────────── CACHE CZASU ─────────────────────────

static inline bool shouldCheckForUpdates() {
    std::ifstream in("/tmp/zpm_last_update_check");
    if (in.is_open()) {
        long long last_check = 0;
        in >> last_check;
        in.close();

        auto now = std::chrono::system_clock::now().time_since_epoch();
        long long current_time = std::chrono::duration_cast<std::chrono::seconds>(now).count();

        // 15 minut = 900 sekund
        if (current_time - last_check < 900) {
            return false;
        }
    }
    return true;
}

static inline void saveCheckTime() {
    std::ofstream out("/tmp/zpm_last_update_check");
    if (out.is_open()) {
        auto now = std::chrono::system_clock::now().time_since_epoch();
        long long current_time = std::chrono::duration_cast<std::chrono::seconds>(now).count();
        out << current_time;
    }
}

// ───────────────────────── REMOTE VERSION ─────────────────────────

static inline std::string get_latest_version() {
    // Wyciszenie błędów curla i parsowania JSON w Pythonie
    std::string cmd =
        "curl -fsSL -H 'User-Agent: ZPM' "
        "https://api.github.com/repos/Zielina-Konrad-productions/ZPM/releases/latest 2>/dev/null "
        "| python3 -c \"\n"
        "import sys, json\n"
        "try:\n"
        "    r = json.load(sys.stdin)\n"
        "    print(r.get('tag_name', '').lstrip('v'))\n"
        "except:\n"
        "    pass\n"
        "\" 2>/dev/null";

    std::string v = exec(cmd.c_str());
    v.erase(std::remove(v.begin(), v.end(), '\n'), v.end());
    v.erase(std::remove(v.begin(), v.end(), ' '), v.end());
    return v;
}

// ───────────────────────── LOCAL VERSION ─────────────────────────

static inline std::string get_installed_version() {
    FILE* f = fopen("/opt/ZPM/VERSION.txt", "r");
    if (!f)
        return "none";

    char line[64];
    std::string v;
    if (fgets(line, sizeof(line), f))
        v = line;
    fclose(f);

    v.erase(std::remove(v.begin(), v.end(), '\n'), v.end());
    v.erase(std::remove(v.begin(), v.end(), ' '), v.end());
    return v.empty() ? "none" : v;
}

// ───────────────────────── VERSION PARSE ─────────────────────────

static inline std::vector<int> parse_version(const std::string& ver) {
    std::string v = ver;

    size_t pos = v.find("-pre");
    if (pos != std::string::npos) {
        std::string suffix = v.substr(pos + 4);
        v.replace(pos, 4 + suffix.length(), suffix.empty() ? ".0" : "." + suffix);
    }

    std::vector<int> out;
    std::stringstream ss(v);
    std::string seg;
    while (std::getline(ss, seg, '.')) {
        try {
            out.push_back(std::stoi(seg));
        } catch (...) {
            out.push_back(0);
        }
    }

    while (out.size() < 3)
        out.push_back(0);

    return out;
}

// ───────────────────────── COMPARE ─────────────────────────

static inline bool is_newer(const std::string& latest, const std::string& current) {
    if (current == "none")
        return true;

    auto a = parse_version(latest);
    auto b = parse_version(current);

    for (size_t i = 0; i < a.size(); i++) {
        if (a[i] > b[i]) return true;
        if (a[i] < b[i]) return false;
    }
    return false;
}

// ───────────────────────── CHECK ─────────────────────────

static inline void checkForUpdates() {
    if (!updateInfoEnabled())
        return;

    // Sprawdzenie czy minęło 15 minut
    if (!shouldCheckForUpdates())
        return;

    std::string current = get_installed_version();
    std::string latest  = get_latest_version();

    // Zapisanie czasu po próbie pobrania najnowszej wersji
    saveCheckTime();

    if (latest.empty() || current == "none")
        return;

    if (!is_newer(latest, current))
        return;

    std::cout << CYAN << "\n====================================\n" << RESET;
    std::cout << YELLOW << "      ZPM UPDATE AVAILABLE\n" << RESET;
    std::cout << CYAN << "====================================\n\n" << RESET;
}

} // namespace zpm_update
