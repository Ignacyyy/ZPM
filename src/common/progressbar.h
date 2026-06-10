#pragma once

#include "colors.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

enum class UiState {
    IDLE,
    CHECKING,
    APT,
    ZYPPER,
    DNF,
    FLATPAK,
    SNAP,
    CLEANUP,
    DONE,
    ERROR,
    CUSTOM
};

namespace zpm::progressbar_detail {

constexpr std::chrono::milliseconds kFrameInterval{80};
constexpr int kDefaultTerminalWidth = 80;
constexpr int kMinFullBarWidth = 3;
constexpr int kMaxBarWidth = 40;
constexpr int kFullColumnsWithoutBar = 21;
constexpr int kTaskSeparatorColumns = 3;
constexpr char kSpinnerFrames[] = {'|', '/', '-', '\\'};

struct Snapshot {
    UiState state = UiState::IDLE;
    float progress = 0.0f;
    int step = 0;
    int totalSteps = 1;
    std::string task;
};

inline float clampProgress(float value) noexcept {
    if (!std::isfinite(value)) {
        return 0.0f;
    }

    return std::clamp(value, 0.0f, 100.0f);
}

inline int normalizeTotalSteps(int total) noexcept {
    return std::max(total, 1);
}

inline int terminalWidth() noexcept {
    winsize size {};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
        return static_cast<int>(size.ws_col);
    }

    return kDefaultTerminalWidth;
}

inline std::string stateLabel(UiState state) {
    switch (state) {
        case UiState::IDLE:
            return "Idle";
        case UiState::CHECKING:
            return "Checking system consistency";
        case UiState::APT:
            return "Updating APT packages";
        case UiState::ZYPPER:
            return "Updating Zypper packages";
        case UiState::DNF:
            return "Updating DNF packages";
        case UiState::FLATPAK:
            return "Updating Flatpak packages";
        case UiState::SNAP:
            return "Updating Snap packages";
        case UiState::CLEANUP:
            return "Cleaning up";
        case UiState::DONE:
            return "Done";
        case UiState::ERROR:
            return "Error";
        case UiState::CUSTOM:
            return {};
    }

    return "Working...";
}

inline std::string sanitizeTask(std::string task, int maxLength) {
    task.erase(std::remove_if(task.begin(),
                              task.end(),
                              [](unsigned char ch) {
                                  return ch < 32 || ch == 127;
                              }),
               task.end());

    if (maxLength <= 0 || task.empty()) {
        return {};
    }

    const auto limit = static_cast<std::size_t>(maxLength);
    if (task.size() <= limit) {
        return task;
    }

    if (limit == 1) {
        return "~";
    }

    task.resize(limit - 1);
    task += '~';
    return task;
}

inline float snapshotProgress(const Snapshot& snapshot) noexcept {
    if (snapshot.state == UiState::CUSTOM) {
        return clampProgress(snapshot.progress);
    }

    const int total = normalizeTotalSteps(snapshot.totalSteps);
    const int step = std::clamp(snapshot.step, 0, total);
    return clampProgress((static_cast<float>(step) / static_cast<float>(total)) * 100.0f);
}

inline std::string snapshotTask(const Snapshot& snapshot) {
    if (snapshot.state == UiState::CUSTOM) {
        return snapshot.task;
    }

    const int total = normalizeTotalSteps(snapshot.totalSteps);
    const int step = std::clamp(snapshot.step, 0, total);
    return std::to_string(step) + "/" + std::to_string(total) + " | " +
           stateLabel(snapshot.state);
}

inline std::mutex& outputMutex() {
    static auto* mutex = new std::mutex;
    return *mutex;
}

inline void writeStatusIcon(int percent, char spinnerChar) {
    if (percent >= 100) {
        std::cout << GREEN << "[+]" << RESET;
    } else {
        std::cout << YELLOW << '[' << spinnerChar << ']' << RESET;
    }
}

inline void drawCompactBar(const Snapshot& snapshot,
                           int percent,
                           int terminalColumns,
                           char spinnerChar) {
    std::lock_guard<std::mutex> outputLock(outputMutex());
    std::cout << "\r\033[K";

    if (terminalColumns < 4) {
        std::cout << (percent >= 100 ? GREEN : YELLOW)
                  << (percent >= 100 ? '+' : spinnerChar)
                  << RESET << "\033[K" << std::flush;
        return;
    }

    const std::string percentText = std::to_string(percent) + "%";
    if (terminalColumns < 8) {
        std::cout << YELLOW << percentText << RESET << "\033[K" << std::flush;
        return;
    }

    const int baseColumns = static_cast<int>(percentText.size()) + 4;
    const int taskMaxLength =
        std::max(0, terminalColumns - baseColumns - kTaskSeparatorColumns);
    const std::string task = sanitizeTask(snapshotTask(snapshot), taskMaxLength);

    std::cout << YELLOW << percentText << ' ' << RESET;
    writeStatusIcon(percent, spinnerChar);

    if (!task.empty()) {
        std::cout << " | " << task;
    }

    std::cout << "\033[K" << std::flush;
}

inline void drawBar(const Snapshot& snapshot, char spinnerChar) {
    const float progress = snapshotProgress(snapshot);
    const int percent = static_cast<int>(progress);
    const int termWidth = terminalWidth();

    if (termWidth < kFullColumnsWithoutBar + kMinFullBarWidth) {
        drawCompactBar(snapshot, percent, termWidth, spinnerChar);
        return;
    }

    const int maxBarWidth = std::min(kMaxBarWidth, termWidth - kFullColumnsWithoutBar);
    const int barWidth = std::clamp(termWidth / 3, kMinFullBarWidth, maxBarWidth);
    const int filledWidth = std::clamp(static_cast<int>((progress / 100.0f) * barWidth),
                                       0,
                                       barWidth);
    const int emptyWidth = barWidth - filledWidth;

    const int taskMaxLength =
        std::max(0, termWidth - barWidth - kFullColumnsWithoutBar - kTaskSeparatorColumns);
    const std::string task = sanitizeTask(snapshotTask(snapshot), taskMaxLength);

    std::lock_guard<std::mutex> outputLock(outputMutex());
    std::cout << "\r\033[K" << YELLOW << "Progress: [" << RESET
              << GREEN << std::string(static_cast<std::size_t>(filledWidth), '#') << RESET
              << std::string(static_cast<std::size_t>(emptyWidth), ' ')
              << YELLOW << "] " << std::setw(3) << percent << "% " << RESET;

    writeStatusIcon(percent, spinnerChar);

    if (!task.empty()) {
        std::cout << " | " << task;
    }

    std::cout << "\033[K" << std::flush;
}

class ProgressBarController {
public:
    ProgressBarController() = default;

    ProgressBarController(const ProgressBarController&) = delete;
    ProgressBarController& operator=(const ProgressBarController&) = delete;

    ~ProgressBarController() {
        stopThread();
    }

    void start(int totalSteps) {
        Snapshot fallback;
        bool renderFallback = false;

        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            state_ = UiState::IDLE;
            progress_ = 0.0f;
            step_ = 0;
            totalSteps_ = normalizeTotalSteps(totalSteps);
            task_.clear();
            dirty_ = true;

            if (!startThreadLocked()) {
                fallback = snapshotLocked();
                renderFallback = true;
            }
        }

        stateChanged_.notify_all();
        if (renderFallback) {
            drawBar(fallback, ' ');
        }
    }

    void setState(UiState state, int step) {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            state_ = state;
            step_ = step;
            dirty_ = true;
        }

        stateChanged_.notify_all();
    }

    void start(float progress, const std::string& task) {
        Snapshot fallback;
        bool renderFallback = false;

        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            setCustomStateLocked(progress, task);
            dirty_ = true;

            if (!startThreadLocked()) {
                fallback = snapshotLocked();
                renderFallback = true;
            }
        }

        stateChanged_.notify_all();
        if (renderFallback) {
            drawBar(fallback, ' ');
        }
    }

    void update(float progress, const std::string& task) {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            setCustomStateLocked(progress, task);
            dirty_ = true;
        }

        stateChanged_.notify_all();
    }

    void finish(const std::string& task) {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            setCustomStateLocked(100.0f, task);
            dirty_ = true;
        }

        stopThread();
        drawBar(Snapshot{UiState::CUSTOM, 100.0f, 0, 1, task}, ' ');

        std::lock_guard<std::mutex> outputLock(outputMutex());
        std::cout << '\n';
    }

private:
    Snapshot snapshotLocked() const {
        return Snapshot{state_, progress_, step_, totalSteps_, task_};
    }

    void setCustomStateLocked(float progress, const std::string& task) {
        state_ = UiState::CUSTOM;
        progress_ = clampProgress(progress);
        task_ = task;
    }

    bool startThreadLocked() {
        if (running_) {
            return true;
        }

        running_ = true;
        try {
            worker_ = std::thread(&ProgressBarController::run, this);
            return true;
        } catch (...) {
            running_ = false;
            return false;
        }
    }

    void stopThread() {
        std::thread workerToJoin;

        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (!running_ && !worker_.joinable()) {
                return;
            }

            running_ = false;
            if (worker_.joinable()) {
                workerToJoin = std::move(worker_);
            }
        }

        stateChanged_.notify_all();
        if (workerToJoin.joinable()) {
            workerToJoin.join();
        }
    }

    void run() {
        std::size_t frame = 0;
        std::unique_lock<std::mutex> lock(stateMutex_);

        while (running_) {
            const Snapshot snapshot = snapshotLocked();
            const char spinnerChar =
                kSpinnerFrames[frame++ % (sizeof(kSpinnerFrames) / sizeof(kSpinnerFrames[0]))];
            dirty_ = false;

            lock.unlock();
            drawBar(snapshot, spinnerChar);
            lock.lock();

            stateChanged_.wait_for(lock, kFrameInterval, [this] {
                return !running_ || dirty_;
            });
        }
    }

    std::mutex stateMutex_;
    std::condition_variable stateChanged_;
    std::thread worker_;
    bool running_ = false;
    bool dirty_ = false;
    UiState state_ = UiState::IDLE;
    float progress_ = 0.0f;
    int step_ = 0;
    int totalSteps_ = 1;
    std::string task_;
};

inline ProgressBarController& controller() {
    static ProgressBarController instance;
    return instance;
}

} // namespace zpm::progressbar_detail

inline void progressbar_start(int total) {
    zpm::progressbar_detail::controller().start(total);
}

inline void progressbar_set_state(UiState state, int step) {
    zpm::progressbar_detail::controller().setState(state, step);
}

inline void progressbar_start(float totalProgress, const std::string& task) {
    zpm::progressbar_detail::controller().start(totalProgress, task);
}

inline void progressbar_start() {
    progressbar_start(0.0f, "Starting...");
}

inline void progressbar_update(float totalProgress, const std::string& task) {
    zpm::progressbar_detail::controller().update(totalProgress, task);
}

inline void progressbar_finish(const std::string& task) {
    zpm::progressbar_detail::controller().finish(task);
}

inline void progressbar(float progress, const std::string& task) {
    progressbar_start(progress, task);
}
