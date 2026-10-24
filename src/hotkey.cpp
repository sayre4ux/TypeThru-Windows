#include "hotkey.h"
#include "keymap.h"

namespace tt {

static bool hasFlag(UINT modifiers, UINT flag) { return (modifiers & flag) != 0; }

static bool isFunctionKey(UINT vk) { return vk >= VK_F1 && vk <= VK_F20; }

Shortcut Shortcut::standard() { return Shortcut{VK_OEM_1, MOD_CONTROL}; }

std::wstring Shortcut::problem() const {
    if (vk == VK_ESCAPE) return L"Esc stops typing, so it cannot start it.";
    if (!isFunctionKey(vk) && !hasFlag(modifiers, MOD_CONTROL) && !hasFlag(modifiers, MOD_ALT)
        && !hasFlag(modifiers, MOD_WIN)) {
        return L"Add Ctrl, Alt, or Win, or use a function key such as F6.";
    }
    if (vk == VK_SPACE && modifiers == 0) return L"Choose a key other than Space.";
    return std::wstring();
}

std::wstring Shortcut::caution() const {
    // DECISION: same reasoning as macOS: Alt opens menu bars in the remote session, Win opens
    // the Start menu, Shift with Ctrl can switch layouts. Ctrl alone is safest.
    if (hasFlag(modifiers, MOD_ALT) || hasFlag(modifiers, MOD_WIN) || hasFlag(modifiers, MOD_SHIFT)) {
        return L"Alt, Win, and Shift also reach the remote session and can trigger Windows shortcuts. Ctrl alone is safest.";
    }
    return std::wstring();
}

std::wstring Shortcut::keyName() const {
    if (isFunctionKey(vk)) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    if (vk == VK_RETURN) return L"Enter";
    if (vk == VK_TAB) return L"Tab";
    if (vk == VK_SPACE) return L"Space";
    if (vk == VK_BACK) return L"Backspace";
    if (vk == VK_LEFT) return L"Left";
    if (vk == VK_RIGHT) return L"Right";
    if (vk == VK_UP) return L"Up";
    if (vk == VK_DOWN) return L"Down";
    if (vk == VK_HOME) return L"Home";
    if (vk == VK_END) return L"End";
    if (vk == VK_PRIOR) return L"PgUp";
    if (vk == VK_NEXT) return L"PgDn";
    HKL layout = GetKeyboardLayout(0);
    if (wchar_t ch = KeyMap::keyCharacter(vk, false, layout)) {
        return std::wstring(1, towupper(ch));
    }
    if (wchar_t ch = KeyMap::keyCharacter(vk, true, layout)) {
        return std::wstring(1, towupper(ch));
    }
    return L"Key " + std::to_wstring(vk);
}

std::wstring Shortcut::title() const {
    std::wstring text;
    if (hasFlag(modifiers, MOD_CONTROL)) text += L"Ctrl+";
    if (hasFlag(modifiers, MOD_ALT)) text += L"Alt+";
    if (hasFlag(modifiers, MOD_SHIFT)) text += L"Shift+";
    if (hasFlag(modifiers, MOD_WIN)) text += L"Win+";
    return text + keyName();
}

}
