// TypeThru entry point: tray app by default, command-line modes for testing.
#include "app.h"
#include "shared.h"
#include "keymap.h"
#include "typer.h"
#include "stopwatcher.h"
#include "settings.h"
#include "hotkey.h"
#include "updates.h"
#include "version.h"
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <chrono>
#include <string>
#include <thread>

#pragma comment(lib, "shell32.lib")

using namespace tt;

namespace {

void enableConsole() {
    // Nothing to attach: the CLI modes write straight to the std handles, which
    // works for pipes (scripts), files (redirection), and a caller's console alike.
}

// GUI-subsystem apps may have no CRT stdout; write the std handle directly.
void printLine(const std::wstring& text) {
    std::wstring line = text + L"\n";
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    if (!WriteConsoleW(out, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr)) {
        int bytes = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                                        nullptr, 0, nullptr, nullptr);
        if (bytes > 0) {
            char* narrow = static_cast<char*>(_alloca(bytes));
            WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), narrow, bytes,
                                nullptr, nullptr);
            WriteFile(out, narrow, bytes, &written, nullptr);
        }
    }
}

// Shows which key presses a text would produce, without typing anything.
int printMap(int argc, wchar_t** argv) {
    std::wstring text;
    for (int i = 2; i < argc; i++) {
        if (!text.empty()) text += L" ";
        text += argv[i];
    }
    enableConsole();
    KeyMap map(GetKeyboardLayout(0));
    for (wchar_t ch : text) {
        wchar_t label[8] = {ch, 0};
        const KeyStroke* stroke = nullptr;
        const wchar_t* special = ch == L'\n' ? L"key Return"
                               : ch == L'\t' ? L"key Tab" : nullptr;
        if (special) { printLine(std::wstring(label) + L"\t" + special); continue; }
        stroke = map.find(ch);
        if (stroke) {
            printLine(std::wstring(label) + L"\tkey " + std::to_wstring(stroke->vk)
                     + (stroke->shift ? L" + Shift" : L""));
        } else {
            const auto& table = replacements();
            auto entry = table.find(ch);
            if (entry != table.end()) {
                printLine(std::wstring(label) + L"\treplaced with \"" + entry->second + L"\"");
            } else {
                printLine(std::wstring(label) + L"\tNO KEY");
            }
        }
    }
    return 0;
}

// Types text after a countdown. For testing by hand; posts real input.
int typeOnce(int argc, wchar_t** argv) {
    Method method = Method::Scancode;
    double interval = 0.24;
    int waitSeconds = 3;
    bool replace = true;
    std::wstring text;
    for (int i = 2; i < argc; i++) {
        std::wstring argument = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (argument == L"--method") {
            std::wstring name = next();
            if (name == L"vk") method = Method::VirtualKey;
            else if (name == L"unicode") method = Method::Unicode;
            else if (name == L"post") method = Method::PostWindow;
            else method = Method::Scancode;
        } else if (argument == L"--interval") {
            interval = _wtof(next().c_str()) / 1000.0;
        } else if (argument == L"--wait") {
            waitSeconds = _wtoi(next().c_str());
        } else if (argument == L"--no-replace") {
            replace = false;
        } else {
            if (!text.empty()) text += L" ";
            text += argument;
        }
    }
    text = normalizeLineBreaks(text);
    enableConsole();
    if (text.empty()) {
        printLine(L"usage: TypeThru --type [--method scancode|vk|unicode|post] "
                  L"[--interval ms] [--wait s] [--no-replace] <text>");
        return 2;
    }
    KeyMap map(GetKeyboardLayout(0));
    map = Typer::shiftReturn(std::move(map));
    if (replace) {
        int replaced = 0;
        text = replacingUntypable(text, replaced,
                                  [&map, method](wchar_t ch) { return Typer::typeable(ch, map, method); });
    }
    std::wstring missing;
    for (wchar_t ch : Typer::unsupported(text, map, method)) missing += ch;
    if (!missing.empty()) {
        printLine(L"unsupported characters: " + missing);
        return 1;
    }
    printLine(L"typing " + std::to_wstring(text.size()) + L" characters in "
              + std::to_wstring(waitSeconds) + L"s - click the target window");
    for (int i = waitSeconds; i > 0; i--) {
        printLine(std::to_wstring(i) + L"...");
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    HWND target = GetForegroundWindow();
    StopWatcher watcher(target);
    TypeResult result = Typer::type(text, map, interval, method, watcher);
    watcher.finish();
    if (result.done) printLine(L"done: sent events for " + std::to_wstring(result.total) + L" characters");
    else printLine(L"stopped after " + std::to_wstring(result.typed) + L" of "
                   + std::to_wstring(result.total) + L": " + result.reason);
    return result.done ? 0 : 1;
}

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 2) {
        std::wstring first = argv[1];
        if (first == L"--print-map") return printMap(argc, argv);
        if (first == L"--type") return typeOnce(argc, argv);
        if (first == L"--version") {
            enableConsole();
            printLine(TYPETHRU_VERSION_STRING);
            return 0;
        }
        if (first == L"--finalize-update" && argc >= 3) {
            return updater::finalizeUpdate(argv[2]);
        }
    }
    LocalFree(argv);

    // One TypeThru at a time; the hotkey and the tray icon cannot be shared.
    HANDLE single = CreateEventW(nullptr, TRUE, FALSE, L"Local\\TypeThru-SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"TypeThru is already running. Check the notification area.",
                    L"TypeThru", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    int status = runTrayApp(instance);
    CloseHandle(single);
    return status;
}
