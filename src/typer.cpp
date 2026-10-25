#include "typer.h"
#include "shared.h"
#include <mmsystem.h>
#include <bitset>

namespace tt {

static bool isHighSurrogate(wchar_t ch) { return ch >= 0xD800 && ch <= 0xDBFF; }
static bool isLowSurrogate(wchar_t ch)  { return ch >= 0xDC00 && ch <= 0xDFFF; }

const char* methodTitle(Method method) {
    switch (method) {
        case Method::Scancode:   return "Scan code key presses";
        case Method::VirtualKey: return "Virtual key events";
        case Method::Unicode:    return "Unicode text events";
        case Method::PostWindow: return "Posted to target window";
    }
    return "?";
}

const char* methodShortTitle(Method method) {
    return method == Method::Scancode ? "Scan code keys (recommended)" : methodTitle(method);
}

const KeyStroke* Typer::strokeFor(wchar_t ch, const KeyMap& map) {
    // The map first: shiftReturn overrides the line-break strokes there.
    if (const KeyStroke* stroke = map.find(ch)) return stroke;
    // Return and Tab stay physical keys even in Unicode mode (parity with macOS).
    if (isNewlineChar(ch)) {
        if (const KeyStroke* stroke = map.find(L'\n')) return stroke;
        return returnKey();
    }
    if (ch == L'\t') return tabKey();
    return nullptr;
}

bool Typer::typeable(wchar_t ch, const KeyMap& map, Method method) {
    if ((ch < 0x20 || (ch >= 0x7F && ch <= 0x9F)) && ch != L'\t' && !isNewlineChar(ch)) return false;
    // Posted messages cannot represent Shift+Enter safely. Refuse instead of submitting.
    if (method == Method::PostWindow && isNewlineChar(ch) && strokeFor(ch, map)->shift) return false;
    if (method == Method::Unicode) return !isHighSurrogate(ch) && !isLowSurrogate(ch);
    return strokeFor(ch, map) != nullptr;
}

std::vector<wchar_t> Typer::unsupported(const std::wstring& text, const KeyMap& map, Method method) {
    std::vector<wchar_t> missing;
    std::bitset<65536> seen;
    for (size_t index = 0; index < text.size(); index++) {
        wchar_t ch = text[index];
        // A well-formed surrogate pair is one typeable character in Unicode mode.
        if (method == Method::Unicode && isHighSurrogate(ch)
            && index + 1 < text.size() && isLowSurrogate(text[index + 1])) {
            index++;
            continue;
        }
        if (typeable(ch, map, method)) continue;
        if (!seen.test(ch)) { seen.set(ch); missing.push_back(ch); }
    }
    return missing;
}

static void sleepMs(int ms) {
    if (ms > 0) Sleep(static_cast<DWORD>(ms));
}

static INPUT unicodeInput(unsigned short unit, bool up) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = unit;
    input.ki.dwFlags = KEYEVENTF_UNICODE | (up ? KEYEVENTF_KEYUP : 0);
    return input;
}

static INPUT keyInput(Method method, UINT vk, UINT scan, bool extended, bool up) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(vk);
    input.ki.wScan = static_cast<WORD>(scan);
    if (method == Method::Scancode) {
        input.ki.dwFlags = KEYEVENTF_SCANCODE | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
    }
    if (up) input.ki.dwFlags |= KEYEVENTF_KEYUP;
    return input;
}

std::vector<Typer::Step> Typer::stepsFor(wchar_t ch, const KeyMap& map, Method method,
                                         int holdMs, int gapMs) {
    std::vector<Step> steps;
    auto add = [&steps](const INPUT& input, int wait) {
        steps.push_back(Step{input, wait});
    };

    // Unicode text events carry UTF-16 payload; surrogate pairs arrive as one character.
    if (method == Method::Unicode && !isNewlineChar(ch) && ch != L'\t') {
        if (isHighSurrogate(ch) || isLowSurrogate(ch)) return steps;  // only pairs, sent in type()
        add(unicodeInput(static_cast<unsigned short>(ch), false), holdMs);
        add(unicodeInput(static_cast<unsigned short>(ch), true), gapMs);
        return steps;
    }

    const KeyStroke* stroke = strokeFor(ch, map);
    if (stroke == nullptr) return steps;

    if (stroke->shift) {
        // DECISION: 20 ms Shift margin, like the macOS helper's Shift press before the key.
        add(keyInput(method, VK_LSHIFT, 0x2A, false, false), 20);
        add(keyInput(method, stroke->vk, stroke->scancode, stroke->extended, false), holdMs);
        add(keyInput(method, stroke->vk, stroke->scancode, stroke->extended, true), 20);
        add(keyInput(method, VK_LSHIFT, 0x2A, false, true), gapMs);
        return steps;
    }
    add(keyInput(method, stroke->vk, stroke->scancode, stroke->extended, false), holdMs);
    add(keyInput(method, stroke->vk, stroke->scancode, stroke->extended, true), gapMs);
    return steps;
}

std::vector<Typer::Step> Typer::unicodePairSteps(wchar_t high, wchar_t low, int holdMs, int gapMs) {
    if (!isHighSurrogate(high) || !isLowSurrogate(low)) return {};
    return {{unicodeInput(high, false), 0}, {unicodeInput(low, false), holdMs},
            {unicodeInput(low, true), 0}, {unicodeInput(high, true), gapMs}};
}

std::wstring Typer::executeSteps(const std::vector<Step>& steps,
    const std::function<bool(const INPUT&)>& send,
    const std::function<std::wstring()>& stop,
    const std::function<void(int)>& wait) {
    std::vector<INPUT> held;
    held.reserve(steps.size());
    auto release = [&] {
        // DECISION: never clear physical modifiers; release only our successful key-downs.
        for (auto it = held.rbegin(); it != held.rend(); ++it) {
            INPUT up = *it;
            up.ki.dwFlags |= KEYEVENTF_KEYUP;
            send(up);
        }
    };
    for (const Step& step : steps) {
        std::wstring reason = stop();
        if (!reason.empty()) { release(); return reason; }
        if (!send(step.input)) {
            release();
            return L"Windows blocked input; check target permissions (key release is best effort)";
        }
        if (step.input.ki.dwFlags & KEYEVENTF_KEYUP) {
            auto matching = std::find_if(held.begin(), held.end(), [&](const INPUT& down) {
                return down.ki.wVk == step.input.ki.wVk && down.ki.wScan == step.input.ki.wScan
                    && down.ki.dwFlags == (step.input.ki.dwFlags & ~KEYEVENTF_KEYUP);
            });
            if (matching != held.end()) held.erase(matching);
        } else {
            held.push_back(step.input);
        }
        for (int remaining = step.waitMs; remaining > 0;) {
            int slice = remaining > 10 ? 10 : remaining;
            wait(slice);
            remaining -= slice;
            reason = stop();
            if (!reason.empty()) { release(); return reason; }
        }
    }
    release();
    return {};
}

TypeResult Typer::type(const std::wstring& source, const KeyMap& map, double intervalSeconds,
                       Method method, StopWatcher& watcher) {
    const std::wstring text = withoutTrailingLineBreaks(normalizeLineBreaks(source));
    TypeResult result;
    result.total = static_cast<int>(text.size());
    if (!unsupported(text, map, method).empty()) {
        result.done = false;
        result.reason = L"the text cannot be represented safely with this method and layout";
        return result;
    }
    TimerResolution timer(true);
    int holdMs = 0, gapMs = 0;
    timing(intervalSeconds, holdMs, gapMs);
    const HWND target = watcher.targetWindow();
    HWND recipient = target;
    if (method == Method::PostWindow) {
        GUITHREADINFO info{sizeof(info)};
        DWORD thread = GetWindowThreadProcessId(target, nullptr);
        if (thread && GetGUIThreadInfo(thread, &info) && info.hwndFocus
            && (info.hwndFocus == target || IsChild(target, info.hwndFocus))) recipient = info.hwndFocus;
    }
    auto stop = [&]() -> std::wstring {
        std::wstring reason = watcher.stopReason();
        if (!reason.empty()) return reason;
        if (!target || GetForegroundWindow() != target) return L"the front window changed";
        if ((method == Method::Scancode || method == Method::VirtualKey)
            && ((GetKeyState(VK_CAPITAL) & 1) || (GetAsyncKeyState(VK_CAPITAL) & 0x8000)))
            return L"Caps Lock is on; turn it off before typing";
        for (int modifier : {VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN}) {
            if (GetAsyncKeyState(modifier) & 0x8000) return L"a modifier key was pressed";
        }
        return {};
    };
    auto send = [](const INPUT& input) {
        INPUT copy = input;
        return SendInput(1, &copy, sizeof(copy)) == 1;
    };
    auto pause = [&](int ms) {
        for (int remaining = ms; remaining > 0 && stop().empty(); remaining -= 10)
            sleepMs(remaining > 10 ? 10 : remaining);
    };
    // No modifier clearing: the floating T button handles full-screen keyboard grabs.
    pause(150);
    for (size_t index = 0; index < text.size(); index++) {
        wchar_t ch = text[index];
        std::wstring reason = stop();
        size_t units = 1;
        if (reason.empty() && (GetAsyncKeyState(VK_SHIFT) & 0x8000))
            reason = L"Shift was pressed before the next character";
        if (reason.empty()) {
            if (method == Method::PostWindow) {
                // WM_KEYDOWN cannot carry case or Shift state across thread queues.
                // Posted text is diagnostic only; Shift+Enter is refused by preflight.
                WPARAM character = isNewlineChar(ch) ? L'\r' : ch;
                if (!PostMessageW(recipient, WM_CHAR, character, 1))
                    reason = L"Windows blocked the posted character";
                pause(holdMs + gapMs);
            } else {
                std::vector<Step> steps;
                if (method == Method::Unicode && isHighSurrogate(ch)) {
                    steps = unicodePairSteps(ch, text[index + 1], holdMs, gapMs);
                    units = 2;
                } else {
                    steps = stepsFor(ch, map, method, holdMs, gapMs);
                }
                reason = executeSteps(steps, send, stop, sleepMs);
            }
        }
        if (!reason.empty()) {
            result.done = false;
            result.reason = reason;
            return result;
        }
        result.typed += static_cast<int>(units);
        index += units - 1;
        pause(static_cast<int>(settleAfter(ch) * 1000));
    }
    return result;
}

KeyMap Typer::shiftReturn(KeyMap map) {
    KeyStroke shifted = *returnKey();
    shifted.shift = true;
    for (wchar_t ch : {L'\n', L'\r'}) map.setOverride(ch, shifted);
    return map;
}

}
