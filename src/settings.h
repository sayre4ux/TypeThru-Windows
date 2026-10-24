// Saved settings, in HKCU\Software\TypeThru (no admin, nothing machine-wide).
#pragma once
#include "hotkey.h"
#include "typer.h"
#include <string>

namespace tt {

struct Settings {
    Method method = Method::Scancode;
    int speedIndex = 0;
    bool replaceSymbols = true;
    bool shiftReturn = true;    // DECISION: on by default, like macOS; chat apps get a new line
    bool checkUpdates = true;
    // DECISION: on by default. Full-screen VDI clients grab every key, so a local
    // hotkey cannot reach us; the click button is the one trigger that always works.
    bool floatButton = true;
    Shortcut shortcut = Shortcut::standard();

    void load();
    void save() const;

    double interval() const;

    // The Run-key entry in HKCU; true when TypeThru starts at login.
    bool runAtLogin() const;
    void setRunAtLogin(bool on) const;
};

// Reads the clipboard once, as text. Never writes or keeps it around.
std::wstring readClipboardText();

}
