#include "typer.h"
#include "shared.h"
#include <mmsystem.h>

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
    if (ch == L'\n' || ch == L'\r') return returnKey();
    if (ch == L'\t') return tabKey();
    return nullptr;
}

bool Typer::typeable(wchar_t ch, const KeyMap& map, Method method) {
    if (method == Method::Unicode) return !isHighSurrogate(ch) && !isLowSurrogate(ch);
    return strokeFor(ch, map) != nullptr;
}

std::vector<wchar_t> Typer::unsupported(const std::wstring& text, const KeyMap& map, Method method) {
    std::vector<wchar_t> missing;
    std::vector<wchar_t> seen;
    for (size_t index = 0; index < text.size(); index++) {
        wchar_t ch = text[index];
        // A well-formed surrogate pair is one typeable character in Unicode mode.
        if (method == Method::Unicode && isHighSurrogate(ch)
            && index + 1 < text.size() && isLowSurrogate(text[index + 1])) {
            index++;
            continue;
        }
        if (typeable(ch, map, method)) continue;
        if (std::find(seen.begin(), seen.end(), ch) == seen.end()) { seen.push_back(ch); missing.push_back(ch); }
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
        add(unicodeInput(static_cast<unsigned short>(ch), false), 10);
        add(unicodeInput(static_cast<unsigned short>(ch), true), gapMs > 10 ? gapMs - 10 : 0);
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

TypeResult Typer::type(const std::wstring& text, const KeyMap& map, double intervalSeconds,
                       Method method, StopWatcher& watcher) {
    TimerResolution timer(true);
    int holdMs = 0, gapMs = 0;
    timing(intervalSeconds, holdMs, gapMs);
    HWND target = GetForegroundWindow();
    TypeResult result;
    result.total = static_cast<int>(text.size());

    for (size_t index = 0; index < text.size(); index++) {
        wchar_t ch = text[index];
        if (watcher.stopReason()[0]) {
            result.done = false;
            result.typed = static_cast<int>(index);
            result.reason = watcher.stopReason();
            return result;
        }
        // A surrogate pair is one character; Unicode mode sends both units while the key is held.
        if (isHighSurrogate(ch)) {
            if (index + 1 >= text.size() || !isLowSurrogate(text[index + 1])) {
                result.done = false;
                result.typed = static_cast<int>(index);
                result.reason = L"the text contains a broken surrogate pair";
                return result;
            }
            if (method == Method::Unicode) {
                INPUT events[2] = {
                    unicodeInput(static_cast<unsigned short>(ch), false),
                    unicodeInput(static_cast<unsigned short>(text[index + 1]), false),
                };
                SendInput(2, events, sizeof(INPUT));
                INPUT ups[2] = {
                    unicodeInput(static_cast<unsigned short>(text[index + 1]), true),
                    unicodeInput(static_cast<unsigned short>(ch), true),
                };
                SendInput(2, ups, sizeof(INPUT));
                sleepMs(gapMs);
                double settle = settleAfter(ch);
                if (settle > 0) sleepMs(static_cast<int>(settle * 1000));
                index++;
                continue;
            }
            ch = text[++index];  // key methods type the pair as the low character's base layout key
        }
        if (method == Method::PostWindow) {
            const KeyStroke* stroke = nullptr;
            if (isNewlineChar(ch) || ch == L'\t') stroke = strokeFor(ch, map);
            else if (map.find(ch)) stroke = map.find(ch);
            LPARAM lParam = stroke ? (1 | (static_cast<LPARAM>(stroke->scancode) << 16)) : 0;
            WPARAM wParam = stroke ? stroke->vk : ch;
            UINT message = stroke ? WM_KEYDOWN : WM_CHAR;
            PostMessageW(target, message, wParam, lParam);
            sleepMs(holdMs);
            PostMessageW(target, WM_KEYUP, wParam, lParam | (1UL << 30) | (1UL << 31));
            sleepMs(gapMs);
        } else {
            std::vector<Step> steps = stepsFor(ch, map, method, holdMs, gapMs);
            if (steps.empty()) {
                result.done = false;
                result.typed = static_cast<int>(index);
                result.reason = L"the character has no key on the current keyboard layout";
                return result;
            }
            for (const Step& step : steps) {
                // SendInput inserting nothing means input was blocked (an elevated
                // target, or a policy); report it instead of claiming success.
                if (SendInput(1, const_cast<INPUT*>(&step.input), sizeof(INPUT)) == 0) {
                    result.done = false;
                    result.typed = static_cast<int>(index);
                    result.reason = L"Windows blocked the key press (is the target window elevated?)";
                    return result;
                }
                sleepMs(step.waitMs);
            }
        }
        // Remote apps can handle a line break after the keys that follow it, so let it land.
        double settle = settleAfter(ch);
        if (settle > 0) sleepMs(static_cast<int>(settle * 1000));
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
