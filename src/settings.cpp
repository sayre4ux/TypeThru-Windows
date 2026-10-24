#include "settings.h"
#include "shared.h"
#include <memory>

namespace tt {

static const wchar_t* keyPath = L"Software\\TypeThru";

static HKEY openKey(bool write) {
    HKEY key = nullptr;
    RegCreateKeyExW(HKEY_CURRENT_USER, keyPath, 0, nullptr, 0,
                    write ? KEY_WRITE : KEY_READ, nullptr, &key, nullptr);
    return key;
}

static DWORD readDword(HKEY key, const wchar_t* name, DWORD fallback) {
    DWORD value = 0, size = sizeof(value), type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size)
        != ERROR_SUCCESS || type != REG_DWORD) {
        return fallback;
    }
    return value;
}

static void writeDword(HKEY key, const wchar_t* name, DWORD value) {
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

void Settings::load() {
    if (HKEY key = openKey(false)) {
        method = static_cast<Method>(readDword(key, L"Method", static_cast<DWORD>(Method::Scancode)));
        if (static_cast<int>(method) < 0 || static_cast<int>(method) >= methodCount) {
            method = Method::Scancode;   // a damaged setting falls back, never crashes
        }
        speedIndex = static_cast<int>(readDword(key, L"SpeedIndex", 0));
        if (speedIndex < 0 || speedIndex >= speedCount) speedIndex = 0;
        replaceSymbols = readDword(key, L"ReplaceSymbols", 1) != 0;
        shiftReturn = readDword(key, L"ShiftReturn", 1) != 0;
        checkUpdates = readDword(key, L"CheckUpdates", 1) != 0;
        floatButton = readDword(key, L"FloatButton", 1) != 0;
        Shortcut loaded;
        loaded.vk = readDword(key, L"ShortcutVK", Shortcut::standard().vk);
        loaded.modifiers = readDword(key, L"ShortcutMods", Shortcut::standard().modifiers);
        if (loaded.problem().empty()) shortcut = loaded;
        RegCloseKey(key);
    }
}

void Settings::save() const {
    if (HKEY key = openKey(true)) {
        writeDword(key, L"Method", static_cast<DWORD>(method));
        writeDword(key, L"SpeedIndex", static_cast<DWORD>(speedIndex));
        writeDword(key, L"ReplaceSymbols", replaceSymbols ? 1 : 0);
        writeDword(key, L"ShiftReturn", shiftReturn ? 1 : 0);
        writeDword(key, L"CheckUpdates", checkUpdates ? 1 : 0);
        writeDword(key, L"FloatButton", floatButton ? 1 : 0);
        writeDword(key, L"ShortcutVK", shortcut.vk);
        writeDword(key, L"ShortcutMods", shortcut.modifiers);
        RegCloseKey(key);
    }
}

double Settings::interval() const { return speeds[speedIndex].interval; }

static const wchar_t* runKeyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool Settings::runAtLogin() const {
    wchar_t value[MAX_PATH] = {};
    DWORD size = sizeof(value);
    LONG status = RegGetValueW(HKEY_CURRENT_USER, runKeyPath, L"TypeThru", RRF_RT_REG_SZ,
                               nullptr, value, &size);
    return status == ERROR_SUCCESS && value[0] != 0;
}

void Settings::setRunAtLogin(bool on) const {
    if (on) {
        wchar_t path[MAX_PATH + 4] = L"\"";
        GetModuleFileNameW(nullptr, path + 1, MAX_PATH);
        wcscat_s(path, L"\"");
        RegSetKeyValueW(HKEY_CURRENT_USER, runKeyPath, L"TypeThru", REG_SZ,
                        path, static_cast<DWORD>((wcslen(path) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, runKeyPath, L"TypeThru");
    }
}

std::wstring readClipboardText() {
    if (!OpenClipboard(nullptr)) {
        // The clipboard can be briefly held by another process; one retry settles it.
        Sleep(60);
        if (!OpenClipboard(nullptr)) return std::wstring();
    }
    std::wstring text;
    if (HANDLE handle = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* raw = static_cast<const wchar_t*>(GlobalLock(handle))) {
            text = raw;
            GlobalUnlock(handle);
        }
    }
    CloseClipboard();
    return text;
}

}
