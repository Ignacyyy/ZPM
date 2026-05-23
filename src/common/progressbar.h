#pragma once
#include "main.h"
#include <atomic>
#include <thread>
#include <chrono>

// ── stan współdzielony ────────────────────────────────────────────────────────
static std::atomic<float>  g_progress{0.0f};
static std::atomic<bool>   g_spinnerRunning{false};
static std::string         g_task;
static std::thread         g_spinnerThread;

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
        drawBar(g_progress.load(), g_task, frames[idx++ % 4]);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
}

// ── publiczne API ─────────────────────────────────────────────────────────────

/** Uruchamia animowany pasek. Wywołaj PRZED blokującą operacją. */
void progressbar_start(float totalProgress, const std::string& task)
{
    g_progress.store(totalProgress);
    g_task = task;

    if (!g_spinnerRunning.load()) {
        g_spinnerRunning.store(true);
        g_spinnerThread = std::thread(spinnerLoop);
    }
}

/** Aktualizuje procent i opis w locie (wątek spinnera sam odczyta). */
void progressbar_update(float totalProgress, const std::string& task)
{
    g_task = task;
    g_progress.store(totalProgress);
}

/** Zatrzymuje wątek, rysuje końcowy stan (100 % + ✓). */
void progressbar_finish(const std::string& task)
{
    g_progress.store(100.0f);
    g_task = task;

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

// ── przykład użycia ───────────────────────────────────────────────────────────
//
// progressbar_start(0.0f, "0/4 | start...");
// std::system("apt-get update -qq");
//
// progressbar_update(25.0f, "1/4 | updating package lists...");
// std::system("apt-get upgrade -y -qq");
//
// progressbar_update(50.0f, "2/4 | upgrading packages...");
// std::system("apt-get autoremove -y -qq");
//
// progressbar_update(75.0f, "3/4 | snap refresh...");
// std::system("snap refresh 2>/dev/null");
//
// progressbar_finish("4/4 | done!");