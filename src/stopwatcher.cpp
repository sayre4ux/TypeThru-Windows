#include "stopwatcher.h"

namespace tt {

static bool keyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

StopWatcher::StopWatcher(HWND targetWindow) : target(targetWindow) {
    InitializeCriticalSection(&lock);
    const int buttons[3] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON};
    // Buttons already down when typing starts (finishing a click on the target field) do not count.
    for (int i = 0; i < 3; i++) wasDown[i] = keyDown(buttons[i]);
    thread = std::thread(&StopWatcher::run, this);
}

StopWatcher::~StopWatcher() {
    finish();
    if (thread.joinable()) thread.join();
    DeleteCriticalSection(&lock);
}

std::wstring StopWatcher::stopReason() const {
    EnterCriticalSection(&const_cast<CRITICAL_SECTION&>(lock));
    std::wstring copy = reason;
    LeaveCriticalSection(&lock);
    return copy;
}

void StopWatcher::finish() { finished = true; }

void StopWatcher::run() {
    const int buttons[3] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON};
    while (!finished) {
        std::wstring found;
        if (keyDown(VK_ESCAPE)) found = L"Esc was pressed";
        for (int i = 0; i < 3 && found.empty(); i++) {
            bool down = keyDown(buttons[i]);
            if (down && !wasDown[i]) found = L"the mouse was clicked";
            wasDown[i] = down;
        }
        // GetForegroundWindow is cheap, so the app and the window are checked every tick.
        if (found.empty() && GetForegroundWindow() != target) found = L"the front window changed";
        if (!found.empty()) {
            EnterCriticalSection(&lock);
            if (reason.empty()) reason = found;
            LeaveCriticalSection(&lock);
            return;
        }
        Sleep(10);
    }
}

}
