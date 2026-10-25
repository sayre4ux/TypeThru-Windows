// Tray application: icon, panel, settings window, trigger flow, updates.
// The Windows counterpart of AppDelegate in main.swift.
#include "app.h"
#include "settings.h"
#include "shared.h"
#include "keymap.h"
#include "typer.h"
#include "stopwatcher.h"
#include "hotkey.h"
#include "updates.h"
#include "ui.h"
#include "ui_layout.h"
#include "version.h"
#include <commctrl.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <atomic>
#include <map>
#include <optional>
#include <thread>
#include <cstdio>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

namespace tt {

namespace {

using namespace ui_layout;

// ASCII titles to wide text for the common controls.
std::wstring utf8(const char* text) {
    int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring wide(static_cast<size_t>(size - 1), 0);
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), size);
    return wide;
}

// ---------- identifiers ----------

constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT WM_APP_TYPEDONE = WM_APP + 2;   // wParam = new TypeResult*, lParam = target name wchar_t*
constexpr UINT WM_APP_UPDATECHECKED = WM_APP + 3;
constexpr UINT WM_APP_UPDATEINSTALLED = WM_APP + 4;   // lParam = new std::wstring error or nullptr
constexpr UINT WM_APP_CLOSEPANEL = WM_APP + 5;
constexpr UINT HOTKEY_ID = 1;
constexpr UINT TIMER_FIRST_CHECK = 1;
constexpr UINT TIMER_HOURLY = 2;
constexpr wchar_t shortcutDefaultNote[] = L"Ctrl-only shortcuts are safest for remote sessions.";

// ---------- state ----------

HINSTANCE g_instance = nullptr;
HWND g_window = nullptr;          // hidden window: tray, hotkey, timers
HWND g_panel = nullptr;
HWND g_settingsWindow = nullptr;
HWND g_floatButton = nullptr;     // always-on-top click trigger for full-screen VDI
NOTIFYICONDATAW g_tray = {};
bool g_trayIconOwned = false;
Settings g_settings;
std::atomic<bool> g_busy{false};
std::atomic<bool> g_installing{false};
std::wstring g_lastAttempt = L"No attempt in this session.";
std::wstring g_hotkeyError;
bool g_hotkeyRegistered = false;
bool g_recording = false;
bool g_settingsPositioned = false;
int g_settingsScrollPos = 0;
int g_settingsWheelDelta = 0;
UINT g_settingsDpi = 96;
bool g_checkingUpdates = false;
DWORD g_lastHotKeyTick = 0;
std::optional<Release> g_updateOffer;
std::wstring g_updateStatus = L" ";
ULONGLONG g_lastUpdateCheck = 0;

// ---------- helpers ----------

void showPanel();
void closePanel();
void syncPanel();
void syncSettingsWindow();
void showSettingsWindow();
void showDiagnostics();

using UiPalette = ui::Palette;

UiPalette g_palette{};
HBRUSH g_panelBrush = nullptr;
HBRUSH g_surfaceBrush = nullptr;
bool g_bufferedPaintReady = false;
bool g_uiReady = false;

struct UiFontSet {
    HFONT body = nullptr;
    HFONT bold = nullptr;
    HFONT caption = nullptr;
    HFONT brandTitle = nullptr;
    HFONT title = nullptr;
    HFONT primary = nullptr;
};

std::map<UINT, UiFontSet> g_fontSets;

static HPAINTBUFFER beginBufferedPaint(HDC target, const RECT& bounds, HDC& dc) {
    dc = target;
    return g_bufferedPaintReady
        ? BeginBufferedPaint(target, &bounds, BPBF_COMPATIBLEBITMAP, nullptr, &dc)
        : nullptr;
}

static bool highContrastEnabled() {
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0)
        && (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

static bool systemPrefersDark() {
    DWORD light = 1, size = sizeof(light);
    LONG result = RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    if (result != ERROR_SUCCESS) {
        size = sizeof(light);
        result = RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    }
    return result == ERROR_SUCCESS && light == 0;
}

static void refreshUiPalette() {
    bool contrast = highContrastEnabled();
    bool dark = !contrast && systemPrefersDark();
    g_palette = ui::paletteFor(dark, contrast);
    ui::setPalette(g_palette);
    if (g_panelBrush) DeleteObject(g_panelBrush);
    if (g_surfaceBrush) DeleteObject(g_surfaceBrush);
    g_panelBrush = CreateSolidBrush(g_palette.background);
    g_surfaceBrush = CreateSolidBrush(g_palette.surface);
}

static HFONT makeUiFont(int points, int weight, UINT dpi) {
    int height = -MulDiv(points, static_cast<int>(dpi ? dpi : 96), 72);
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

static UiFontSet& fontsForDpi(UINT dpi) {
    if (!dpi) dpi = 96;
    auto found = g_fontSets.find(dpi);
    if (found != g_fontSets.end()) return found->second;
    UiFontSet fonts;
    fonts.body = makeUiFont(ui_layout::bodyFontPoints, FW_NORMAL, dpi);
    fonts.bold = makeUiFont(ui_layout::bodyFontPoints, FW_SEMIBOLD, dpi);
    fonts.caption = makeUiFont(ui_layout::captionFontPoints, FW_NORMAL, dpi);
    fonts.brandTitle = makeUiFont(ui_layout::brandTitleFontPoints, FW_SEMIBOLD, dpi);
    fonts.title = makeUiFont(ui_layout::titleFontPoints, FW_SEMIBOLD, dpi);
    fonts.primary = makeUiFont(ui_layout::primaryFontPoints, FW_SEMIBOLD, dpi);
    return g_fontSets.emplace(dpi, fonts).first->second;
}

static void applyFontsToWindow(HWND window) {
    if (!window) return;
    UiFontSet& fonts = fontsForDpi(GetDpiForWindow(window));
    for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        int id = GetDlgCtrlID(child);
        HFONT font = fonts.body;
        if (id == IDS_METHOD_LABEL || id == IDS_VERSION_LABEL
            || id == IDS_GENERAL_HEADING || id == IDS_TYPING_HEADING || id == IDS_UPDATES_HEADING)
            font = fonts.bold;
        if (id == IDC_TITLE) font = fonts.brandTitle;
        if (id == IDS_SETTINGS_TITLE) font = fonts.title;
        if (id == IDC_TYPE) font = fonts.primary;
        if (id == IDC_STATUS || id == IDC_SPEED_DETAIL || id == IDC_TEST_HELP
            || id == IDC_TYPE_HELP || id == IDC_UPDATE_STATUS || id == IDS_NOTE
            || id == IDS_UPDATE_STATUS || id == IDS_SETTINGS_SUBTITLE || id == IDS_METHOD_HINT
            || id == IDS_SHIFTENTER_HINT) font = fonts.caption;
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

static void applyWindowAppearance(HWND window, bool roundedPanel) {
    if (!window) return;
    BOOL dark = g_palette.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(window, static_cast<DWMWINDOWATTRIBUTE>(20), &dark, sizeof(dark));
    int corner = 2; // DWMWCP_ROUND; DWM supplies smooth corners without a jagged GDI region.
    DwmSetWindowAttribute(window, static_cast<DWMWINDOWATTRIBUTE>(33), &corner, sizeof(corner));
    if (!roundedPanel) {
        COLORREF caption = g_palette.background;
        COLORREF text = g_palette.text;
        DwmSetWindowAttribute(window, static_cast<DWMWINDOWATTRIBUTE>(35), &caption, sizeof(caption));
        DwmSetWindowAttribute(window, static_cast<DWMWINDOWATTRIBUTE>(36), &text, sizeof(text));
    }
}

void alert(const std::wstring& title, const std::wstring& message) {
    MessageBoxW(g_panel ? g_panel : g_window, message.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

void refreshTrayTip() {
    wcscpy_s(g_tray.szTip, g_busy ? L"TypeThru - typing..." : L"TypeThru");
    Shell_NotifyIconW(NIM_MODIFY, &g_tray);
}

// Waits until Ctrl, Alt, Win, and Shift are all released (up to 3 seconds).
// The trigger VK is a copy: the UI thread may change the setting while we wait.
static bool waitBeforeTyping(int ms, bool& cancelled) {
    for (int elapsed = 0; elapsed <= ms; elapsed += 10) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) cancelled = true;
        if (cancelled) return false;
        if (elapsed < ms) Sleep(10);
    }
    return true;
}

bool waitForModifierRelease(UINT triggerVk, bool includeTriggerKey, bool& cancelled) {
    for (int i = 0; i < 150; i++) {
        if (!waitBeforeTyping(0, cancelled)) return false;
        const int modifiers[5] = {VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN};
        bool allUp = true;
        for (int vk : modifiers) {
            if (GetAsyncKeyState(vk) & 0x8000) { allUp = false; break; }
        }
        if (allUp && includeTriggerKey && triggerVk && (GetAsyncKeyState(triggerVk) & 0x8000)) allUp = false;
        if (allUp) return true;
        if (!waitBeforeTyping(20, cancelled)) return false;
    }
    return false;
}

std::wstring windowDescription(HWND window) {
    // Window titles can contain document text; diagnostics keep only the class name.
    wchar_t className[64] = L"";
    GetClassNameW(window, className, 64);
    return className[0] ? className : std::wstring(L"unknown");
}

static bool tokenIsElevated(HANDLE token) {
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    return GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)
        && elevation.TokenIsElevated != 0;
}

bool processIsElevated() {
    HANDLE token = nullptr;
    bool elevated = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        elevated = tokenIsElevated(token);
        CloseHandle(token);
    }
    return elevated;
}

// Whether the target window's process runs elevated; injected input cannot reach it.
bool windowIsElevated(HWND window) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (!pid) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    HANDLE token = nullptr;
    bool elevated = false;
    if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
        elevated = tokenIsElevated(token);
        CloseHandle(token);
    }
    CloseHandle(process);
    return elevated;
}

std::wstring timeNow() {
    SYSTEMTIME time;
    GetLocalTime(&time);
    wchar_t text[64];
    swprintf_s(text, L"%02d:%02d:%02d", time.wHour, time.wMinute, time.wSecond);
    return text;
}

// ---------- typing ----------

struct TypedDone {
    TypeResult result;
    wchar_t targetName[128];
};

void setBusy(bool busy) {
    g_busy = busy;
    refreshTrayTip();
    syncPanel();
    syncSettingsWindow();
}

// Waits until the foreground window has been the same window for 300 ms, up to
// 2 s. Releasing a full-screen VDI client's keyboard grab (the Ctrl+Alt escape)
// leaves focus flickering through a transition, and keys sent into it are dropped.
static HWND settledForegroundWindow(bool& cancelled) {
    HWND stable = GetForegroundWindow();
    int sameFor = 0;
    for (int i = 0; i < 20 && sameFor < 3; i++) {
        if (!waitBeforeTyping(100, cancelled)) return nullptr;
        HWND now = GetForegroundWindow();
        if (now == stable) sameFor++;
        else { stable = now; sameFor = 0; }
    }
    return sameFor >= 3 ? stable : nullptr;
}

// The typing run: everything after the checks in trigger() happens off the UI thread.
void runTyping(std::wstring text, KeyMap map, double interval, Method method,
               bool fromMenu, int settleBeforeMs, UINT triggerVk, HWND initialTarget) {
    std::thread([text = std::move(text), map = std::move(map), interval, method, fromMenu,
                 settleBeforeMs, triggerVk, initialTarget]() mutable {
        bool cancelled = false;
        waitBeforeTyping(settleBeforeMs, cancelled);
        // DECISION: the shortcut path starts on release, so the remote client receives the
        // physical key-up sequence before the injected keys begin (same reason as macOS).
        bool released = !cancelled && waitForModifierRelease(triggerVk, !fromMenu, cancelled);
        if (released) released = waitBeforeTyping(fromMenu ? 150 : 750, cancelled);
        TypedDone* done = new TypedDone();
        if (!released) {
            done->result.done = false;
            done->result.total = static_cast<int>(text.size());
            done->result.reason = cancelled ? L"Esc was pressed during the countdown"
                                           : L"shortcut or modifier keys were not released";
        } else {
            HWND target = settledForegroundWindow(cancelled);
            DWORD pid = 0;
            GetWindowThreadProcessId(target, &pid);
            HKL targetLayout = target ? GetKeyboardLayout(GetWindowThreadProcessId(target, nullptr)) : nullptr;
            if (!target || pid == GetCurrentProcessId() || (!fromMenu && target != initialTarget)) {
                done->result.done = false;
                done->result.total = static_cast<int>(text.size());
                done->result.reason = cancelled ? L"Esc was pressed before typing"
                    : L"the target window was unavailable or changed before typing";
            } else if (targetLayout != map.layout()) {
                done->result.done = false;
                done->result.total = static_cast<int>(text.size());
                done->result.reason = L"the keyboard layout changed; select the target field and try again";
            } else if (windowIsElevated(target) && !processIsElevated()) {
                // UIPI quietly blocks our input into elevated windows; say so up front.
                done->result.done = false;
                done->result.total = static_cast<int>(text.size());
                done->result.reason = L"the target window runs elevated; open TypeThru as administrator to type into it";
                wcscpy_s(done->targetName, windowDescription(target).substr(0, 127).c_str());
            } else {
                wcscpy_s(done->targetName, windowDescription(target).substr(0, 127).c_str());
                StopWatcher watcher(target);
                done->result = Typer::type(text, map, interval, method, watcher);
                watcher.finish();
            }
        }
        if (!PostMessageW(g_window, WM_APP_TYPEDONE, reinterpret_cast<WPARAM>(done), 0)) delete done;
    }).detach();
}

// The active layout of the window that will receive the keys, not of this thread.
static HKL layoutForWindow(HWND window) {
    DWORD thread = GetWindowThreadProcessId(window, nullptr);
    return thread ? GetKeyboardLayout(thread) : GetKeyboardLayout(0);
}

void trigger(bool fromMenu, const TypingTest* test) {
    if (g_busy || g_installing) { MessageBeep(MB_ICONEXCLAMATION); return; }
    Method method = g_settings.method;
    HWND initialTarget = GetForegroundWindow();
    if ((method == Method::Scancode || method == Method::VirtualKey) && (GetKeyState(VK_CAPITAL) & 1)) {
        alert(L"Turn off Caps Lock", L"Turn off Caps Lock on this PC and in the remote session, then try again.");
        return;
    }
    g_lastAttempt = L"Attempt: " + timeNow() + L"\nTrigger: "
                  + (test ? utf8(test->title) + L" test"
                          : (fromMenu ? L"panel" : L"shortcut"))
                  + L"\nMethod: " + utf8(methodShortTitle(method));

    std::wstring copied = test ? test->text : readClipboardText();
    std::wstring original = withoutTrailingLineBreaks(normalizeLineBreaks(copied));
    if (original.empty()) {
        g_lastAttempt += L"\nNot started: clipboard has no text.";
        alert(L"Nothing typed", L"The clipboard has no text. You can run a typing test from the TypeThru panel.");
        return;
    }
    if (original.size() < copied.size()) g_lastAttempt += L"\nLeft out the line break at the end.";

    KeyMap map(layoutForWindow(initialTarget));
    if (g_settings.shiftReturn) {
        map = Typer::shiftReturn(std::move(map));
        g_lastAttempt += L"\nLine breaks: Shift+Enter.";
    }
    if (g_settings.replaceSymbols && method != Method::Unicode) {
        int replaced = 0;
        original = replacingUntypable(original, replaced,
                                      [&map, method](wchar_t ch) { return Typer::typeable(ch, map, method); });
        if (replaced > 0) g_lastAttempt += L"\nReplaced " + std::to_wstring(replaced) + L" symbols that have no key.";
    }
    std::vector<wchar_t> missing = Typer::unsupported(original, map, method);
    if (!missing.empty()) {
        g_lastAttempt += L"\nNot started: unsupported characters.";
        alert(L"Nothing typed", method == Method::PostWindow && g_settings.shiftReturn
              ? L"Posted messages cannot send Shift+Enter. Choose Scan code keys or Unicode text events."
              : L"Some characters have no key on the current keyboard layout. "
              L"Try Unicode text events, or check your keyboard layout.");
        return;
    }

    double interval = (test && !test->usesSelectedSpeed) ? speeds[0].interval : g_settings.interval();
    g_lastAttempt += L"\nSpeed: " + std::to_wstring(static_cast<int>(interval * 1000)) + L" ms per character";
    setBusy(true);
    // Panel runs give the user time to click the target field; tests give more, for the switch.
    int settle = test ? 3000 : (fromMenu ? 1000 : 0);
    runTyping(original, std::move(map), interval, method, fromMenu, settle, g_settings.shortcut.vk, initialTarget);
}

// ---------- hotkey ----------

bool registerHotKey(const Shortcut& shortcut) {
    if (g_hotkeyRegistered) {
        UnregisterHotKey(g_window, HOTKEY_ID);
        g_hotkeyRegistered = false;
    }
    // MOD_NOREPEAT keeps a held key from firing the hotkey twice.
    if (RegisterHotKey(g_window, HOTKEY_ID, shortcut.modifiers | MOD_NOREPEAT, shortcut.vk)) {
        g_settings.shortcut = shortcut;
        g_hotkeyRegistered = true;
        g_hotkeyError.clear();
    } else {
        g_hotkeyError = L"Another app may use this shortcut.";
    }
    return g_hotkeyRegistered;
}

// ---------- updates ----------

void runUpdateCheck(bool manual) {
    if (g_installing || g_checkingUpdates) return;
    FILETIME timestamp{};
    GetSystemTimeAsFileTime(&timestamp);
    ULARGE_INTEGER current{};
    current.LowPart = timestamp.dwLowDateTime;
    current.HighPart = timestamp.dwHighDateTime;
    if (!manual) {
        if (!g_settings.checkUpdates) return;
        if (g_lastUpdateCheck != 0 && (current.QuadPart <= g_lastUpdateCheck
            || current.QuadPart - g_lastUpdateCheck < 24ULL * 3600 * 10000000ULL)) return;
    }
    // Save every attempt, including failures, so restart/retry cannot exceed the daily policy.
    g_lastUpdateCheck = current.QuadPart;
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\TypeThru", L"LastUpdateAttempt", REG_QWORD,
                   &g_lastUpdateCheck, sizeof(g_lastUpdateCheck));
    g_checkingUpdates = true;
    g_updateStatus = L"Checking...";
    syncPanel();
    syncSettingsWindow();
    std::thread([] {
        Release* found = new Release();
        UpdateOutcome outcome = updater::checkForNewer(TYPETHRU_VERSION_STRING, *found);
        if (!PostMessageW(g_window, WM_APP_UPDATECHECKED, static_cast<WPARAM>(outcome),
                          reinterpret_cast<LPARAM>(found))) {
            delete found;
        }
    }).detach();
}

void installUpdate() {
    if (g_busy || g_installing || g_checkingUpdates || !g_updateOffer) { MessageBeep(MB_ICONEXCLAMATION); return; }
    g_installing = true;
    g_updateStatus = L"Downloading...";
    syncPanel();
    syncSettingsWindow();
    Release wanted = *g_updateOffer;
    std::thread([wanted] {
        // A saved offer carries no download links; fetch the live release again,
        // the same re-check the macOS app does before installing.
        Release release = wanted;
        if (release.zipUrl.empty() || release.sigUrl.empty()) {
            Release found;
            if (updater::checkForNewer(TYPETHRU_VERSION_STRING, found) == UpdateOutcome::Available) {
                release = found;
            }
        }
        std::wstring error = release.zipUrl.empty()
            ? std::wstring(L"Could not reach GitHub to confirm the download. Try again later.")
            : updater::install(release);
        auto* payload = new std::wstring(std::move(error));
        if (!PostMessageW(g_window, WM_APP_UPDATEINSTALLED, 0, reinterpret_cast<LPARAM>(payload))) {
            delete payload;
        }
    }).detach();
}

// ---------- panel ----------

int panelScale(HWND window, int value) {
    UINT dpi = GetDpiForWindow(window);
    return MulDiv(value, static_cast<int>(dpi ? dpi : 96), 96);
}

void placeControl(HWND panel, int id, int x, int y, int w, int h) {
    HWND control = GetDlgItem(panel, id);
    SetWindowPos(control, nullptr, panelScale(panel, x), panelScale(panel, y),
                 panelScale(panel, w), panelScale(panel, h), SWP_NOZORDER | SWP_NOACTIVATE);
}

static int panelContentHeight() {
    return ui_layout::panelHeight(g_updateOffer.has_value());
}

static void drawPanelChrome(HWND panel, HDC dc) {
    RECT client{};
    GetClientRect(panel, &client);
    ui::drawSurface(dc, client, GetDpiForWindow(panel));
}

static void setComboItemHeights(HWND combo) {
    if (!combo) return;
    UINT dpi = GetDpiForWindow(combo);
    ComboBox_SetItemHeight(combo, -1, MulDiv(ui_layout::comboSelectionHeight,
        static_cast<int>(dpi ? dpi : 96), 96));
    ComboBox_SetItemHeight(combo, 0, MulDiv(ui_layout::comboItemHeight,
        static_cast<int>(dpi ? dpi : 96), 96));
    int count = ComboBox_GetCount(combo);
    if (count > 0) ComboBox_SetMinVisible(combo, count);
}

static void resizePanel(HWND panel) {
    if (!panel) return;
    RECT bounds{};
    GetWindowRect(panel, &bounds);
    int width = panelScale(panel, ui_layout::panelWidth);
    int height = panelScale(panel, panelContentHeight());
    POINT center{bounds.left + (bounds.right - bounds.left) / 2,
                 bounds.top + (bounds.bottom - bounds.top) / 2};
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    HMONITOR monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
    if (GetMonitorInfoW(monitor, &info)) {
        RECT area = info.rcWork;
        if (bounds.left + width > area.right) bounds.left = area.right - width - 8;
        if (bounds.top + height > area.bottom) bounds.top = area.bottom - height - 8;
        bounds.left = max(area.left + 8, bounds.left);
        bounds.top = max(area.top + 8, bounds.top);
    }
    SetWindowPos(panel, nullptr, bounds.left, bounds.top, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    applyWindowAppearance(panel, true);
}

void layoutPanel(HWND panel) {
    for (const Placement& item : ui_layout::panel(g_updateOffer.has_value()))
        placeControl(panel, item.id, item.x, item.y, item.w, item.h);
}

void syncPanel() {
    if (g_floatButton) InvalidateRect(g_floatButton, nullptr, FALSE);
    if (!g_panel) return;
    CheckDlgButton(g_panel, IDC_SHIFTENTER, g_settings.shiftReturn ? BST_CHECKED : BST_UNCHECKED);
    HWND speed = GetDlgItem(g_panel, IDC_SPEED);
    if (ComboBox_GetCount(speed) == 0) {
        for (int i = 0; i < speedCount; i++) {
            ComboBox_AddString(speed, utf8(speeds[i].title).c_str());
        }
        setComboItemHeights(speed);
    }
    ComboBox_SetCurSel(speed, g_settings.speedIndex);
    SetDlgItemTextW(g_panel, IDC_SPEED_DETAIL, utf8(speeds[g_settings.speedIndex].detail).c_str());
    std::wstring typeText = L"Type Clipboard";
    if (g_hotkeyRegistered) typeText += L"     " + g_settings.shortcut.title();
    SetDlgItemTextW(g_panel, IDC_TYPE, typeText.c_str());
    std::wstring updateText = g_checkingUpdates || g_installing ? g_updateStatus
        : g_updateOffer ? L"Update available: " + g_updateOffer->version : g_updateStatus;
    SetDlgItemTextW(g_panel, IDC_UPDATE_STATUS, updateText.c_str());
    ShowWindow(GetDlgItem(g_panel, IDC_UPDATE_STATUS), g_updateOffer ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_panel, IDC_INSTALL), g_updateOffer ? SW_SHOW : SW_HIDE);
    EnableWindow(GetDlgItem(g_panel, IDC_INSTALL),
                 g_updateOffer.has_value() && !g_busy && !g_checkingUpdates && !g_installing);
    EnableWindow(GetDlgItem(g_panel, IDC_TYPE), !g_busy && !g_installing);
    for (int id : {IDC_TEST_SHORT, IDC_TEST_CAPITALS, IDC_TEST_SYMBOLS, IDC_TEST_SMART, IDC_TEST_LONG})
        EnableWindow(GetDlgItem(g_panel, id), !g_busy && !g_installing);
    EnableWindow(GetDlgItem(g_panel, IDC_QUIT), !g_busy && !g_installing);
    // Status reports shortcut health; the floating T remains the full-screen trigger.
    std::wstring status = g_installing ? L"Updating" : g_busy ? L"Typing"
        : !g_hotkeyRegistered ? L"Shortcut off" : L"Ready";
    SetDlgItemTextW(g_panel, IDC_STATUS, status.c_str());
    layoutPanel(g_panel);
    if (IsWindowVisible(g_panel)) resizePanel(g_panel);
    RedrawWindow(g_panel, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

// The panel closes when the user clicks anywhere outside it, like the macOS panel.
// The panel is WS_EX_NOACTIVATE (the remote client must keep focus), so no focus or
// activation message ever reaches it; instead a WH_MOUSE_LL hook runs only while the
// panel is visible, sees nothing but click positions, and is removed on close.
HHOOK g_panelClickHook = nullptr;
DWORD g_panelClosedAt = 0;

static bool windowBelongsToPanel(HWND window) {
    if (!window) return false;
    // Children (buttons, combos) root to the panel through the parent chain.
    HWND root = GetAncestor(window, GA_ROOT);
    if (root == g_panel) return true;
    // Combo dropdown lists are top-level windows owned by controls in the panel;
    // follow owner links and re-root each one.
    for (HWND owner = GetWindow(root, GW_OWNER); owner; owner = GetWindow(owner, GW_OWNER)) {
        if (owner == g_panel || GetAncestor(owner, GA_ROOT) == g_panel) return true;
    }
    return false;
}

static LRESULT CALLBACK panelClickHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION
        && (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN || wParam == WM_MBUTTONDOWN)) {
        POINT point = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam)->pt;
        if (!windowBelongsToPanel(WindowFromPoint(point))) {
            PostMessageW(g_window, WM_APP_CLOSEPANEL, 0, 0);
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void showPanel() {
    if (!g_panel) {
        g_panel = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                  L"TypeThruPanel", L"TypeThru", WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                  0, 0, 0, 0, g_window, nullptr, g_instance, nullptr);
    }
    refreshUiPalette();
    syncPanel();
    POINT cursor = {};
    GetCursorPos(&cursor);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    RECT area{};
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    if (GetMonitorInfoW(monitor, &info)) area = info.rcWork;
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    int width = panelScale(g_panel, ui_layout::panelWidth), height = panelScale(g_panel, panelContentHeight());
    int x = cursor.x - width - 12;
    int y = cursor.y - height - 12;
    if (x < area.left + 8) x = cursor.x + 12;
    x = max(area.left + 8, min(x, area.right - width - 8));
    y = max(area.top + 8, min(y, area.bottom - height - 8));
    SetWindowPos(g_panel, HWND_TOPMOST, x, y, width, height,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    applyWindowAppearance(g_panel, true);
    layoutPanel(g_panel);
    // The panel never takes focus, so the remote client stays in front and ready.
    RedrawWindow(g_panel, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    if (!g_panelClickHook) g_panelClickHook = SetWindowsHookExW(WH_MOUSE_LL, panelClickHook, nullptr, 0);
}

void closePanel() {
    if (g_panel) ShowWindow(g_panel, SW_HIDE);
    // A click on the tray icon also lands outside the panel; remember when we closed
    // so the tray message does not immediately reopen it (same guard as macOS).
    g_panelClosedAt = GetTickCount();
    if (g_panelClickHook) {
        UnhookWindowsHookEx(g_panelClickHook);
        g_panelClickHook = nullptr;
    }
}

// ---------- floating Type button ----------

// Full-screen VDI clients grab every key before local hotkeys run, so a click is
// the one trigger that always reaches us. A small always-on-top button, off in the
// corner: click = Type Clipboard (then click the target field), drag = move it,
// right-click = hide (bring it back from the panel).
constexpr int floatButtonSize = ui_layout::floatingSize;

static void saveFloatPosition(int x, int y) {
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\TypeThru", L"FloatX", REG_DWORD,
                    &x, sizeof(x));
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\TypeThru", L"FloatY", REG_DWORD,
                    &y, sizeof(y));
}

static POINT savedFloatPosition(int size) {
    int x = 0, y = 0;
    bool hasX = false, hasY = false;
    DWORD value = 0, sizeOut = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\TypeThru", L"FloatX", RRF_RT_REG_DWORD,
                     nullptr, &value, &sizeOut) == ERROR_SUCCESS) {
        x = static_cast<int>(value);
        hasX = true;
    }
    sizeOut = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\TypeThru", L"FloatY", RRF_RT_REG_DWORD,
                     nullptr, &value, &sizeOut) == ERROR_SUCCESS) {
        y = static_cast<int>(value);
        hasY = true;
    }
    bool saved = hasX && hasY;
    POINT anchor{x, y};
    if (!saved || (x == 0 && y == 0)) GetCursorPos(&anchor);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    RECT area{};
    HMONITOR monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    if (GetMonitorInfoW(monitor, &info)) area = info.rcWork;
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    if (!saved || (x == 0 && y == 0)) {   // first run: low corner of the active monitor
        x = area.right - size - 24;
        y = area.bottom - size - 24;
    }
    // Clamp whatever we have back into the visible work area.
    if (x < area.left) x = area.left + 8;
    if (y < area.top) y = area.top + 8;
    if (x + size > area.right) x = area.right - size - 8;
    if (y + size > area.bottom) y = area.bottom - size - 8;
    return POINT{x, y};
}

static LRESULT CALLBACK floatButtonProc(HWND button, UINT message, WPARAM wParam, LPARAM lParam) {
    static bool dragging = false;
    static bool movedSincePress = false;
    static POINT pressAt = {};
    static POINT windowAt = {};
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC target = BeginPaint(button, &paint);
            RECT client{};
            GetClientRect(button, &client);
            HDC dc = target;
            HPAINTBUFFER buffer = beginBufferedPaint(target, client, dc);
            ui::drawFloatingButton(dc, client, GetDpiForWindow(button), ui::hotProgress(button),
                                   dragging && !movedSincePress, g_busy || g_installing);
            if (buffer) EndBufferedPaint(buffer, TRUE);
            EndPaint(button, &paint);
            return 0;
        }
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        case WM_LBUTTONDOWN:
            dragging = true;
            movedSincePress = false;
            GetCursorPos(&pressAt);
            RECT current;
            GetWindowRect(button, &current);
            windowAt = POINT{current.left, current.top};
            SetCapture(button);
            InvalidateRect(button, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE:
            if (dragging) {
                POINT at;
                GetCursorPos(&at);
                if (abs(at.x - pressAt.x) + abs(at.y - pressAt.y) > 4) movedSincePress = true;
                if (movedSincePress) {
                    SetWindowPos(button, nullptr, windowAt.x + at.x - pressAt.x,
                                 windowAt.y + at.y - pressAt.y, 0, 0,
                                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                }
                InvalidateRect(button, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP: {
            if (!dragging) return 0;
            bool moved = false;
            if (dragging) {
                dragging = false;
                ReleaseCapture();
                POINT now;
                GetCursorPos(&now);
                moved = movedSincePress || abs(now.x - pressAt.x) + abs(now.y - pressAt.y) > 4;
            }
            if (moved) {
                RECT placed;
                GetWindowRect(button, &placed);
                saveFloatPosition(placed.left, placed.top);
            } else if (!g_busy && !g_installing) {
                // Same flow as the panel's Type Clipboard: click the target field next.
                trigger(true, nullptr);
            } else {
                MessageBeep(MB_ICONEXCLAMATION);
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            dragging = false;
            InvalidateRect(button, nullptr, FALSE);
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_RBUTTONUP:
            g_settings.floatButton = false;
            g_settings.save();
            ShowWindow(button, SW_HIDE);
            syncPanel();
            return 0;
        case WM_DPICHANGED: {
            RECT* suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(button, HWND_TOPMOST, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOACTIVATE);
            applyWindowAppearance(button, true);
            return 0;
        }
        case WM_SETTINGCHANGE:
        case WM_SYSCOLORCHANGE:
        case WM_THEMECHANGED:
            refreshUiPalette();
            applyWindowAppearance(button, true);
            RedrawWindow(button, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
            return 0;
    }
    return DefWindowProcW(button, message, wParam, lParam);
}

void applyFloatButton() {
    if (g_settings.floatButton) {
        if (!g_floatButton) {
            int size = floatButtonSize * GetDpiForSystem() / 96;
            POINT at = savedFloatPosition(size);
            g_floatButton = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                            L"TypeThruFloat", L"Type clipboard", WS_POPUP,
                                            at.x, at.y, size, size, g_window, nullptr, g_instance, nullptr);
            ui::installControl(g_floatButton, ui::ControlKind::Button);
            applyWindowAppearance(g_floatButton, true);
            INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
            InitCommonControlsEx(&controls);
            HWND tip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, TOOLTIPS_CLASSW, nullptr,
                WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0,
                g_floatButton, nullptr, g_instance, nullptr);
            if (tip) {
                TOOLINFOW tool{sizeof(tool)};
                tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
                tool.hwnd = g_floatButton;
                tool.uId = reinterpret_cast<UINT_PTR>(g_floatButton);
                tool.lpszText = const_cast<wchar_t*>(L"Type clipboard\nDrag to move. Right-click to hide.");
                SendMessageW(tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
                SendMessageW(tip, TTM_SETMAXTIPWIDTH, 0, panelScale(g_floatButton, 220));
            }
        }
        ShowWindow(g_floatButton, SW_SHOWNOACTIVATE);
    } else if (g_floatButton) {
        ShowWindow(g_floatButton, SW_HIDE);
    }
}

static void drawButtonItem(const DRAWITEMSTRUCT* item, bool primary) {
    RECT bounds = item->rcItem;
    HDC dc = item->hDC;
    HPAINTBUFFER buffer = beginBufferedPaint(item->hDC, bounds, dc);
    if (!buffer) dc = item->hDC;
    UINT state = item->itemState;
    bool disabled = (state & ODS_DISABLED) != 0;
    float hover = disabled ? 0.0f : ui::hotProgress(item->hwndItem);
    bool pressed = (state & ODS_SELECTED) != 0;
    UINT dpi = GetDpiForWindow(item->hwndItem);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(item->hwndItem, WM_GETFONT, 0, 0));
    int id = GetDlgCtrlID(item->hwndItem);
    bool quiet = id == IDC_SETTINGS_BTN || id == IDC_QUIT || id == IDS_DIAGNOSTICS
        || id == IDS_UNINSTALL || id == IDS_DONE;
    bool prominent = primary || id == IDC_INSTALL || id == IDS_INSTALL;
    std::wstring label(128, L'\0');
    int length = GetWindowTextW(item->hwndItem, label.data(), static_cast<int>(label.size()));
    label.resize(length > 0 ? static_cast<size_t>(length) : 0);
    ui::drawButton(dc, bounds, label.c_str(), font, dpi, prominent,
                   disabled, hover, pressed, (state & ODS_FOCUS) != 0, quiet,
                   ui_layout::buttonUsesSurface(id));
    if (buffer) EndBufferedPaint(buffer, TRUE);
}

static void drawBrandItem(const DRAWITEMSTRUCT* item) {
    RECT bounds = item->rcItem;
    HDC dc = item->hDC;
    HPAINTBUFFER buffer = beginBufferedPaint(item->hDC, bounds, dc);
    if (!buffer) dc = item->hDC;
    ui::drawBrand(dc, bounds);
    if (buffer) EndBufferedPaint(buffer, TRUE);
}

static void drawStatusItem(const DRAWITEMSTRUCT* item) {
    RECT bounds = item->rcItem;
    int dpi = static_cast<int>(GetDpiForWindow(item->hwndItem));
    HDC dc = item->hDC;
    HPAINTBUFFER buffer = beginBufferedPaint(item->hDC, bounds, dc);
    if (!buffer) dc = item->hDC;
    FillRect(dc, &bounds, g_panelBrush ? g_panelBrush : GetSysColorBrush(COLOR_WINDOW));
    ui::rounded(dc, bounds, static_cast<float>((bounds.bottom - bounds.top) / 2),
                g_palette.surface, g_palette.border);
    std::wstring label(96, L'\0');
    int length = GetWindowTextW(item->hwndItem, label.data(), static_cast<int>(label.size()));
    label.resize(length > 0 ? static_cast<size_t>(length) : 0);
    COLORREF dot = label == L"Ready" ? g_palette.success : g_palette.warning;
    int diameter = MulDiv(7, dpi, 96);
    int y = bounds.top + ((bounds.bottom - bounds.top) - diameter) / 2;
    int x = bounds.left + MulDiv(10, dpi, 96);
    RECT dotBounds{x, y, x + diameter, y + diameter};
    ui::rounded(dc, dotBounds, static_cast<float>(diameter / 2), dot, dot);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(item->hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, g_palette.text);
    RECT textRect{bounds.left + MulDiv(23, dpi, 96), bounds.top,
                  bounds.right - MulDiv(8, dpi, 96), bounds.bottom};
    DrawTextW(dc, label.c_str(), static_cast<int>(label.size()), &textRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (oldFont) SelectObject(dc, oldFont);
    if (buffer) EndBufferedPaint(buffer, TRUE);
}

static void drawShortcutValueItem(const DRAWITEMSTRUCT* item) {
    RECT bounds = item->rcItem;
    UINT dpi = GetDpiForWindow(item->hwndItem);
    HDC dc = item->hDC;
    HPAINTBUFFER buffer = beginBufferedPaint(item->hDC, bounds, dc);
    if (!buffer) dc = item->hDC;
    FillRect(dc, &bounds, g_surfaceBrush ? g_surfaceBrush : GetSysColorBrush(COLOR_WINDOW));
    ui::rounded(dc, bounds, static_cast<float>((bounds.bottom - bounds.top) / 2),
                g_palette.surface, g_palette.border);
    std::wstring label(96, L'\0');
    int length = GetWindowTextW(item->hwndItem, label.data(), static_cast<int>(label.size()));
    label.resize(length > 0 ? static_cast<size_t>(length) : 0);
    HFONT font = fontsForDpi(dpi).bold;
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, g_palette.text);
    DrawTextW(dc, label.c_str(), static_cast<int>(label.size()), &bounds,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (oldFont) SelectObject(dc, oldFont);
    if (buffer) EndBufferedPaint(buffer, TRUE);
}

static void drawComboItem(const DRAWITEMSTRUCT* item) {
    if (item->itemState & ODS_COMBOBOXEDIT) {
        InvalidateRect(item->hwndItem, nullptr, FALSE);
        return;
    }
    if (item->itemID == static_cast<UINT>(-1)) return;
    HWND combo = item->hwndItem;
    int length = static_cast<int>(SendMessageW(combo, CB_GETLBTEXTLEN, item->itemID, 0));
    if (length < 0) return;
    std::wstring label(static_cast<size_t>(length) + 1, L'\0');
    SendMessageW(combo, CB_GETLBTEXT, item->itemID, reinterpret_cast<LPARAM>(label.data()));
    label.resize(static_cast<size_t>(length));
    ui::drawComboItem(item->hDC, item->rcItem, label.c_str(),
                      reinterpret_cast<HFONT>(SendMessageW(combo, WM_GETFONT, 0, 0)),
                      GetDpiForWindow(combo), (item->itemState & ODS_SELECTED) != 0,
                      (item->itemState & ODS_DISABLED) != 0);
}

LRESULT CALLBACK panelProc(HWND panel, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            auto add = [panel](const wchar_t* klass, const wchar_t* text, DWORD style, int id) {
                HWND control = CreateWindowExW(0, klass, text, WS_CHILD | WS_VISIBLE | style,
                    0, 0, 0, 0, panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
                if (control) {
                    if (id == IDC_SPEED) ui::installControl(control, ui::ControlKind::Combo, false);
                    else if (id == IDC_SHIFTENTER)
                        ui::installControl(control, ui::ControlKind::Toggle, false);
                    else if (id == IDC_TYPE || id == IDC_INSTALL || id == IDC_SETTINGS_BTN
                        || id == IDC_QUIT || id == IDC_TEST_SHORT || id == IDC_TEST_CAPITALS
                        || id == IDC_TEST_SYMBOLS || id == IDC_TEST_SMART || id == IDC_TEST_LONG)
                        ui::installControl(control, ui::ControlKind::Button,
                            id == IDC_SETTINGS_BTN || id == IDC_QUIT);
                }
                return control;
            };
            add(L"STATIC", ui_layout::label(IDC_BRAND), SS_OWNERDRAW, IDC_BRAND);
            add(L"STATIC", ui_layout::label(IDC_TITLE), SS_LEFT | SS_NOPREFIX, IDC_TITLE);
            add(L"STATIC", ui_layout::label(IDC_STATUS), SS_OWNERDRAW, IDC_STATUS);
            add(L"BUTTON", ui_layout::label(IDC_TYPE), BS_OWNERDRAW | WS_TABSTOP, IDC_TYPE);
            add(L"STATIC", ui_layout::label(IDC_TYPE_HELP), SS_LEFT | SS_NOPREFIX, IDC_TYPE_HELP);
            add(L"STATIC", ui_layout::label(IDC_SPEED_LABEL), SS_LEFT | SS_NOPREFIX, IDC_SPEED_LABEL);
            add(L"COMBOBOX", ui_layout::label(IDC_SPEED), CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS
                | WS_VSCROLL | WS_TABSTOP, IDC_SPEED);
            add(L"STATIC", ui_layout::label(IDC_SPEED_DETAIL), SS_LEFT | SS_NOPREFIX, IDC_SPEED_DETAIL);
            add(L"BUTTON", ui_layout::label(IDC_SHIFTENTER), BS_AUTOCHECKBOX | WS_TABSTOP, IDC_SHIFTENTER);
            add(L"STATIC", ui_layout::label(IDC_TEST_LABEL), SS_LEFT | SS_NOPREFIX, IDC_TEST_LABEL);
            add(L"STATIC", ui_layout::label(IDC_TEST_HELP), SS_LEFT | SS_NOPREFIX, IDC_TEST_HELP);
            add(L"BUTTON", ui_layout::label(IDC_TEST_SHORT), BS_OWNERDRAW | WS_TABSTOP, IDC_TEST_SHORT);
            add(L"BUTTON", ui_layout::label(IDC_TEST_CAPITALS), BS_OWNERDRAW | WS_TABSTOP, IDC_TEST_CAPITALS);
            add(L"BUTTON", ui_layout::label(IDC_TEST_SYMBOLS), BS_OWNERDRAW | WS_TABSTOP, IDC_TEST_SYMBOLS);
            add(L"BUTTON", ui_layout::label(IDC_TEST_SMART), BS_OWNERDRAW | WS_TABSTOP, IDC_TEST_SMART);
            add(L"BUTTON", ui_layout::label(IDC_TEST_LONG), BS_OWNERDRAW | WS_TABSTOP, IDC_TEST_LONG);
            add(L"STATIC", ui_layout::label(IDC_UPDATE_STATUS), SS_LEFT | SS_NOPREFIX, IDC_UPDATE_STATUS);
            add(L"BUTTON", ui_layout::label(IDC_INSTALL), BS_OWNERDRAW | WS_TABSTOP, IDC_INSTALL);
            add(L"BUTTON", ui_layout::label(IDC_SETTINGS_BTN), BS_OWNERDRAW | WS_TABSTOP, IDC_SETTINGS_BTN);
            add(L"BUTTON", ui_layout::label(IDC_QUIT), BS_OWNERDRAW | WS_TABSTOP, IDC_QUIT);
            applyFontsToWindow(panel);
            setComboItemHeights(GetDlgItem(panel, IDC_SPEED));
            applyWindowAppearance(panel, true);
            layoutPanel(panel);
            return 0;
        }
        case WM_DRAWITEM: {
            auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (item->CtlID == IDC_BRAND) drawBrandItem(item);
            else if (item->CtlID == IDC_STATUS) drawStatusItem(item);
            else if (item->CtlID == IDC_TYPE) drawButtonItem(item, true);
            else if (item->CtlType == ODT_COMBOBOX) drawComboItem(item);
            else if (item->CtlType == ODT_BUTTON) drawButtonItem(item, false);
            return TRUE;
        }
        case WM_MEASUREITEM: {
            auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (item->CtlType != ODT_COMBOBOX) break;
            UINT dpi = GetDpiForWindow(panel);
            item->itemHeight = static_cast<UINT>(MulDiv(ui_layout::comboItemHeight,
                static_cast<int>(dpi ? dpi : 96), 96));
            return TRUE;
        }
        case WM_COMMAND: {
            WORD id = LOWORD(wParam);
            WORD code = HIWORD(wParam);
            if (id == IDC_TYPE && code == BN_CLICKED) {
                closePanel();
                trigger(true, nullptr);
            } else if (code == BN_CLICKED && (id == IDC_TEST_SHORT || id == IDC_TEST_CAPITALS
                || id == IDC_TEST_SYMBOLS || id == IDC_TEST_SMART || id == IDC_TEST_LONG)) {
                int index = id == IDC_TEST_SHORT ? 0 : id == IDC_TEST_CAPITALS ? 1
                    : id == IDC_TEST_SYMBOLS ? 2 : id == IDC_TEST_SMART ? 3 : 4;
                const auto& tests = typingTests();
                if (index >= 0 && index < static_cast<int>(tests.size())) {
                    closePanel();
                    trigger(true, &tests[index]);
                }
            } else if (id == IDC_SPEED && code == CBN_SELENDOK) {
                int index = ComboBox_GetCurSel(GetDlgItem(panel, IDC_SPEED));
                if (index >= 0 && index < speedCount) {
                    g_settings.speedIndex = index;
                    g_settings.save();
                    SetDlgItemTextW(panel, IDC_SPEED_DETAIL, utf8(speeds[index].detail).c_str());
                }
            } else if (id == IDC_SHIFTENTER && code == BN_CLICKED) {
                g_settings.shiftReturn = IsDlgButtonChecked(panel, IDC_SHIFTENTER) == BST_CHECKED;
                g_settings.save();
            } else if (id == IDC_SETTINGS_BTN && code == BN_CLICKED) {
                closePanel();
                showSettingsWindow();
            } else if (id == IDC_INSTALL && code == BN_CLICKED) {
                installUpdate();
                syncPanel();
            } else if (id == IDC_QUIT && code == BN_CLICKED) {
                if (g_busy || g_installing) {
                    MessageBeep(MB_ICONEXCLAMATION);
                    return 0;
                }
                PostQuitMessage(0);
            }
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            HWND control = reinterpret_cast<HWND>(lParam);
            bool listBox = message == WM_CTLCOLORLISTBOX;
            SetBkMode(dc, TRANSPARENT);
            int id = control ? GetDlgCtrlID(control) : 0;
            COLORREF text = id == IDC_TYPE_HELP || id == IDC_SPEED_DETAIL
                || id == IDC_TEST_HELP || id == IDC_UPDATE_STATUS ? g_palette.muted : g_palette.text;
            SetTextColor(dc, text);
            SetBkColor(dc, listBox ? g_palette.surface : g_palette.background);
            HBRUSH brush = listBox ? g_surfaceBrush : g_panelBrush;
            if (id == IDC_STATUS) brush = g_surfaceBrush;
            return reinterpret_cast<LRESULT>(brush ? brush : GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC target = BeginPaint(panel, &paint);
            RECT client{};
            GetClientRect(panel, &client);
            HDC dc = target;
            HPAINTBUFFER buffer = beginBufferedPaint(target, client, dc);
            drawPanelChrome(panel, dc);
            if (buffer) EndBufferedPaint(buffer, TRUE);
            EndPaint(panel, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DPICHANGED: {
            RECT* suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(panel, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            applyFontsToWindow(panel);
            setComboItemHeights(GetDlgItem(panel, IDC_SPEED));
            layoutPanel(panel);
            resizePanel(panel);
            RedrawWindow(panel, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
            return 0;
        }
        case WM_SETTINGCHANGE:
        case WM_SYSCOLORCHANGE:
        case WM_THEMECHANGED:
            refreshUiPalette();
            applyWindowAppearance(panel, true);
            RedrawWindow(panel, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
    }
    return DefWindowProcW(panel, message, wParam, lParam);
}

// ---------- settings window ----------

void syncSettingsWindow() {
    if (!g_settingsWindow) return;
    SetDlgItemTextW(g_settingsWindow, IDS_SHORTCUT_LABEL, L"Shortcut");
    SetDlgItemTextW(g_settingsWindow, IDS_SHORTCUT_VALUE, g_settings.shortcut.title().c_str());
    CheckDlgButton(g_settingsWindow, IDS_RUNLOGIN, g_settings.runAtLogin() ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_settingsWindow, IDS_CHECKUPDATES, g_settings.checkUpdates ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_settingsWindow, IDS_REPLACE, g_settings.replaceSymbols ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_settingsWindow, IDS_SHIFTENTER, g_settings.shiftReturn ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_settingsWindow, IDS_FLOATBTN, g_settings.floatButton ? BST_CHECKED : BST_UNCHECKED);
    HWND method = GetDlgItem(g_settingsWindow, IDS_METHOD);
    if (method) ComboBox_SetCurSel(method, static_cast<int>(g_settings.method));
    std::wstring version = L"Version  " TYPETHRU_VERSION_STRING;
    SetDlgItemTextW(g_settingsWindow, IDS_VERSION_LABEL, version.c_str());
    std::wstring updateText = g_checkingUpdates || g_installing ? g_updateStatus : g_updateOffer
        ? L"Version " + g_updateOffer->version + L" is available."
        : g_updateStatus != L" " ? g_updateStatus
        : g_settings.checkUpdates ? L"Automatic checks are on."
        : L"Automatic checks are off.";
    SetDlgItemTextW(g_settingsWindow, IDS_UPDATE_STATUS, updateText.c_str());
    SetDlgItemTextW(g_settingsWindow, IDS_INSTALL, g_installing ? L"Installing..." : L"Install update");
    ShowWindow(GetDlgItem(g_settingsWindow, IDS_INSTALL), g_updateOffer ? SW_SHOW : SW_HIDE);
    EnableWindow(GetDlgItem(g_settingsWindow, IDS_CHECKNOW), !g_checkingUpdates && !g_installing);
    EnableWindow(GetDlgItem(g_settingsWindow, IDS_INSTALL), g_updateOffer.has_value()
                 && !g_busy && !g_checkingUpdates && !g_installing);
    EnableWindow(GetDlgItem(g_settingsWindow, IDS_UNINSTALL), !g_busy && !g_installing);
    EnableWindow(GetDlgItem(g_settingsWindow, IDS_RESET), !(g_settings.shortcut == Shortcut::standard()));
    wchar_t note[256] = L"";
    GetDlgItemTextW(g_settingsWindow, IDS_NOTE, note,
                    static_cast<int>(sizeof(note) / sizeof(note[0])));
    if (!g_recording && !g_hotkeyRegistered && !g_hotkeyError.empty()
        && (note[0] == L'\0' || wcscmp(note, shortcutDefaultNote) == 0)) {
        SetDlgItemTextW(g_settingsWindow, IDS_NOTE,
                        (L"Shortcut unavailable. " + g_hotkeyError).c_str());
    } else if (!g_recording && note[0] == L'\0') {
        SetDlgItemTextW(g_settingsWindow, IDS_NOTE, shortcutDefaultNote);
    }
    RedrawWindow(g_settingsWindow, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

static constexpr int settingsMinimumHeight = 320;

static int settingsScrollLimit(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    return max(0, panelScale(window, ui_layout::settingsHeight) - client.bottom);
}

static void updateSettingsScrollBar(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    info.nMin = 0;
    info.nMax = max(0, panelScale(window, ui_layout::settingsHeight) - 1);
    info.nPage = static_cast<UINT>(max(0, client.bottom));
    int limit = max(0, info.nMax - static_cast<int>(info.nPage) + 1);
    g_settingsScrollPos = min(max(0, g_settingsScrollPos), limit);
    info.nPos = g_settingsScrollPos;
    SetScrollInfo(window, SB_VERT, &info, TRUE);
}

static void layoutSettingsWindow(HWND window) {
    updateSettingsScrollBar(window);
    RECT client{};
    GetClientRect(window, &client);
    UINT dpi = GetDpiForWindow(window);
    int contentWidth = max(ui_layout::settingsWidth,
        MulDiv(client.right, 96, static_cast<int>(dpi ? dpi : 96)));
    int extra = contentWidth - ui_layout::settingsWidth;
    auto place = [window](const Placement& item) {
        SetWindowPos(GetDlgItem(window, item.id), nullptr,
            panelScale(window, item.x), panelScale(window, item.y) - g_settingsScrollPos,
            panelScale(window, item.w), panelScale(window, item.h), SWP_NOZORDER | SWP_NOACTIVATE);
    };
    for (const Placement& item : ui_layout::settings(extra)) place(item);
}

static void resizeSettingsWindow(HWND window, UINT dpi, HMONITOR monitor = nullptr) {
    if (!dpi) dpi = 96;
    if (!monitor) monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    RECT work{};
    if (GetMonitorInfoW(monitor, &info)) work = info.rcWork;
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);

    int contentWidth = MulDiv(ui_layout::settingsWidth, static_cast<int>(dpi), 96);
    int contentHeight = MulDiv(ui_layout::settingsHeight, static_cast<int>(dpi), 96);
    RECT full{0, 0, contentWidth, contentHeight};
    DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    AdjustWindowRectExForDpi(&full, style, FALSE, exStyle, dpi);
    int frameHeight = full.bottom - full.top - contentHeight;
    int available = work.bottom - work.top - frameHeight - MulDiv(16, static_cast<int>(dpi), 96);
    int clientHeight = min(contentHeight,
        max(MulDiv(settingsMinimumHeight, static_cast<int>(dpi), 96), available));

    RECT client{0, 0, contentWidth, clientHeight};
    AdjustWindowRectExForDpi(&client, style, FALSE, exStyle, dpi);
    int width = client.right - client.left;
    int height = client.bottom - client.top;
    SetWindowPos(window, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void showSettingsWindow() {
    if (!g_settingsWindow) {
        g_settingsWindow = CreateWindowExW(WS_EX_CONTROLPARENT, L"TypeThruSettings", L"TypeThru Settings",
                                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX
                                               | WS_MAXIMIZEBOX | WS_THICKFRAME | WS_VSCROLL | WS_CLIPCHILDREN,
                                           CW_USEDEFAULT, CW_USEDEFAULT, 0, 0, g_window, nullptr, g_instance, nullptr);
        if (g_settingsWindow && g_tray.hIcon) {
            SendMessageW(g_settingsWindow, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_tray.hIcon));
            SendMessageW(g_settingsWindow, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_tray.hIcon));
        }
    }
    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR monitor = g_settingsPositioned
        ? MonitorFromWindow(g_settingsWindow, MONITOR_DEFAULTTONEAREST)
        : MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    if (!g_settingsPositioned) {
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info)) {
            SetWindowPos(g_settingsWindow, nullptr, info.rcWork.left + 8, info.rcWork.top + 8,
                         0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        g_settingsDpi = GetDpiForWindow(g_settingsWindow);
        resizeSettingsWindow(g_settingsWindow, g_settingsDpi, monitor);
        RECT bounds{};
        GetWindowRect(g_settingsWindow, &bounds);
        if (GetMonitorInfoW(monitor, &info)) {
            int width = bounds.right - bounds.left;
            int height = bounds.bottom - bounds.top;
            int x = info.rcWork.left + max(8, (info.rcWork.right - info.rcWork.left - width) / 2);
            int y = info.rcWork.top + max(8, (info.rcWork.bottom - info.rcWork.top - height) / 2);
            SetWindowPos(g_settingsWindow, nullptr, x, y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        g_settingsPositioned = true;
    } else {
        g_settingsDpi = GetDpiForWindow(g_settingsWindow);
    }
    refreshUiPalette();
    syncSettingsWindow();
    applyWindowAppearance(g_settingsWindow, false);
    ShowWindow(g_settingsWindow, SW_SHOWNORMAL);
    SetForegroundWindow(g_settingsWindow);
}

void applyShortcut(const Shortcut& shortcut) {
    Shortcut previous = g_settings.shortcut;
    if (registerHotKey(shortcut)) {
        g_settings.save();
        SetDlgItemTextW(g_settingsWindow, IDS_NOTE, shortcut.caution().c_str());
    } else {
        registerHotKey(previous);
        SetDlgItemTextW(g_settingsWindow, IDS_NOTE,
                        (L"Windows did not accept " + shortcut.title() + L". " + g_hotkeyError).c_str());
    }
    syncSettingsWindow();
}

void uninstallEverything() {
    if (g_busy || g_installing) {
        MessageBeep(MB_ICONEXCLAMATION);
        return;
    }
    if (MessageBoxW(g_settingsWindow,
                    L"Remove TypeThru's settings and its start-at-login entry, then quit?\n"
                    L"(Your program files are not deleted; delete the TypeThru folder or use "
                    L"Settings > Apps to remove the program itself.)",
                    L"Uninstall TypeThru?", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
        return;
    }
    g_settings.setRunAtLogin(false);
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\TypeThru");
    MessageBoxW(g_settingsWindow, L"TypeThru's settings are removed. TypeThru is quitting.",
                L"TypeThru", MB_OK | MB_ICONINFORMATION);
    PostQuitMessage(0);
}

static void drawSettingsChrome(HWND window, HDC dc) {
    RECT client{};
    GetClientRect(window, &client);
    UINT dpi = GetDpiForWindow(window);
    ui::drawSurface(dc, client, dpi);
    int contentWidth = max(ui_layout::settingsWidth,
        MulDiv(client.right, 96, static_cast<int>(dpi ? dpi : 96)));
    int extra = contentWidth - ui_layout::settingsWidth;
    for (const ui_layout::Surface& surface : ui_layout::settingsSurfaces) {
        RECT card{panelScale(window, surface.x), panelScale(window, surface.y) - g_settingsScrollPos,
                  panelScale(window, surface.x + surface.w + extra),
                  panelScale(window, surface.y + surface.h) - g_settingsScrollPos};
        ui::drawSurface(dc, card, dpi, true);
    }
    HPEN pen = CreatePen(PS_SOLID, 1, g_palette.border);
    HGDIOBJ previous = SelectObject(dc, pen);
    int inset = panelScale(window, 20);
    int lineY = panelScale(window, ui_layout::settingsDividerY) - g_settingsScrollPos;
    MoveToEx(dc, inset, lineY, nullptr);
    LineTo(dc, client.right - inset, lineY);
    SelectObject(dc, previous);
    DeleteObject(pen);
}

static void scrollSettingsTo(HWND window, int position) {
    updateSettingsScrollBar(window);
    g_settingsScrollPos = min(max(0, position), settingsScrollLimit(window));
    updateSettingsScrollBar(window);
    layoutSettingsWindow(window);
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

static void ensureSettingsControlVisible(HWND window, HWND control) {
    if (!control || !IsWindowVisible(control)) return;
    RECT bounds{};
    GetWindowRect(control, &bounds);
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&bounds), 2);
    RECT client{};
    GetClientRect(window, &client);
    int margin = panelScale(window, 8);
    int top = margin;
    int bottom = max(top, client.bottom - margin);
    int position = g_settingsScrollPos;
    if (bounds.top < top) position += bounds.top - top;
    else if (bounds.bottom > bottom) position += bounds.bottom - bottom;
    if (position != g_settingsScrollPos) scrollSettingsTo(window, position);
}

static LRESULT CALLBACK settingsControlProc(HWND control, UINT message, WPARAM wParam,
                                            LPARAM lParam, UINT_PTR subclassId, DWORD_PTR refData) {
    (void)refData;
    HWND window = GetParent(control);
    if (message == WM_SETFOCUS) {
        LRESULT result = DefSubclassProc(control, message, wParam, lParam);
        ensureSettingsControlVisible(window, control);
        return result;
    }
    if (message == WM_MOUSEWHEEL) {
        wchar_t klass[32] = L"";
        GetClassNameW(control, klass, 32);
        bool comboOpen = wcscmp(klass, L"ComboBox") == 0
            && SendMessageW(control, CB_GETDROPPEDSTATE, 0, 0) != FALSE;
        if (!comboOpen) {
            SendMessageW(window, message, wParam, lParam);
            return 0;
        }
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(control, settingsControlProc, subclassId);
    return DefSubclassProc(control, message, wParam, lParam);
}

static bool settingsControlUsesSurface(int id) {
    switch (id) {
        case IDS_SHORTCUT_LABEL:
        case IDS_SHORTCUT_VALUE:
        case IDS_NOTE:
        case IDS_RUNLOGIN:
        case IDS_CHECKUPDATES:
        case IDS_METHOD_LABEL:
        case IDS_METHOD:
        case IDS_METHOD_HINT:
        case IDS_REPLACE:
        case IDS_SHIFTENTER:
        case IDS_SHIFTENTER_HINT:
        case IDS_FLOATBTN:
        case IDS_VERSION_LABEL:
        case IDS_UPDATE_STATUS:
            return true;
        default:
            return false;
    }
}

LRESULT CALLBACK settingsProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            auto add = [window](const wchar_t* klass, const wchar_t* text, DWORD style, int id) {
                HWND control = CreateWindowExW(0, klass, text, WS_CHILD | WS_VISIBLE | style,
                    0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
                if (control) {
                    SetWindowSubclass(control, settingsControlProc, 1, 0);
                    if (id == IDS_RUNLOGIN || id == IDS_CHECKUPDATES || id == IDS_REPLACE
                        || id == IDS_SHIFTENTER || id == IDS_FLOATBTN)
                        ui::installControl(control, ui::ControlKind::Toggle, true);
                    else if (id == IDS_METHOD)
                        ui::installControl(control, ui::ControlKind::Combo, true);
                    else if (id == IDS_CHANGE || id == IDS_RESET || id == IDS_CHECKNOW
                        || id == IDS_INSTALL || id == IDS_DIAGNOSTICS || id == IDS_UNINSTALL
                        || id == IDS_DONE)
                        ui::installControl(control, ui::ControlKind::Button,
                            ui_layout::buttonUsesSurface(id));
                }
                return control;
            };
            add(L"STATIC", ui_layout::label(IDS_SETTINGS_BRAND), SS_OWNERDRAW, IDS_SETTINGS_BRAND);
            add(L"STATIC", ui_layout::label(IDS_SETTINGS_TITLE), SS_LEFT | SS_NOPREFIX, IDS_SETTINGS_TITLE);
            add(L"STATIC", ui_layout::label(IDS_SETTINGS_SUBTITLE), SS_LEFT | SS_NOPREFIX, IDS_SETTINGS_SUBTITLE);
            add(L"STATIC", ui_layout::label(IDS_GENERAL_HEADING), SS_LEFT | SS_NOPREFIX, IDS_GENERAL_HEADING);
            add(L"STATIC", ui_layout::label(IDS_SHORTCUT_LABEL), SS_LEFT | SS_NOPREFIX, IDS_SHORTCUT_LABEL);
            add(L"STATIC", ui_layout::label(IDS_SHORTCUT_VALUE), SS_OWNERDRAW, IDS_SHORTCUT_VALUE);
            add(L"STATIC", ui_layout::label(IDS_NOTE), SS_LEFT | SS_NOPREFIX, IDS_NOTE);
            add(L"BUTTON", ui_layout::label(IDS_CHANGE), BS_OWNERDRAW | WS_TABSTOP, IDS_CHANGE);
            add(L"BUTTON", ui_layout::label(IDS_RESET), BS_OWNERDRAW | WS_TABSTOP, IDS_RESET);
            add(L"BUTTON", ui_layout::label(IDS_RUNLOGIN), BS_AUTOCHECKBOX | WS_TABSTOP, IDS_RUNLOGIN);
            add(L"BUTTON", ui_layout::label(IDS_CHECKUPDATES), BS_AUTOCHECKBOX | WS_TABSTOP, IDS_CHECKUPDATES);
            add(L"BUTTON", ui_layout::label(IDS_FLOATBTN), BS_AUTOCHECKBOX | WS_TABSTOP, IDS_FLOATBTN);
            add(L"STATIC", ui_layout::label(IDS_TYPING_HEADING), SS_LEFT | SS_NOPREFIX, IDS_TYPING_HEADING);
            add(L"STATIC", ui_layout::label(IDS_METHOD_LABEL), SS_LEFT | SS_NOPREFIX, IDS_METHOD_LABEL);
            HWND method = add(L"COMBOBOX", ui_layout::label(IDS_METHOD), CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED
                | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, IDS_METHOD);
            add(L"STATIC", ui_layout::label(IDS_METHOD_HINT), SS_LEFT | SS_NOPREFIX, IDS_METHOD_HINT);
            add(L"BUTTON", ui_layout::label(IDS_REPLACE), BS_AUTOCHECKBOX | WS_TABSTOP, IDS_REPLACE);
            add(L"BUTTON", ui_layout::label(IDS_SHIFTENTER), BS_AUTOCHECKBOX | WS_TABSTOP, IDS_SHIFTENTER);
            add(L"STATIC", ui_layout::label(IDS_SHIFTENTER_HINT), SS_LEFT | SS_NOPREFIX, IDS_SHIFTENTER_HINT);
            add(L"STATIC", ui_layout::label(IDS_UPDATES_HEADING), SS_LEFT | SS_NOPREFIX, IDS_UPDATES_HEADING);
            add(L"STATIC", ui_layout::label(IDS_VERSION_LABEL), SS_LEFT | SS_NOPREFIX, IDS_VERSION_LABEL);
            add(L"STATIC", ui_layout::label(IDS_UPDATE_STATUS), SS_LEFT | SS_NOPREFIX, IDS_UPDATE_STATUS);
            add(L"BUTTON", ui_layout::label(IDS_CHECKNOW), BS_OWNERDRAW | WS_TABSTOP, IDS_CHECKNOW);
            add(L"BUTTON", ui_layout::label(IDS_INSTALL), BS_OWNERDRAW | WS_TABSTOP, IDS_INSTALL);
            add(L"BUTTON", ui_layout::label(IDS_DIAGNOSTICS), BS_OWNERDRAW | WS_TABSTOP, IDS_DIAGNOSTICS);
            add(L"BUTTON", ui_layout::label(IDS_UNINSTALL), BS_OWNERDRAW | WS_TABSTOP, IDS_UNINSTALL);
            add(L"BUTTON", ui_layout::label(IDS_DONE), BS_DEFPUSHBUTTON | BS_OWNERDRAW | WS_TABSTOP, IDS_DONE);
            for (int i = 0; i < methodCount; i++) {
                ComboBox_AddString(method, utf8(methodShortTitle(static_cast<Method>(i))).c_str());
            }
            g_settingsDpi = GetDpiForWindow(window);
            resizeSettingsWindow(window, g_settingsDpi);
            applyFontsToWindow(window);
            setComboItemHeights(method);
            layoutSettingsWindow(window);
            applyWindowAppearance(window, false);
            return 0;
        }
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) layoutSettingsWindow(window);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            UINT dpi = GetDpiForWindow(window);
            if (!dpi) dpi = 96;
            RECT minimum{0, 0, MulDiv(ui_layout::settingsWidth, static_cast<int>(dpi), 96),
                         MulDiv(settingsMinimumHeight, static_cast<int>(dpi), 96)};
            DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
            DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
            AdjustWindowRectExForDpi(&minimum, style, FALSE, exStyle, dpi);
            limits->ptMinTrackSize.x = minimum.right - minimum.left;
            limits->ptMinTrackSize.y = minimum.bottom - minimum.top;
            return 0;
        }
        case WM_VSCROLL: {
            SCROLLINFO info{};
            info.cbSize = sizeof(info);
            info.fMask = SIF_ALL;
            GetScrollInfo(window, SB_VERT, &info);
            int position = info.nPos;
            switch (LOWORD(wParam)) {
                case SB_LINEUP: position -= panelScale(window, 48); break;
                case SB_LINEDOWN: position += panelScale(window, 48); break;
                case SB_PAGEUP: position -= max(1, static_cast<int>(info.nPage) - panelScale(window, 48)); break;
                case SB_PAGEDOWN: position += max(1, static_cast<int>(info.nPage) - panelScale(window, 48)); break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: position = info.nTrackPos; break;
                default: return 0;
            }
            scrollSettingsTo(window, position);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            g_settingsWheelDelta += GET_WHEEL_DELTA_WPARAM(wParam);
            int notches = g_settingsWheelDelta / WHEEL_DELTA;
            g_settingsWheelDelta %= WHEEL_DELTA;
            if (!notches) return 0;
            UINT lines = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            RECT client{};
            GetClientRect(window, &client);
            int distance = lines == WHEEL_PAGESCROLL
                ? max(1, client.bottom - panelScale(window, 48))
                : panelScale(window, 32 * static_cast<int>(lines));
            scrollSettingsTo(window, g_settingsScrollPos - notches * distance);
            return 0;
        }
        case WM_GETDLGCODE:
            if (g_recording) return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS;
            break;
        case WM_DRAWITEM: {
            auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (item->CtlID == IDS_SETTINGS_BRAND) drawBrandItem(item);
            else if (item->CtlType == ODT_COMBOBOX) drawComboItem(item);
            else if (item->CtlID == IDS_SHORTCUT_VALUE) drawShortcutValueItem(item);
            else if (item->CtlType == ODT_BUTTON) drawButtonItem(item, false);
            return TRUE;
        }
        case WM_MEASUREITEM: {
            auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (item->CtlType != ODT_COMBOBOX) break;
            UINT dpi = GetDpiForWindow(window);
            item->itemHeight = static_cast<UINT>(MulDiv(ui_layout::comboItemHeight,
                static_cast<int>(dpi ? dpi : 96), 96));
            return TRUE;
        }
        case WM_KEYDOWN: {
            if (!g_recording) break;
            UINT vk = static_cast<UINT>(wParam);
            if (vk == VK_ESCAPE) {
                g_recording = false;
                registerHotKey(g_settings.shortcut);
                SetDlgItemTextW(window, IDS_NOTE, L"");
                syncSettingsWindow();
                return 0;
            }
            Shortcut shortcut;
            shortcut.vk = vk;
            if (GetKeyState(VK_CONTROL) & 0x8000) shortcut.modifiers |= MOD_CONTROL;
            if (GetKeyState(VK_SHIFT) & 0x8000) shortcut.modifiers |= MOD_SHIFT;
            if (GetKeyState(VK_MENU) & 0x8000) shortcut.modifiers |= MOD_ALT;
            if (GetKeyState(VK_LWIN) & 0x8000 || GetKeyState(VK_RWIN) & 0x8000) shortcut.modifiers |= MOD_WIN;
            if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) return 0;
            if (!shortcut.problem().empty()) {
                SetDlgItemTextW(window, IDS_NOTE, shortcut.problem().c_str());
                return 0;
            }
            g_recording = false;
            applyShortcut(shortcut);
            return 0;
        }
        case WM_SYSKEYDOWN:
            if (g_recording) {
                SendMessageW(window, WM_KEYDOWN, wParam, lParam);
                return 0;
            }
            break;
        case WM_COMMAND: {
            WORD id = LOWORD(wParam);
            WORD code = HIWORD(wParam);
            if (id == IDOK || id == IDCANCEL) {
                if (!g_recording) ShowWindow(window, SW_HIDE);
            } else if (id == IDS_METHOD && code == CBN_SELENDOK) {
                int index = ComboBox_GetCurSel(GetDlgItem(window, IDS_METHOD));
                if (index >= 0 && index < methodCount) {
                    g_settings.method = static_cast<Method>(index);
                    g_settings.save();
                }
            } else if (code == BN_CLICKED && id == IDS_CHANGE) {
                UnregisterHotKey(g_window, HOTKEY_ID);
                g_hotkeyRegistered = false;
                g_recording = true;
                SetDlgItemTextW(window, IDS_NOTE, L"Press the new shortcut. Esc cancels.");
                SetFocus(window);
            } else if (code == BN_CLICKED && id == IDS_RESET) {
                g_recording = false;
                applyShortcut(Shortcut::standard());
            } else if (code == BN_CLICKED && id == IDS_RUNLOGIN) {
                g_settings.setRunAtLogin(IsDlgButtonChecked(window, IDS_RUNLOGIN) == BST_CHECKED);
                syncSettingsWindow();
            } else if (code == BN_CLICKED && id == IDS_CHECKUPDATES) {
                g_settings.checkUpdates = IsDlgButtonChecked(window, IDS_CHECKUPDATES) == BST_CHECKED;
                g_settings.save();
                if (g_settings.checkUpdates) runUpdateCheck(true);
                syncSettingsWindow();
            } else if (code == BN_CLICKED && id == IDS_REPLACE) {
                g_settings.replaceSymbols = IsDlgButtonChecked(window, IDS_REPLACE) == BST_CHECKED;
                g_settings.save();
            } else if (code == BN_CLICKED && id == IDS_SHIFTENTER) {
                g_settings.shiftReturn = IsDlgButtonChecked(window, IDS_SHIFTENTER) == BST_CHECKED;
                g_settings.save();
            } else if (code == BN_CLICKED && id == IDS_FLOATBTN) {
                g_settings.floatButton = IsDlgButtonChecked(window, IDS_FLOATBTN) == BST_CHECKED;
                g_settings.save();
                applyFloatButton();
            } else if (code == BN_CLICKED && id == IDS_CHECKNOW) {
                runUpdateCheck(true);
                syncSettingsWindow();
            } else if (code == BN_CLICKED && id == IDS_INSTALL) {
                installUpdate();
                syncSettingsWindow();
                syncPanel();
            } else if (code == BN_CLICKED && id == IDS_DIAGNOSTICS) {
                showDiagnostics();
            } else if (code == BN_CLICKED && id == IDS_UNINSTALL) {
                uninstallEverything();
            } else if (code == BN_CLICKED && id == IDS_DONE) {
                ShowWindow(window, SW_HIDE);
            }
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            HWND control = reinterpret_cast<HWND>(lParam);
            bool surface = message == WM_CTLCOLORLISTBOX || settingsControlUsesSurface(GetDlgCtrlID(control));
            SetBkMode(dc, TRANSPARENT);
            SetBkColor(dc, surface ? g_palette.surface : g_palette.background);
            int id = control ? GetDlgCtrlID(control) : 0;
            COLORREF text = id == IDS_SETTINGS_SUBTITLE || id == IDS_METHOD_HINT
                || id == IDS_SHIFTENTER_HINT || id == IDS_UPDATE_STATUS ? g_palette.muted : g_palette.text;
            if (id == IDS_NOTE) {
                wchar_t label[256] = L"";
                GetWindowTextW(control, label, static_cast<int>(sizeof(label) / sizeof(label[0])));
                text = wcscmp(label, shortcutDefaultNote) == 0 ? g_palette.muted : g_palette.warning;
            }
            SetTextColor(dc, text);
            HBRUSH brush = surface ? g_surfaceBrush : g_panelBrush;
            return reinterpret_cast<LRESULT>(brush ? brush : GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC target = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            HDC dc = target;
            HPAINTBUFFER buffer = beginBufferedPaint(target, client, dc);
            drawSettingsChrome(window, dc);
            if (buffer) EndBufferedPaint(buffer, TRUE);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DPICHANGED: {
            UINT dpi = HIWORD(wParam);
            int logicalScroll = MulDiv(g_settingsScrollPos, 96,
                                       static_cast<int>(g_settingsDpi ? g_settingsDpi : 96));
            RECT* suggested = reinterpret_cast<RECT*>(lParam);
            HMONITOR monitor = MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST);
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            resizeSettingsWindow(window, dpi, monitor);
            g_settingsDpi = dpi;
            g_settingsScrollPos = MulDiv(logicalScroll, static_cast<int>(dpi), 96);
            applyFontsToWindow(window);
            setComboItemHeights(GetDlgItem(window, IDS_METHOD));
            layoutSettingsWindow(window);
            applyWindowAppearance(window, false);
            RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
            return 0;
        }
        case WM_SETTINGCHANGE:
        case WM_SYSCOLORCHANGE:
        case WM_THEMECHANGED:
            refreshUiPalette();
            applyWindowAppearance(window, false);
            RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
            return 0;
        case WM_CLOSE:
            ShowWindow(window, SW_HIDE);
            return 0;
        case WM_SHOWWINDOW:
            if (!wParam && g_recording) {
                g_recording = false;
                registerHotKey(g_settings.shortcut);
                SetDlgItemTextW(window, IDS_NOTE, L"");
                syncSettingsWindow();
            }
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// ---------- diagnostics ----------

void showDiagnostics() {
    wchar_t lastHotKey[64] = L"Never received in this session";
    if (g_lastHotKeyTick != 0) {
        SYSTEMTIME time;
        GetLocalTime(&time);
        swprintf_s(lastHotKey, L"%02d:%02d:%02d", time.wHour, time.wMinute, time.wSecond);
    }
    HWND front = GetForegroundWindow();
    std::wstring elevatedNote;
    if (front && windowIsElevated(front)) {
        elevatedNote = L"\n\nThe current front window is elevated (run as administrator). "
                       L"TypeThru cannot type into elevated windows unless TypeThru itself is elevated.";
    }
    std::wstring text = std::wstring(L"Version: ") + TYPETHRU_VERSION_STRING
        + L"\nShortcut: " + g_settings.shortcut.title()
        + (g_hotkeyRegistered ? L", registered" : L", NOT registered")
        + L"\nLast shortcut received: " + lastHotKey
        + L"\nSelected method: " + utf8(methodShortTitle(g_settings.method))
        + L"\n\n" + g_lastAttempt
        + L"\n\nSent events do not confirm that the remote session accepted them. "
          L"Clipboard contents are not recorded." + elevatedNote;
    MessageBoxW(g_panel ? g_panel : g_window, text.c_str(), L"TypeThru Diagnostics",
                MB_OK | MB_ICONINFORMATION);
}

// ---------- main window ----------

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_HOTKEY: {
            if (g_recording) return 0;
            g_lastHotKeyTick = GetTickCount();
            // Fire on the hotkey path with the target that is in front right now.
            trigger(false, nullptr);
            return 0;
        }
        case WM_APP_CLOSEPANEL:
            closePanel();
            return 0;
        case WM_APP_TRAY: {
            if (lParam == WM_LBUTTONUP || lParam == WM_RBUTTONUP || lParam == WM_LBUTTONDBLCLK) {
                bool justClosed = GetTickCount() - g_panelClosedAt < 300;
                if (IsWindowVisible(g_panel) || justClosed) {
                    closePanel();
                } else {
                    showPanel();
                }
            }
            return 0;
        }
        case WM_APP_TYPEDONE: {
            TypedDone* done = reinterpret_cast<TypedDone*>(wParam);
            setBusy(false);
            if (done->result.done) {
                g_lastAttempt += L"\nSent events for " + std::to_wstring(done->result.total)
                               + L" characters. Remote acceptance is unverified.";
            } else {
                g_lastAttempt += L"\nTarget: ";
                g_lastAttempt += done->targetName;
                g_lastAttempt += L"\nStopped after sending " + std::to_wstring(done->result.typed)
                               + L" of " + std::to_wstring(done->result.total) + L" characters because "
                               + done->result.reason + L".";
                MessageBeep(MB_ICONEXCLAMATION);
                alert(L"Typing stopped",
                      L"Stopped after sending " + std::to_wstring(done->result.typed) + L" of "
                      + std::to_wstring(done->result.total) + L" characters because " + done->result.reason + L".");
            }
            delete done;
            syncPanel();
            return 0;
        }
        case WM_APP_UPDATECHECKED: {
            g_checkingUpdates = false;
            g_updateOffer.reset();
            Release* found = reinterpret_cast<Release*>(lParam);
            if (wParam == static_cast<WPARAM>(UpdateOutcome::Available)) {
                g_updateOffer = *found;
                g_updateStatus = L"Version " + found->version + L" is available.";
                updater::rememberOffer(*found);
            } else if (wParam == static_cast<WPARAM>(UpdateOutcome::UpToDate)) {
                g_updateStatus = L"TypeThru is up to date.";
                updater::rememberOffer(std::nullopt);
            } else {
                g_updateStatus = L"Could not reach GitHub. Try again later.";
            }
            delete found;
            syncPanel();
            syncSettingsWindow();
            return 0;
        }
        case WM_APP_UPDATEINSTALLED: {
            std::wstring* error = reinterpret_cast<std::wstring*>(lParam);
            g_installing = false;
            if (error->empty()) {
                delete error;
                PostQuitMessage(0);   // the new copy takes over
            } else {
                alert(L"Could not install the update",
                      *error + L"\n\nYou can download it from the release page instead.");
                g_updateStatus = L"The update was not installed.";
                delete error;
            }
            syncPanel();
            syncSettingsWindow();
            return 0;
        }
        case WM_TIMER: {
            if (wParam == TIMER_FIRST_CHECK) {
                KillTimer(window, TIMER_FIRST_CHECK);
                runUpdateCheck(false);
                SetTimer(window, TIMER_HOURLY, 3600 * 1000, nullptr);
            } else if (wParam == TIMER_HOURLY) {
                runUpdateCheck(false);
            }
            return 0;
        }
        case WM_DESTROY:
            if (g_hotkeyRegistered) UnregisterHotKey(window, HOTKEY_ID);
            Shell_NotifyIconW(NIM_DELETE, &g_tray);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}   // namespace

bool appBusy() { return g_busy || g_installing; }

int runTrayApp(HINSTANCE instance) {
    g_instance = instance;
    g_settings.load();
    g_uiReady = ui::initialize();
    g_bufferedPaintReady = SUCCEEDED(BufferedPaintInit());
    DWORD attemptSize = sizeof(g_lastUpdateCheck);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\TypeThru", L"LastUpdateAttempt", RRF_RT_REG_QWORD,
                 nullptr, &g_lastUpdateCheck, &attemptSize);

    refreshUiPalette();

    WNDCLASSW hidden{};
    hidden.lpfnWndProc = windowProc;
    hidden.hInstance = instance;
    hidden.lpszClassName = L"TypeThruWindow";
    RegisterClassW(&hidden);

    WNDCLASSW panelClass{};
    panelClass.lpfnWndProc = panelProc;
    panelClass.hInstance = instance;
    panelClass.lpszClassName = L"TypeThruPanel";
    RegisterClassW(&panelClass);

    WNDCLASSW settingsClass{};
    settingsClass.lpfnWndProc = settingsProc;
    settingsClass.hInstance = instance;
    settingsClass.lpszClassName = L"TypeThruSettings";
    RegisterClassW(&settingsClass);

    WNDCLASSW floatClass{};
    floatClass.lpfnWndProc = floatButtonProc;
    floatClass.hInstance = instance;
    floatClass.hbrBackground = nullptr;
    floatClass.lpszClassName = L"TypeThruFloat";
    RegisterClassW(&floatClass);

    g_window = CreateWindowExW(0, L"TypeThruWindow", L"TypeThru", WS_OVERLAPPED,
                               0, 0, 0, 0, nullptr, nullptr, instance, nullptr);

    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = g_window;
    g_tray.uID = 1;
    g_tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_tray.uCallbackMessage = WM_APP_TRAY;
    g_tray.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                                 0, 0, LR_DEFAULTSIZE));
    g_trayIconOwned = g_tray.hIcon != nullptr;
    if (!g_tray.hIcon) g_tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_tray.szTip, L"TypeThru");
    Shell_NotifyIconW(NIM_ADD, &g_tray);
    refreshTrayTip();

    if (!registerHotKey(g_settings.shortcut)) {
        alert(L"Could not use " + g_settings.shortcut.title(),
              L"Windows did not accept the shortcut. Another app may use it. "
              L"Choose a different one in TypeThru Settings.");
    }
    // A saved offer from a previous session still shows, without the network.
    if (auto saved = updater::savedOffer()) {
        if (updater::offerIsNewer(*saved, TYPETHRU_VERSION_STRING)) {
            g_updateOffer = saved;
            g_updateStatus = L"Version " + saved->version + L" is available.";
        }
    }
    applyFloatButton();
    SetTimer(g_window, TIMER_FIRST_CHECK, 10 * 1000, nullptr);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (g_recording && g_settingsWindow && IsWindowVisible(g_settingsWindow)
            && (message.hwnd == g_settingsWindow || IsChild(g_settingsWindow, message.hwnd))
            && (message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN)) {
            SendMessageW(g_settingsWindow, message.message, message.wParam, message.lParam);
            continue;
        }
        if (g_settingsWindow && IsWindowVisible(g_settingsWindow)
            && IsDialogMessageW(g_settingsWindow, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    closePanel();
    if (g_panel) { DestroyWindow(g_panel); g_panel = nullptr; }
    if (g_settingsWindow) { DestroyWindow(g_settingsWindow); g_settingsWindow = nullptr; }
    if (g_floatButton) { DestroyWindow(g_floatButton); g_floatButton = nullptr; }
    if (g_window) { DestroyWindow(g_window); g_window = nullptr; }
    if (g_trayIconOwned && g_tray.hIcon) DestroyIcon(g_tray.hIcon);
    if (g_panelBrush) { DeleteObject(g_panelBrush); g_panelBrush = nullptr; }
    if (g_surfaceBrush) { DeleteObject(g_surfaceBrush); g_surfaceBrush = nullptr; }
    for (auto& [dpi, fonts] : g_fontSets) {
        (void)dpi;
        if (fonts.body) DeleteObject(fonts.body);
        if (fonts.bold) DeleteObject(fonts.bold);
        if (fonts.caption) DeleteObject(fonts.caption);
        if (fonts.brandTitle) DeleteObject(fonts.brandTitle);
        if (fonts.title) DeleteObject(fonts.title);
        if (fonts.primary) DeleteObject(fonts.primary);
    }
    g_fontSets.clear();
    if (g_bufferedPaintReady) BufferedPaintUnInit();
    if (g_uiReady) ui::shutdown();
    return 0;
}

}   // namespace tt
