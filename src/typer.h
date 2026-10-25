// Types text as real key presses: the Windows counterpart of Typer in main.swift.
#pragma once
#include "keymap.h"
#include "stopwatcher.h"
#include <string>
#include <vector>
#include <functional>

namespace tt {

enum class Method {
    Scancode,    // SendInput with scan codes; what remote clients accept (recommended)
    VirtualKey,  // SendInput with virtual keys only
    Unicode,     // SendInput KEYEVENTF_UNICODE text events
    PostWindow,  // WM_KEYDOWN posted to the target window; diagnostic
};

inline constexpr int methodCount = 4;
const char* methodTitle(Method method);
const char* methodShortTitle(Method method);

struct TypeResult {
    bool done = true;
    int typed = 0;
    int total = 0;
    std::wstring reason;   // set when not done
};

class Typer {
public:
    // One input plus the wait that follows it, in ms. Pure data: tests inspect, never send.
    struct Step {
        INPUT input{};
        int waitMs = 0;
    };

    static bool typeable(wchar_t ch, const KeyMap& map, Method method);
    static std::vector<wchar_t> unsupported(const std::wstring& text, const KeyMap& map, Method method);

    // Builds the inputs for one character without sending them; empty when it cannot be typed.
    static std::vector<Step> stepsFor(wchar_t ch, const KeyMap& map, Method method,
                                      int holdMs, int gapMs);

    static std::vector<Step> unicodePairSteps(wchar_t high, wchar_t low, int holdMs, int gapMs);

    // Injectable event transport: production uses SendInput, tests only collect structs.
    // On failure/cancellation, release only keys successfully pressed by this sequence.
    static std::wstring executeSteps(const std::vector<Step>& steps,
        const std::function<bool(const INPUT&)>& send,
        const std::function<std::wstring()>& stop,
        const std::function<void(int)>& wait);

    // Types `text` into the foreground target; blocks. Check stopReason only between inputs.
    static TypeResult type(const std::wstring& text, const KeyMap& map, double intervalSeconds,
                           Method method, StopWatcher& watcher);

    // Line breaks as Shift+Return, for apps where Return sends or submits.
    static KeyMap shiftReturn(KeyMap map);

    // Whether the character goes through the layout as a real key (used for hotkey names too).
    static const KeyStroke* strokeFor(wchar_t ch, const KeyMap& map);
};

// Millisecond sleep that stays accurate while typing (default timer is ~15 ms).
class TimerResolution {
public:
    explicit TimerResolution(bool enable) : enabled(enable) { if (enable) timeBeginPeriod(1); }
    ~TimerResolution() { if (enabled) timeEndPeriod(1); }
private:
    bool enabled;
};

}
