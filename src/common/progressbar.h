#pragma once

#include "colors.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <thread>
#include <unistd.h>

enum class UiState {
    IDLE,
    CHECKING,
    ZPM_CHECKING,
    ZPM_DOWNLOAD,
    ZPM_EXTRACT,
    ZPM_BUILD,
    ZPM_INSTALL,
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
constexpr int kMinFullBarWidth = 8;
constexpr int kMaxBarWidth = 40;
constexpr int kFullColumnsWithoutBar = 12;
constexpr int kTaskSeparatorColumns = 3;
constexpr int kBounceBlockWidth = 3;
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
        case UiState::ZPM_CHECKING:
            return "Checking ZPM requirements";
        case UiState::ZPM_DOWNLOAD:
            return "Downloading ZPM release";
        case UiState::ZPM_EXTRACT:
            return "Extracting ZPM release";
        case UiState::ZPM_BUILD:
            return "Building ZPM";
        case UiState::ZPM_INSTALL:
            return "Installing ZPM";
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

class TerminalGuard {
public:
    TerminalGuard() = default;

    TerminalGuard(const TerminalGuard&) = delete;
    TerminalGuard& operator=(const TerminalGuard&) = delete;

    ~TerminalGuard() {
        restore(false);
    }

    void activate() noexcept {
        hideCursor();

        if (inputGuardActive_ || !::isatty(STDIN_FILENO)) {
            return;
        }

        termios current {};
        if (::tcgetattr(STDIN_FILENO, &current) != 0) {
            return;
        }

        originalTermios_ = current;
        current.c_lflag &= ~ECHO;

#ifdef ECHONL
        current.c_lflag &= ~ECHONL;
#endif

        if (::tcsetattr(STDIN_FILENO, TCSANOW, &current) != 0) {
            return;
        }

        inputGuardActive_ = true;
    }

    void restore(bool flushInput = true) noexcept {
        if (inputGuardActive_) {
            if (flushInput) {
                ::tcflush(STDIN_FILENO, TCIFLUSH);
            }

            ::tcsetattr(STDIN_FILENO, TCSANOW, &originalTermios_);
            inputGuardActive_ = false;
        }

        showCursor();
    }

private:
    void hideCursor() noexcept {
        if (cursorHidden_ || !::isatty(STDOUT_FILENO)) {
            return;
        }

        std::lock_guard<std::mutex> outputLock(outputMutex());
        std::cout << "\033[?25l" << std::flush;
        cursorHidden_ = true;
    }

    void showCursor() noexcept {
        if (!cursorHidden_) {
            return;
        }

        std::lock_guard<std::mutex> outputLock(outputMutex());
        std::cout << "\033[?25h" << std::flush;
        cursorHidden_ = false;
    }

    termios originalTermios_ {};
    bool inputGuardActive_ = false;
    bool cursorHidden_ = false;
};

inline char spinnerFrame(std::size_t frame) noexcept {
    return kSpinnerFrames[frame % (sizeof(kSpinnerFrames) / sizeof(kSpinnerFrames[0]))];
}

inline void writeStatusIcon(bool finished, char spinnerChar) {
    if (finished) {
        std::cout << GREEN << "[+]" << RESET;
    } else {
        std::cout << GREEN << '[' << spinnerChar << ']' << RESET;
    }
}

inline int bouncePosition(std::size_t frame, int barWidth) noexcept {
    const int blockWidth = std::min(kBounceBlockWidth, std::max(barWidth, 0));
    const int travel = std::max(barWidth - blockWidth, 0);
    if (travel == 0) {
        return 0;
    }

    const int period = travel * 2;
    const int offset = static_cast<int>(frame % static_cast<std::size_t>(period));
    return offset <= travel ? offset : period - offset;
}

inline void writeBounceTrack(int barWidth, std::size_t frame, bool finished);

inline void drawCompactBar(const Snapshot&,
                           int terminalColumns,
                           std::size_t frame,
                           bool finished) {
    std::lock_guard<std::mutex> outputLock(outputMutex());
    std::cout << "\r\033[K";
    const char spinnerChar = spinnerFrame(frame);

    if (terminalColumns < 4) {
        std::cout << (finished ? GREEN : YELLOW)
                  << (finished ? '+' : spinnerChar)
                  << RESET << "\033[K" << std::flush;
        return;
    }

    const int barWidth = std::max(terminalColumns - kFullColumnsWithoutBar, 1);
    std::cout << YELLOW << '[' << RESET;
    writeBounceTrack(barWidth, frame, finished);
    std::cout << YELLOW << ']' << RESET;

    if (terminalColumns >= 8) {
        std::cout << ' ';
        writeStatusIcon(finished, spinnerChar);
    }

    std::cout << "\033[K" << std::flush;
}

inline void writeBounceTrack(int barWidth, std::size_t frame, bool finished) {
    const int blockWidth = std::min(kBounceBlockWidth, std::max(barWidth, 0));
    if (finished) {
        std::cout << GREEN << std::string(static_cast<std::size_t>(barWidth), '#') << RESET;
        return;
    }

    const int position = bouncePosition(frame, barWidth);
    const int after = std::max(barWidth - position - blockWidth, 0);

    std::cout << GRAY << std::string(static_cast<std::size_t>(position), '=') << RESET
              << GREEN << std::string(static_cast<std::size_t>(blockWidth), '#') << RESET
              << GRAY << std::string(static_cast<std::size_t>(after), '=') << RESET;
}

inline void drawBar(const Snapshot& snapshot, std::size_t frame, bool finished = false) {
    const int termWidth = terminalWidth();
    const char spinnerChar = spinnerFrame(frame);

    if (termWidth < kFullColumnsWithoutBar + kMinFullBarWidth) {
        drawCompactBar(snapshot, termWidth, frame, finished);
        return;
    }

    const int maxBarWidth = std::min(kMaxBarWidth, termWidth - kFullColumnsWithoutBar);
    const int barWidth = std::clamp(termWidth / 3, kMinFullBarWidth, maxBarWidth);

    const int taskMaxLength =
        std::max(0, termWidth - barWidth - kFullColumnsWithoutBar - kTaskSeparatorColumns);
    const std::string task = sanitizeTask(snapshotTask(snapshot), taskMaxLength);

    std::lock_guard<std::mutex> outputLock(outputMutex());
    std::cout << "\r\033[K" << YELLOW << "Progress: [" << RESET;
    writeBounceTrack(barWidth, frame, finished);
    std::cout << YELLOW << "] " << RESET;
    writeStatusIcon(finished, spinnerChar);

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
            drawBar(fallback, 0);
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
            drawBar(fallback, 0);
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
        drawBar(Snapshot{UiState::CUSTOM, 100.0f, 0, 1, task}, 0, true);

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

        terminalGuard_.activate();
        running_ = true;
        try {
            worker_ = std::thread(&ProgressBarController::run, this);
            return true;
        } catch (...) {
            running_ = false;
            terminalGuard_.restore(false);
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

        terminalGuard_.restore();
    }

    void run() {
        std::size_t frame = 0;
        std::unique_lock<std::mutex> lock(stateMutex_);

        while (running_) {
            const Snapshot snapshot = snapshotLocked();
            dirty_ = false;

            lock.unlock();
            drawBar(snapshot, frame++);
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
    TerminalGuard terminalGuard_;
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
