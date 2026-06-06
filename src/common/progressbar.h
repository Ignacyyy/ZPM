#pragma once
#include "main.h"
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
#include <sys/ioctl.h>

// ============================================================
//  UiState — stan aktualizacji, ustawiany przez logikę zupd.
//  Dodaj nowe wartości gdy dodasz nowe systemy pakietów.
// ============================================================
enum class UiState {
    IDLE,       // przed startem
    CHECKING,   // dpkg fix / rpm --rebuilddb
    APT,        // aktualizacja APT    (Debian/Ubuntu)
    ZYPPER,     // aktualizacja Zypper (openSUSE/SLES)
    DNF,        // aktualizacja DNF    (Fedora/RHEL/Rocky/Alma)
    FLATPAK,    // aktualizacja Flatpak
    SNAP,       // aktualizacja Snap
    CLEANUP,    // czyszczenie
    DONE,       // sukces
    ERROR,      // błąd
    CUSTOM      // etykieta ręczna — stare API (zclean, zinst, zrm)
    // Nowe systemy: dopisz tutaj, np. PACMAN, HOMEBREW, itp.
};

// ── stan współdzielony ────────────────────────────────────────────────────────
static std::atomic<float>    g_progress{0.0f};
static std::atomic<bool>     g_spinnerRunning{false};
static std::atomic<UiState>  g_uiState{UiState::IDLE};
static std::atomic<int>      g_step{0};
static std::atomic<int>      g_totalSteps{1};
static std::thread           g_spinnerThread;

static std::mutex            g_taskMutex;
static std::string           g_task;

// ── mapowanie UiState → etykieta ─────────────────────────────────────────────
static std::string uiStateLabel(UiState s) {
    switch (s) {
        case UiState::IDLE:     return "Idle";
        case UiState::CHECKING: return "Checking system consistency";
        case UiState::APT:      return "Updating APT packages";
        case UiState::ZYPPER:   return "Updating Zypper packages";
        case UiState::DNF:      return "Updating DNF packages";
        case UiState::FLATPAK:  return "Updating Flatpak packages";
        case UiState::SNAP:     return "Updating Snap packages";
        case UiState::CLEANUP:  return "Cleaning up";
        case UiState::DONE:     return "Done";
        case UiState::ERROR:    return "Error";
        case UiState::CUSTOM: {
            std::lock_guard<std::mutex> lock(g_taskMutex);
            return g_task;
        }
        default: return "Working...";
    }
}

// ── wewnętrzna funkcja rysująca jeden kadr ────────────────────────────────────
static void drawBar(float totalProgress, const std::string& task, char spinnerChar)
{
    struct winsize w;
    int termWidth = 80;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
        termWidth = w.ws_col;

    const int barWidth        = std::max(10, std::min(40, termWidth / 3));
    const int visualPrefixLen = 27 + barWidth + 4;
    const int taskMaxLen      = std::max(1, termWidth - visualPrefixLen);

    std::string taskTrimmed = task;
    taskTrimmed.erase(std::remove(taskTrimmed.begin(), taskTrimmed.end(), '\n'),
                      taskTrimmed.end());
    if ((int)taskTrimmed.size() > taskMaxLen)
        taskTrimmed = taskTrimmed.substr(0, taskMaxLen - 1) + "~";

    int pos     = barWidth * (totalProgress / 100.0f);
    int percent = std::max(0, std::min(100, (int)totalProgress));

    std::string spinnerDisplay;
    if (percent >= 100)
        spinnerDisplay = GREEN + std::string("[✓]") + RESET;
    else
        spinnerDisplay = YELLOW + std::string("[") + spinnerChar + std::string("]") + RESET;

    std::cout << "\r\033[K" << YELLOW << "Progress: [" << RESET;
    for (int i = 0; i < barWidth; ++i)
        std::cout << (i < pos ? GREEN + std::string("#") + RESET : std::string(" "));

    std::cout << YELLOW << "] " << std::setw(3) << percent << "% " << RESET
              << spinnerDisplay << " | " << taskTrimmed << "\033[K" << std::flush;
}

// ── pętla spinnera (osobny wątek) ─────────────────────────────────────────────
static void spinnerLoop()
{
    static const char frames[] = { '|', '/', '-', '\\' };
    int idx = 0;
    while (g_spinnerRunning.load()) {
        UiState state = g_uiState.load();

        float pct;
        std::string label;

        if (state == UiState::CUSTOM) {
            // Tryb stary (zinst/zrm/zclean): procent i etykieta ustawiane ręcznie
            pct = g_progress.load();
            std::lock_guard<std::mutex> lock(g_taskMutex);
            label = g_task;
        } else {
            // Tryb nowy (zupd): procent wyliczany z kroku
            int step  = g_step.load();
            int total = g_totalSteps.load();
            pct = (total > 0)
                ? static_cast<float>(step) / static_cast<float>(total) * 100.0f
                : 0.0f;
            if (pct > 100.0f) pct = 100.0f;
            label = std::to_string(step) + "/" +
                    std::to_string(total) + " | " +
                    uiStateLabel(state);
        }

        drawBar(pct, label, frames[idx++ % 4]);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
}

// ── pomocnicza: uruchom wątek jeśli jeszcze nie działa ───────────────────────
static void ensureSpinnerRunning() {
    if (!g_spinnerRunning.load()) {
        g_spinnerRunning.store(true);
        g_spinnerThread = std::thread(spinnerLoop);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  PUBLICZNE API — NOWE  (zupd z UiState)
// ═════════════════════════════════════════════════════════════════════════════

/** Wystartuj pasek. total = łączna liczba kroków. */
inline void progressbar_start(int total) {
    g_totalSteps.store(total);
    g_step.store(0);
    g_uiState.store(UiState::IDLE);
    g_progress.store(0.0f);
    ensureSpinnerRunning();
}

/** Zaktualizuj stan przed każdym krokiem. */
inline void progressbar_set_state(UiState state, int step) {
    g_uiState.store(state);
    g_step.store(step);
}

// ═════════════════════════════════════════════════════════════════════════════
//  PUBLICZNE API — STARE  (zinst, zrm, zclean)
//
//  zinst/zrm wywołują progressbar_start() WIELOKROTNIE (raz na pakiet) —
//  dlatego przy kolejnych wywołaniach tylko aktualizujemy stan,
//  nie restartujemy wątku.
// ═════════════════════════════════════════════════════════════════════════════

/** Uruchamia pasek (lub aktualizuje jeśli już działa). */
inline void progressbar_start(float totalProgress, const std::string& task) {
    g_uiState.store(UiState::CUSTOM);
    g_progress.store(totalProgress);
    {
        std::lock_guard<std::mutex> lock(g_taskMutex);
        g_task = task;
    }
    ensureSpinnerRunning(); // startuje tylko raz — przy kolejnych pakietach nic nie robi
}

/** Uruchamia pasek bez argumentów (fallback). */
inline void progressbar_start() {
    progressbar_start(0.0f, "Starting...");
}

/** Aktualizuje procent i opis w locie. */
inline void progressbar_update(float totalProgress, const std::string& task) {
    g_uiState.store(UiState::CUSTOM);
    {
        std::lock_guard<std::mutex> lock(g_taskMutex);
        g_task = task;
    }
    g_progress.store(totalProgress);
}

/** Zatrzymuje wątek, rysuje końcowy stan (100% + ✓). */
inline void progressbar_finish(const std::string& task) {
    g_uiState.store(UiState::CUSTOM);
    g_progress.store(100.0f);
    {
        std::lock_guard<std::mutex> lock(g_taskMutex);
        g_task = task;
    }
    g_spinnerRunning.store(false);
    if (g_spinnerThread.joinable())
        g_spinnerThread.join();

    drawBar(100.0f, task, ' ');
    std::cout << std::endl;
}

// ── alias wstecznej kompatybilności ──────────────────────────────────────────
inline void progressbar(float p, const std::string& task) {
    progressbar_start(p, task);
}
