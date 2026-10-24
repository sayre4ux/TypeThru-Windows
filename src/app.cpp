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
#include "version.h"
#include <commctrl.h>
#include <windowsx.h>
#include <shellapi.h>
#include <atomic>
#include <optional>
#include <thread>
#include <cstdio>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace tt {

namespace {

// ASCII titles to wide text for the common controls.
std::wstring utf8(const char* text) {
    int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring wide(static_cast<size_t>(size - 1), 0);
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), size);
    return wide;
}

// ---------- identifiers ----------

enum {
    IDC_STATUS = 100, IDC_METHOD_LABEL, IDC_METHOD, IDC_SPEED_LABEL, IDC_SPEED,
    IDC_REPLACE, IDC_SHIFTENTER, IDC_FLOATBTN, IDC_TYPE, IDC_TEST_LABEL, IDC_TEST, IDC_RUNTEST,
    IDC_UPDATE_STATUS, IDC_INSTALL, IDC_SETTINGS_BTN, IDC_DIAGNOSTICS, IDC_QUIT,
    IDS_SHORTCUT_LABEL = 200, IDS_NOTE, IDS_CHANGE, IDS_RESET, IDS_RUNLOGIN,
    IDS_CHECKUPDATES, IDS_UNINSTALL, IDS_DONE,
};

constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT WM_APP_TYPEDONE = WM_APP + 2;   // wParam = new TypeResult*, lParam = target name wchar_t*
constexpr UINT WM_APP_UPDATECHECKED = WM_APP + 3;
constexpr UINT WM_APP_UPDATEINSTALLED = WM_APP + 4;   // lParam = new std::wstring error or nullptr
constexpr UINT WM_APP_CLOSEPANEL = WM_APP + 5;
constexpr UINT HOTKEY_ID = 1;
constexpr UINT TIMER_FIRST_CHECK = 1;
constexpr UINT TIMER_HOURLY = 2;

// ---------- state ----------

HINSTANCE g_instance = nullptr;
HWND g_window = nullptr;          // hidden window: tray, hotkey, timers
HWND g_panel = nullptr;
HWND g_settingsWindow = nullptr;
HWND g_floatButton = nullptr;     // always-on-top click trigger for full-screen VDI
NOTIFYICONDATAW g_tray = {};
HFONT g_font = nullptr;
HFONT g_fontBold = nullptr;
Settings g_settings;
std::atomic<bool> g_busy{false};
std::atomic<bool> g_installing{false};
std::wstring g_lastAttempt = L"No attempt in this session.";
std::wstring g_hotkeyError;
bool g_hotkeyRegistered = false;
bool g_recording = false;
DWORD g_lastHotKeyTick = 0;
std::optional<Release> g_updateOffer;
std::wstring g_updateStatus = L" ";
ULONGLONG g_lastUpdateCheck = 0;

// ---------- helpers ----------

void showPanel();
void closePanel();
void syncPanel();
void showSettingsWindow();
void showDiagnostics();

void alert(const std::wstring& title, const std::wstring& message) {
    MessageBoxW(g_panel ? g_panel : g_window, message.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

void refreshTrayTip() {
    wcscpy_s(g_tray.szTip, g_busy ? L"TypeThru - typing..." : L"TypeThru");
    Shell_NotifyIconW(NIM_MODIFY, &g_tray);
}

// Waits until Ctrl, Alt, Win, and Shift are all released (up to 3 seconds).
// The trigger VK is a copy: the UI thread may change the setting while we wait.
bool waitForModifierRelease(UINT triggerVk, bool includeTriggerKey) {
    for (int i = 0; i < 150; i++) {
        const int modifiers[5] = {VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN};
        bool allUp = true;
        for (int vk : modifiers) {
            if (GetAsyncKeyState(vk) & 0x8000) { allUp = false; break; }
        }
        if (allUp && includeTriggerKey && triggerVk && (GetAsyncKeyState(triggerVk) & 0x8000)) allUp = false;
        if (allUp) return true;
        Sleep(20);
    }
    return false;
}

std::wstring windowDescription(HWND window) {
    wchar_t title[128] = L"";
    GetWindowTextW(window, title, 128);
    if (title[0]) return title;
    wchar_t className[64] = L"";
    GetClassNameW(window, className, 64);
    return className[0] ? className : std::wstring(L"unknown");
}

static bool tokenIsElevated(HANDLE token) {
    TOKEN_ELEVATION_TYPE type = TokenElevationTypeDefault;
    DWORD size = 0;
    return GetTokenInformation(token, TokenElevationType, &type, sizeof(type), &size)
        && type == TokenElevationTypeFull;
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
    if (g_panel) {
        HWND button = GetDlgItem(g_panel, IDC_TYPE);
        EnableWindow(button, !busy);
    }
}

// The typing run: everything after the checks in trigger() happens off the UI thread.
void runTyping(std::wstring text, KeyMap map, double interval, Method method,
               bool fromMenu, int settleBeforeMs, UINT triggerVk) {
    std::thread([text = std::move(text), map = std::move(map), interval, method, fromMenu,
                 settleBeforeMs, triggerVk]() mutable {
        Sleep(settleBeforeMs);
        // DECISION: the shortcut path starts on release, so the remote client receives the
        // physical key-up sequence before the injected keys begin (same reason as macOS).
        bool released = waitForModifierRelease(triggerVk, !fromMenu);
        if (released) Sleep(fromMenu ? 150 : 750);
        TypedDone* done = new TypedDone();
        if (!released) {
            done->result.done = false;
            done->result.total = static_cast<int>(text.size());
            done->result.reason = L"shortcut or modifier keys were not released";
        } else {
            HWND target = GetForegroundWindow();
            DWORD pid = 0;
            GetWindowThreadProcessId(target, &pid);
            if (!target || pid == GetCurrentProcessId()) {
                done->result.done = false;
                done->result.total = static_cast<int>(text.size());
                done->result.reason = L"the target window was unavailable or changed before typing";
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

    KeyMap map(layoutForWindow(GetForegroundWindow()));
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
        alert(L"Nothing typed", L"Some characters have no key on the current keyboard layout. "
              L"Try Unicode text events, or check your keyboard layout.");
        return;
    }

    double interval = (test && !test->usesSelectedSpeed) ? speeds[0].interval : g_settings.interval();
    g_lastAttempt += L"\nSpeed: " + std::to_wstring(static_cast<int>(interval * 1000)) + L" ms per character";
    setBusy(true);
    // Panel runs give the user time to click the target field; tests give more, for the switch.
    int settle = test ? 3000 : (fromMenu ? 1000 : 0);
    runTyping(original, std::move(map), interval, method, fromMenu, settle, g_settings.shortcut.vk);
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
    if (g_installing) return;
    if (!manual) {
        if (!g_settings.checkUpdates) return;
        ULONGLONG now = 0;
        GetSystemTimeAsFileTime(reinterpret_cast<FILETIME*>(&now));
        if (g_lastUpdateCheck != 0 && now - g_lastUpdateCheck < 20ULL * 3600 * 10000000ULL) return;
    }
    g_updateStatus = L"Checking...";
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
    if (g_installing || !g_updateOffer) { MessageBeep(MB_ICONEXCLAMATION); return; }
    g_installing = true;
    g_updateStatus = L"Downloading...";
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
    return value * static_cast<int>(dpi) / 96;
}

void placeControl(HWND panel, int id, int x, int y, int w, int h) {
    HWND control = GetDlgItem(panel, id);
    SetWindowPos(control, nullptr, panelScale(panel, x), panelScale(panel, y),
                 panelScale(panel, w), panelScale(panel, h), SWP_NOZORDER);
}

void layoutPanel(HWND panel) {
    placeControl(panel, IDC_STATUS, 16, 14, 288, 20);
    placeControl(panel, IDC_METHOD_LABEL, 16, 44, 120, 20);
    placeControl(panel, IDC_METHOD, 150, 41, 154, 400);
    placeControl(panel, IDC_SPEED_LABEL, 16, 78, 120, 20);
    placeControl(panel, IDC_SPEED, 150, 75, 154, 400);
    placeControl(panel, IDC_REPLACE, 16, 112, 288, 22);
    placeControl(panel, IDC_SHIFTENTER, 16, 136, 288, 22);
    placeControl(panel, IDC_TYPE, 16, 166, 288, 34);
    placeControl(panel, IDC_TEST_LABEL, 16, 212, 120, 20);
    placeControl(panel, IDC_TEST, 150, 209, 154, 400);
    placeControl(panel, IDC_RUNTEST, 16, 240, 288, 28);
    placeControl(panel, IDC_UPDATE_STATUS, 16, 280, 288, 34);
    placeControl(panel, IDC_INSTALL, 16, 318, 288, 28);
    placeControl(panel, IDC_SETTINGS_BTN, 16, 356, 90, 28);
    placeControl(panel, IDC_DIAGNOSTICS, 112, 356, 98, 28);
    placeControl(panel, IDC_QUIT, 216, 356, 88, 28);
    placeControl(panel, IDC_FLOATBTN, 16, 392, 288, 22);
}

void syncPanel() {
    if (!g_panel) return;
    CheckDlgButton(g_panel, IDC_REPLACE, g_settings.replaceSymbols ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_panel, IDC_SHIFTENTER, g_settings.shiftReturn ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_panel, IDC_FLOATBTN, g_settings.floatButton ? BST_CHECKED : BST_UNCHECKED);
    SendMessageW(GetDlgItem(g_panel, IDC_METHOD), CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < methodCount; i++) {
       ComboBox_AddString(GetDlgItem(g_panel, IDC_METHOD), utf8(methodShortTitle(static_cast<Method>(i))).c_str());
        ComboBox_SetItemData(GetDlgItem(g_panel, IDC_METHOD), i, i);
    }
    ComboBox_SetCurSel(GetDlgItem(g_panel, IDC_METHOD), static_cast<int>(g_settings.method));
    SendMessageW(GetDlgItem(g_panel, IDC_SPEED), CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < speedCount; i++) {
        ComboBox_AddString(GetDlgItem(g_panel, IDC_SPEED), utf8(speeds[i].title).c_str());
    }
    ComboBox_SetCurSel(GetDlgItem(g_panel, IDC_SPEED), g_settings.speedIndex);
    SendMessageW(GetDlgItem(g_panel, IDC_TEST), CB_RESETCONTENT, 0, 0);
    for (const TypingTest& test : typingTests()) {
        ComboBox_AddString(GetDlgItem(g_panel, IDC_TEST), utf8(test.title).c_str());
    }
    ComboBox_SetCurSel(GetDlgItem(g_panel, IDC_TEST), 0);
    SetDlgItemTextW(g_panel, IDC_UPDATE_STATUS, g_updateStatus.c_str());
    ShowWindow(GetDlgItem(g_panel, IDC_INSTALL),
               g_updateOffer ? SW_SHOW : SW_HIDE);
    EnableWindow(GetDlgItem(g_panel, IDC_TYPE), !g_busy);
    // Status: hotkey is the one thing that can break silently.
    std::wstring status = g_busy ? L"Typing..." : L"Ready";
    if (!g_hotkeyRegistered) status = L"Shortcut not registered";
    SetDlgItemTextW(g_panel, IDC_STATUS, status.c_str());
    InvalidateRect(g_panel, nullptr, TRUE);
}

// The panel closes when the user clicks anywhere outside it, like the macOS panel.
// The panel is WS_EX_NOACTIVATE (the remote client must keep focus), so no focus or
// activation message ever reaches it; instead a WH_MOUSE_LL hook runs only while the
// panel is visible, sees nothing but click positions, and is removed on close.
HHOOK g_panelClickHook = nullptr;
DWORD g_panelClosedAt = 0;

static bool windowBelongsToPanel(HWND window) {
    // Walk the parent chain to the root, then any owner chain (combo dropdown lists
    // are top-level windows owned by controls inside the panel).
    for (HWND root = window; root; ) {
        if (root == g_panel) return true;
        HWND owner = GetWindow(root, GW_OWNER);
        root = owner ? GetAncestor(owner, GA_ROOT) : nullptr;
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
                                  L"TypeThruPanel", L"TypeThru", WS_POPUP | WS_BORDER | WS_CLIPCHILDREN,
                                  0, 0, 0, 0, g_window, nullptr, g_instance, nullptr);
    }
    syncPanel();
    RECT area = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    POINT cursor = {};
    GetCursorPos(&cursor);
    int width = panelScale(g_panel, 320), height = panelScale(g_panel, 424);
    int x = cursor.x - width - 12;
    int y = cursor.y - height - 12;
    if (y < area.top) y = area.top + 8;
    if (x + width > area.right) x = area.right - width - 8;
    if (x < area.left) x = area.left + 8;
    SetWindowPos(g_panel, HWND_TOPMOST, x, y, width, height, SWP_SHOWWINDOW);
    // The panel never takes focus, so the remote client stays in front and ready.
    InvalidateRect(g_panel, nullptr, TRUE);
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
constexpr int floatButtonSize = 34;

static void saveFloatPosition(int x, int y) {
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\TypeThru", L"FloatX", REG_DWORD,
                    &x, sizeof(x));
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\TypeThru", L"FloatY", REG_DWORD,
                    &y, sizeof(y));
}

static POINT savedFloatPosition(int size) {
    int x = 0, y = 0;
    DWORD value = 0, sizeOut = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\TypeThru", L"FloatX", RRF_RT_REG_DWORD,
                     nullptr, &value, &sizeOut) == ERROR_SUCCESS) {
        x = static_cast<int>(value);
    }
    sizeOut = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\TypeThru", L"FloatY", RRF_RT_REG_DWORD,
                     nullptr, &value, &sizeOut) == ERROR_SUCCESS) {
        y = static_cast<int>(value);
    }
    RECT area = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    if (x == 0 && y == 0) {   // first run: the low corner, away from most content
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
    static POINT pressAt = {};
    static POINT windowAt = {};
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint;
            HDC dc = BeginPaint(button, &paint);
            RECT client;
            GetClientRect(button, &client);
            FillRect(dc, &client, reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            HBRUSH background = CreateSolidBrush(RGB(0, 110, 220));
            HRGN shape = CreateRoundRectRgn(client.left + 1, client.top + 1,
                                            client.right - 1, client.bottom - 1, 10, 10);
            FillRgn(dc, shape, background);
            DeleteObject(shape);
            DeleteObject(background);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            HFONT font = CreateFontW((client.bottom - client.top) * 2 / 3, 0, 0, 0, FW_SEMIBOLD,
                                     0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            HFONT previous = static_cast<HFONT>(SelectObject(dc, font));
            SetWindowTextW(button, L"Type clipboard");
            DrawTextW(dc, L"T", 1, &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, previous);
            DeleteObject(font);
            EndPaint(button, &paint);
            return 0;
        }
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        case WM_LBUTTONDOWN:
            dragging = true;
            GetCursorPos(&pressAt);
            RECT current;
            GetWindowRect(button, &current);
            windowAt = POINT{current.left, current.top};
            SetCapture(button);
            return 0;
        case WM_MOUSEMOVE:
            if (dragging) {
                POINT at;
                GetCursorPos(&at);
                SetWindowPos(button, nullptr, windowAt.x + at.x - pressAt.x,
                             windowAt.y + at.y - pressAt.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            }
            return 0;
        case WM_LBUTTONUP: {
            bool moved = false;
            if (dragging) {
                dragging = false;
                ReleaseCapture();
                POINT now;
                GetCursorPos(&now);
                moved = abs(now.x - pressAt.x) + abs(now.y - pressAt.y) > 4;
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
        case WM_RBUTTONUP:
            g_settings.floatButton = false;
            g_settings.save();
            ShowWindow(button, SW_HIDE);
            syncPanel();
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
        }
        ShowWindow(g_floatButton, SW_SHOWNOACTIVATE);
    } else if (g_floatButton) {
        ShowWindow(g_floatButton, SW_HIDE);
    }
}

LRESULT CALLBACK panelProc(HWND panel, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_SIMPLE,
                            0, 0, 0, 0, panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS)), g_instance, nullptr);
            CreateWindowExW(0, L"STATIC", L"Method", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, panel,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_METHOD_LABEL)), g_instance, nullptr);
            HWND method = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                          0, 0, 0, 0, panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_METHOD)), g_instance, nullptr);
            SendMessageW(method, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
            CreateWindowExW(0, L"STATIC", L"Speed", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, panel,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SPEED_LABEL)), g_instance, nullptr);
            CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                            0, 0, 0, 0, panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SPEED)), g_instance, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Replace symbols that have no key", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                            0, 0, 0, 0, panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_REPLACE)), g_instance, nullptr);
            CreateWindowExW(0, L"BUTTON", L"New lines as Shift+Enter", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                            0, 0, 0, 0, panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SHIFTENTER)), g_instance, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Show the Type button (works in full-screen VDI)", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                            0, 0, 0, 0, panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_FLOATBTN)), g_instance, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Type Clipboard", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            0, 0, 0, 0, panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TYPE)), g_instance, nullptr);
            CreateWindowExW(0, L"STATIC", L"Test", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, panel,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TEST_LABEL)), g_instance, nullptr);
            CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                            0, 0, 0, 0, panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TEST)), g_instance, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Run Test", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            0, 0, 0, 0, panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RUNTEST)), g_instance, nullptr);
            CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                            0, 0, 0, 0, panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_UPDATE_STATUS)), g_instance, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Install Update", WS_CHILD | BS_PUSHBUTTON,
                            0, 0, 0, 0, panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_INSTALL)), g_instance, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Settings", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            0, 0, 0, 0, panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_BTN)), g_instance, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Diagnostics", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            0, 0, 0, 0, panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DIAGNOSTICS)), g_instance, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Quit", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            0, 0, 0, 0, panel, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_QUIT)), g_instance, nullptr);
            for (HWND child = GetWindow(panel, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
            }
            SendMessageW(GetDlgItem(panel, IDC_STATUS), WM_SETFONT, reinterpret_cast<WPARAM>(g_fontBold), TRUE);
            layoutPanel(panel);
            return 0;
        }
        case WM_COMMAND: {
            WORD id = LOWORD(wParam);
            WORD code = HIWORD(wParam);
            if (id == IDC_TYPE && code == BN_CLICKED) {
                closePanel();
                trigger(true, nullptr);
            } else if (id == IDC_RUNTEST && code == BN_CLICKED) {
                int index = ComboBox_GetCurSel(GetDlgItem(panel, IDC_TEST));
                const auto& tests = typingTests();
                if (index >= 0 && index < static_cast<int>(tests.size())) {
                    closePanel();
                    trigger(true, &tests[index]);
                }
            } else if (id == IDC_METHOD && code == CBN_SELENDOK) {
                g_settings.method = static_cast<Method>(ComboBox_GetCurSel(GetDlgItem(panel, IDC_METHOD)));
                g_settings.save();
                syncPanel();
            } else if (id == IDC_SPEED && code == CBN_SELENDOK) {
                g_settings.speedIndex = ComboBox_GetCurSel(GetDlgItem(panel, IDC_SPEED));
                g_settings.save();
            } else if (id == IDC_REPLACE && code == BN_CLICKED) {
                g_settings.replaceSymbols = IsDlgButtonChecked(panel, IDC_REPLACE) == BST_CHECKED;
                g_settings.save();
            } else if (id == IDC_SHIFTENTER && code == BN_CLICKED) {
                g_settings.shiftReturn = IsDlgButtonChecked(panel, IDC_SHIFTENTER) == BST_CHECKED;
                g_settings.save();
            } else if (id == IDC_FLOATBTN && code == BN_CLICKED) {
                g_settings.floatButton = IsDlgButtonChecked(panel, IDC_FLOATBTN) == BST_CHECKED;
                g_settings.save();
                applyFloatButton();
            } else if (id == IDC_SETTINGS_BTN && code == BN_CLICKED) {
                closePanel();
                showSettingsWindow();
            } else if (id == IDC_DIAGNOSTICS && code == BN_CLICKED) {
                closePanel();
                showDiagnostics();
            } else if (id == IDC_INSTALL && code == BN_CLICKED) {
                installUpdate();
                syncPanel();
            } else if (id == IDC_QUIT && code == BN_CLICKED) {
                PostQuitMessage(0);
            }
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            SetTextColor(dc, RGB(20, 20, 20));
            return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
        }
        case WM_DPICHANGED: {
            RECT* suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(panel, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER);
            layoutPanel(panel);
            return 0;
        }
        case WM_ERASEBKGND: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            RECT client;
            GetClientRect(panel, &client);
            FillRect(dc, &client, reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            return 1;
        }
    }
    return DefWindowProcW(panel, message, wParam, lParam);
}

// ---------- settings window ----------

void syncSettingsWindow() {
    if (!g_settingsWindow) return;
    std::wstring label = L"Shortcut: " + g_settings.shortcut.title();
    if (!g_hotkeyRegistered && !g_hotkeyError.empty()) label += L"  (not registered)";
    SetDlgItemTextW(g_settingsWindow, IDS_SHORTCUT_LABEL, label.c_str());
    CheckDlgButton(g_settingsWindow, IDS_RUNLOGIN, g_settings.runAtLogin() ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_settingsWindow, IDS_CHECKUPDATES, g_settings.checkUpdates ? BST_CHECKED : BST_UNCHECKED);
}

void showSettingsWindow() {
    if (!g_settingsWindow) {
        g_settingsWindow = CreateWindowExW(0, L"TypeThruSettings", L"TypeThru Settings",
                                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                           CW_USEDEFAULT, CW_USEDEFAULT, 0, 0, g_window, nullptr, g_instance, nullptr);
    }
    syncSettingsWindow();
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

LRESULT CALLBACK settingsProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDS_SHORTCUT_LABEL)), g_instance, nullptr);
            CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDS_NOTE)), g_instance, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Change...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDS_CHANGE)), g_instance, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Reset", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDS_RESET)), g_instance, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Open TypeThru when I sign in", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                            0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDS_RUNLOGIN)), g_instance, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Check for updates automatically", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                            0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDS_CHECKUPDATES)), g_instance, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Uninstall TypeThru...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDS_UNINSTALL)), g_instance, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Done", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDS_DONE)), g_instance, nullptr);
            for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
            }
            int dpi = GetDpiForWindow(window);
            auto scale = [dpi](int v) { return v * static_cast<int>(dpi) / 96; };
            SetWindowPos(window, nullptr, 0, 0, scale(316), scale(268),
                         SWP_NOMOVE | SWP_NOZORDER);
            #define PLACE(id, x, y, w, h) SetWindowPos(GetDlgItem(window, id), nullptr, \
                scale(x), scale(y), scale(w), scale(h), SWP_NOZORDER)
            PLACE(IDS_SHORTCUT_LABEL, 14, 12, 280, 20);
            PLACE(IDS_NOTE, 14, 32, 280, 34);
            PLACE(IDS_CHANGE, 14, 70, 100, 28);
            PLACE(IDS_RESET, 122, 70, 80, 28);
            PLACE(IDS_RUNLOGIN, 14, 112, 280, 22);
            PLACE(IDS_CHECKUPDATES, 14, 136, 280, 22);
            PLACE(IDS_UNINSTALL, 14, 170, 160, 28);
            PLACE(IDS_DONE, 200, 170, 100, 28);
            #undef PLACE
            return 0;
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
            // RegisterHotKey sees lone menu keys as system input; drop bare modifier choices.
            if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) return 0;
            if (!shortcut.problem().empty()) {
                SetDlgItemTextW(window, IDS_NOTE, shortcut.problem().c_str());
                return 0;
            }
            g_recording = false;
            applyShortcut(shortcut);
            return 0;
        }
        case WM_SYSKEYDOWN: {
            if (g_recording) {
                SendMessageW(window, WM_KEYDOWN, wParam, lParam);
                return 0;
            }
            break;
        }
        case WM_COMMAND: {
            WORD id = LOWORD(wParam);
            WORD code = HIWORD(wParam);
            if (code == BN_CLICKED && id == IDS_CHANGE) {
                // Recording: the hotkey is off meanwhile, so pressing it does not start typing.
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
            } else if (code == BN_CLICKED && id == IDS_UNINSTALL) {
                uninstallEverything();
            } else if (code == BN_CLICKED && id == IDS_DONE) {
                ShowWindow(window, SW_HIDE);
            }
            return 0;
        }
        case WM_CLOSE:
            ShowWindow(window, SW_HIDE);
            return 0;
        case WM_SHOWWINDOW:
            // Hiding the settings window cancels a recording; otherwise the hotkey
            // would stay unregistered with no way back except Change... again.
            if (!wParam && g_recording) {
                g_recording = false;
                registerHotKey(g_settings.shortcut);
                SetDlgItemTextW(window, IDS_NOTE, L"");
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
    std::wstring text = std::wstring(L"Shortcut: ") + g_settings.shortcut.title()
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

    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    g_font = CreateFontIndirectW(&metrics.lfMessageFont);
    LOGFONTW bold = metrics.lfMessageFont;
    bold.lfWeight = FW_SEMIBOLD;
    g_fontBold = CreateFontIndirectW(&bold);

    WNDCLASSW hidden{};
    hidden.lpfnWndProc = windowProc;
    hidden.hInstance = instance;
    hidden.lpszClassName = L"TypeThruWindow";
    RegisterClassW(&hidden);

    WNDCLASSW panelClass{};
    panelClass.lpfnWndProc = panelProc;
    panelClass.hInstance = instance;
    panelClass.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    panelClass.lpszClassName = L"TypeThruPanel";
    RegisterClassW(&panelClass);

    WNDCLASSW settingsClass{};
    settingsClass.lpfnWndProc = settingsProc;
    settingsClass.hInstance = instance;
    settingsClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
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
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

}   // namespace tt
