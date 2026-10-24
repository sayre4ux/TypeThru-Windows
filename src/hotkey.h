// Global shortcut: a virtual key plus RegisterHotKey modifiers.
#pragma once
#include <windows.h>
#include <string>

namespace tt {

struct Shortcut {
    UINT vk = 0;
    UINT modifiers = 0;   // MOD_CONTROL / MOD_ALT / MOD_SHIFT / MOD_WIN

    // DECISION: Ctrl+Backslash by default, the same key as macOS. The hotkey consumes its key
    // but not its modifiers, which still reach the remote session; Ctrl alone is harmless there
    // and Windows apps rarely use Ctrl+\.
    static Shortcut standard();

    bool operator==(const Shortcut& other) const { return vk == other.vk && modifiers == other.modifiers; }

    // Why this cannot be the shortcut, or empty when it can.
    std::wstring problem() const;

    // A warning for shortcuts that work but may misbehave in a remote session.
    std::wstring caution() const;

    std::wstring title() const;

    // The key's label on the current layout, so "Ctrl+\" reads correctly outside the US layout.
    std::wstring keyName() const;
};

}
