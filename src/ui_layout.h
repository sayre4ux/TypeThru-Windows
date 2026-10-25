#pragma once

#include <array>
#include "version.h"

namespace tt::ui_layout {

enum ControlId : int {
    IDC_STATUS = 100, IDC_BRAND, IDC_TITLE, IDC_SPEED_LABEL, IDC_SPEED,
    IDC_SPEED_DETAIL, IDC_SHIFTENTER, IDC_TYPE, IDC_TYPE_HELP, IDC_TEST_LABEL,
    IDC_TEST_HELP, IDC_UPDATE_STATUS, IDC_INSTALL, IDC_SETTINGS_BTN, IDC_QUIT,
    IDC_TEST_SHORT, IDC_TEST_CAPITALS, IDC_TEST_SYMBOLS, IDC_TEST_SMART,
    IDC_TEST_LONG,
    IDS_SHORTCUT_LABEL = 200, IDS_NOTE, IDS_CHANGE, IDS_RESET, IDS_RUNLOGIN,
    IDS_CHECKUPDATES, IDS_UNINSTALL, IDS_DONE, IDS_METHOD_LABEL, IDS_METHOD,
    IDS_REPLACE, IDS_SHIFTENTER, IDS_FLOATBTN, IDS_VERSION_LABEL, IDS_UPDATE_STATUS,
    IDS_CHECKNOW, IDS_INSTALL, IDS_DIAGNOSTICS, IDS_SETTINGS_TITLE, IDS_SETTINGS_SUBTITLE,
    IDS_GENERAL_HEADING, IDS_TYPING_HEADING, IDS_UPDATES_HEADING, IDS_METHOD_HINT,
    IDS_SHIFTENTER_HINT, IDS_SHORTCUT_VALUE, IDS_SETTINGS_BRAND,
};

struct Placement {
    int id;
    int x;
    int y;
    int w;
    int h;
};

inline const wchar_t* label(int id) {
    switch (id) {
        case IDC_BRAND: case IDC_TITLE: case IDS_SETTINGS_BRAND: return L"TypeThru";
        case IDC_STATUS: return L"Ready";
        case IDC_TYPE: return L"Type Clipboard";
        case IDC_TYPE_HELP: return L"Esc, click, or switching apps stops typing.";
        case IDC_SPEED_LABEL: return L"Speed";
        case IDC_SHIFTENTER: case IDS_SHIFTENTER: return L"Type line breaks as Shift+Enter";
        case IDC_TEST_LABEL: return L"Typing tests";
        case IDC_TEST_HELP: return L"Starts in 3 seconds. Click the target field first.";
        case IDC_TEST_SHORT: return L"Short";
        case IDC_TEST_CAPITALS: return L"Capitals";
        case IDC_TEST_SYMBOLS: return L"Symbols";
        case IDC_TEST_SMART: return L"Smart symbols";
        case IDC_TEST_LONG: return L"Long";
        case IDC_SETTINGS_BTN: case IDS_SETTINGS_TITLE: return L"Settings";
        case IDC_QUIT: return L"Quit";
        case IDS_SETTINGS_SUBTITLE: return L"Adjust typing behavior, shortcuts and updates.";
        case IDS_GENERAL_HEADING: return L"General";
        case IDS_SHORTCUT_LABEL: return L"Shortcut";
        case IDS_SHORTCUT_VALUE: return L"Ctrl+;";
        case IDS_NOTE: return L"Ctrl-only shortcuts are safest for remote sessions.";
        case IDS_CHANGE: return L"Change";
        case IDS_RESET: return L"Reset";
        case IDS_RUNLOGIN: return L"Open TypeThru at login";
        case IDS_FLOATBTN: return L"Show the floating Type button for full-screen sessions";
        case IDS_TYPING_HEADING: return L"Typing";
        case IDS_METHOD_LABEL: return L"Typing method";
        case IDS_METHOD_HINT: return L"Scan code is recommended for VDI and remote desktops.";
        case IDS_REPLACE: return L"Replace symbols without a key (• → -)";
        case IDS_SHIFTENTER_HINT: return L"Adds a line in chat apps. Turn off for spreadsheets.";
        case IDS_UPDATES_HEADING: return L"Updates";
        case IDS_CHECKUPDATES: return L"Check for updates automatically";
        case IDS_VERSION_LABEL: return L"Version  " TYPETHRU_VERSION_STRING;
        case IDS_CHECKNOW: return L"Check Now";
        case IDS_UPDATE_STATUS: return L"Automatic checks are on.";
        case IDS_DIAGNOSTICS: return L"Diagnostics";
        case IDS_UNINSTALL: return L"Uninstall TypeThru";
        case IDC_INSTALL: case IDS_INSTALL: return L"Install";
        case IDS_DONE: return L"Done";
        default: return nullptr;
    }
}

struct Surface {
    int x;
    int y;
    int w;
    int h;
};

inline constexpr int panelWidth = 384;
inline constexpr int floatingSize = 40;
inline constexpr int panelHeight(bool updateOffer) { return updateOffer ? 520 : 478; }
inline constexpr int settingsWidth = 560;
inline constexpr int settingsHeight = 652;
inline constexpr int bodyFontPoints = 10;
inline constexpr int captionFontPoints = 9;
inline constexpr int brandTitleFontPoints = 15;
inline constexpr int titleFontPoints = 18;
inline constexpr int primaryFontPoints = 12;
inline constexpr int comboSelectionHeight = 32;
inline constexpr int comboItemHeight = 30;

inline constexpr std::array<Placement, 20> panel(bool updateOffer) {
    int footerY = updateOffer ? 474 : 432;
    return {{
        {IDC_BRAND, 20, 18, 32, 32},
        {IDC_TITLE, 62, 16, 150, 36},
        {IDC_STATUS, 220, 20, 144, 28},
        {IDC_TYPE, 20, 68, 344, 52},
        {IDC_TYPE_HELP, 20, 126, 344, 20},
        {IDC_SPEED_LABEL, 20, 160, 56, 32},
        {IDC_SPEED, 88, 158, 276, 32},
        {IDC_SPEED_DETAIL, 20, 198, 344, 24},
        {IDC_SHIFTENTER, 20, 234, 344, 28},
        {IDC_TEST_LABEL, 20, 278, 344, 22},
        {IDC_TEST_HELP, 20, 301, 344, 20},
        {IDC_TEST_SHORT, 20, 332, 108, 36},
        {IDC_TEST_CAPITALS, 138, 332, 108, 36},
        {IDC_TEST_SYMBOLS, 256, 332, 108, 36},
        {IDC_TEST_SMART, 20, 376, 226, 36},
        {IDC_TEST_LONG, 256, 376, 108, 36},
        {IDC_UPDATE_STATUS, 20, 424, 254, 32},
        {IDC_INSTALL, 280, 423, 84, 34},
        {IDC_SETTINGS_BTN, 20, footerY, 108, 32},
        {IDC_QUIT, 284, footerY, 80, 32},
    }};
}

inline constexpr std::array<Surface, 3> settingsSurfaces = {{
    {20, 112, 520, 144},
    {20, 296, 520, 158},
    {20, 484, 520, 106},
}};

inline constexpr bool buttonUsesSurface(int id) {
    return id == IDS_CHANGE || id == IDS_RESET || id == IDS_CHECKNOW || id == IDS_INSTALL;
}

inline constexpr int settingsDividerY = 600;

inline constexpr std::array<Placement, 27> settings(int extraWidth = 0) {
    return {{
        {IDS_SETTINGS_BRAND, 30, 24, 24, 24},
        {IDS_SETTINGS_TITLE, 64, 22, 468 + extraWidth, 30},
        {IDS_SETTINGS_SUBTITLE, 30, 54, 500 + extraWidth, 20},
        {IDS_GENERAL_HEADING, 30, 88, 500 + extraWidth, 22},
        {IDS_SHORTCUT_LABEL, 32, 124, 74, 30},
        {IDS_SHORTCUT_VALUE, 114, 123, 142, 30},
        {IDS_CHANGE, 334 + extraWidth, 121, 88, 32},
        {IDS_RESET, 430 + extraWidth, 121, 98, 32},
        {IDS_NOTE, 32, 157, 496 + extraWidth, 30},
        {IDS_RUNLOGIN, 32, 190, 496 + extraWidth, 28},
        {IDS_FLOATBTN, 32, 221, 496 + extraWidth, 28},
        {IDS_TYPING_HEADING, 30, 272, 500 + extraWidth, 22},
        {IDS_METHOD_LABEL, 32, 309, 190, 28},
        {IDS_METHOD, 250, 305, 278 + extraWidth, 32},
        {IDS_METHOD_HINT, 32, 338, 496 + extraWidth, 18},
        {IDS_REPLACE, 32, 362, 496 + extraWidth, 28},
        {IDS_SHIFTENTER, 32, 392, 496 + extraWidth, 28},
        {IDS_SHIFTENTER_HINT, 52, 420, 476 + extraWidth, 22},
        {IDS_UPDATES_HEADING, 30, 462, 500 + extraWidth, 22},
        {IDS_CHECKUPDATES, 32, 495, 496 + extraWidth, 28},
        {IDS_VERSION_LABEL, 32, 527, 280, 24},
        {IDS_CHECKNOW, 324 + extraWidth, 523, 92, 30},
        {IDS_INSTALL, 424 + extraWidth, 523, 104, 30},
        {IDS_UPDATE_STATUS, 32, 557, 496 + extraWidth, 24},
        {IDS_DIAGNOSTICS, 32, 608, 106, 32},
        {IDS_UNINSTALL, 148, 608, 140, 32},
        {IDS_DONE, 432 + extraWidth, 608, 96, 32},
    }};
}

} // namespace tt::ui_layout
