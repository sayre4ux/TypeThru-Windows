// Stops typing when the user acts: Esc, a mouse click, or a different window in front.
// The Windows counterpart of StopWatcher in main.swift; polls on its own thread so a
// quick tap is noticed even while a key is held.
#pragma once
#include <windows.h>
#include <string>
#include <atomic>
#include <thread>

namespace tt {

class StopWatcher {
public:
    explicit StopWatcher(HWND targetWindow);
    ~StopWatcher();

    // Empty while typing should continue; otherwise the human-readable stop reason.
    std::wstring stopReason() const;

    void finish();

private:
    void run();
    std::atomic<bool> finished{false};
    std::wstring reason;          // guarded by the mutex below
    mutable CRITICAL_SECTION lock;
    HWND target = nullptr;
    bool wasDown[3] = {false, false, false};  // left, right, middle at start
    std::thread thread;
};

}
