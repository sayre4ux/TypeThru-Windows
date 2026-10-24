#include "keymap.h"

namespace tt {

// Virtual keys that never produce a character through the layout. Space, digits,
// letters, and the OEM punctuation keys are the typable block and stay out of this.
static bool isControlKey(UINT vk) {
    return vk <= 0x1F                                    // backspace, tab, enter, modifiers, IME
        || (vk >= VK_PRIOR && vk <= VK_HELP)             // navigation keys through help (0x21-0x2F)
        || (vk >= VK_LWIN && vk <= VK_MODECHANGE)        // win, apps, sleep (0x5B-0x5F)
        || (vk >= VK_F1 && vk <= VK_F24)                 // function keys
        || (vk >= VK_NUMLOCK && vk <= VK_SCROLL);        // num and scroll lock
}

// Keypad keys depend on Num Lock on the remote side, so the main keys are used instead.
static bool isKeypad(UINT vk) {
    return (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE) || vk == VK_SEPARATOR;
}

// HID usage for Windows scancodes we can send (Set 1 make codes of the main keyboard).
static UINT makeCode(UINT vk, HKL layout, bool& extended) {
    UINT scan = MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC_EX, layout);
    if (scan == 0) return 0;
    // MAPVK_VK_TO_VSC_EX marks E0-prefixed keys with 0xE000, not 0x100.
    extended = (scan & 0xFF00) == 0xE000;
    return scan & 0xFF;
}

wchar_t KeyMap::keyCharacter(UINT vk, bool shift, HKL layout) {
    BYTE state[256] = {};
    if (shift) state[VK_SHIFT] = 0x80;
    wchar_t buffer[8];
    // Flag 2 keeps the layout's dead-key state untouched, like the macOS no-dead-keys option.
    int written = ToUnicodeEx(vk, MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC_EX, layout), state, buffer, 8, 2, layout);
    if (written == 1 && buffer[0] >= 0x20 && buffer[0] != 0x7F) return buffer[0];
    return 0;
}

KeyMap::KeyMap(HKL layout) : layoutUsed(layout) {
    for (int pass = 0; pass < 2; pass++) {
        bool shift = pass == 1;
        for (UINT vk = 0x08; vk < 0xFF; vk++) {
            if (isControlKey(vk) || isKeypad(vk)) continue;
            bool extended = false;
            UINT scan = makeCode(vk, layout, extended);
            if (scan == 0) continue;
            wchar_t ch = keyCharacter(vk, shift, layout);
            if (ch == 0) continue;
            if (map.find(ch) == map.end()) {
                map[ch] = KeyStroke{vk, scan, extended, shift};
            }
        }
    }
}

const KeyStroke* KeyMap::find(wchar_t ch) const {
    auto it = map.find(ch);
    return it == map.end() ? nullptr : &it->second;
}

const KeyStroke* returnKey() {
    static const KeyStroke stroke = {VK_RETURN, 0x1C, false, false};
    return &stroke;
}

const KeyStroke* tabKey() {
    static const KeyStroke stroke = {VK_TAB, 0x0F, false, false};
    return &stroke;
}

}
