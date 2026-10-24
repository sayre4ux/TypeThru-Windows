// Builds a character -> key press table from a Windows keyboard layout,
// the counterpart of buildKeyMap() in the macOS app.
#pragma once
#include <windows.h>
#include <map>

namespace tt {

struct KeyStroke {
    UINT vk;          // virtual key
    UINT scancode;    // make-code without the extended bit
    bool extended;    // E0-prefixed key (arrows, keypad enter, right modifiers)
    bool shift;       // needs Shift held
};

class KeyMap {
public:
    // Scans a layout: unshifted pass first so "8" maps to the 8 key, not "*".
    // Keypad keys are skipped: they depend on Num Lock on the remote side.
    explicit KeyMap(HKL layout);

    const KeyStroke* find(wchar_t ch) const;
    const std::map<wchar_t, KeyStroke>& table() const { return map; }
    HKL layout() const { return layoutUsed; }

    // Replaces the stroke for one character (used for Shift+Return).
    void setOverride(wchar_t ch, const KeyStroke& stroke) { map[ch] = stroke; }

    // The character printed by a key on this layout (for shortcut names); 0 when none.
    static wchar_t keyCharacter(UINT vk, bool shift, HKL layout);

private:
    std::map<wchar_t, KeyStroke> map;
    HKL layoutUsed = 0;
};

// Newlines and Tab as physical keys, like the macOS controls table.
// Pointers to fixed entries: callers store them across the typing loop.
const KeyStroke* returnKey();
const KeyStroke* tabKey();

}
