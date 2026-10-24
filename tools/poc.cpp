// TypeThruPOC: the smallest honest test of whether a remote session accepts
// injected input. Run it, click the remote session's text field, wait for the
// countdown. It types the same text with the chosen method and reports.
//
//   TypeThruPOC [--method scancode|vk|unicode] [--interval ms] [--wait s] "text"
//
// scancode is the production method; vk and unicode exist to compare.
#include "../src/shared.h"
#include "../src/keymap.h"
#include "../src/typer.h"
#include "../src/stopwatcher.h"
#include <windows.h>
#include <stdio.h>
#include <chrono>
#include <string>
#include <thread>

using namespace tt;

static Method parseMethod(const wchar_t* name) {
    if (wcscmp(name, L"vk") == 0) return Method::VirtualKey;
    if (wcscmp(name, L"unicode") == 0) return Method::Unicode;
    return Method::Scancode;
}

int main(int argc, char** rawArgv) {
    (void)argc; (void)rawArgv;   // wide argv is parsed from the raw command line
    int count = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &count);
    Method method = Method::Scancode;
    double interval = 0.24;
    int waitSeconds = 3;
    std::wstring text;
    for (int i = 1; i < count; i++) {
        std::wstring argument = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < count ? argv[++i] : L""; };
        if (argument == L"--method") method = parseMethod(next().c_str());
        else if (argument == L"--interval") interval = _wtof(next().c_str()) / 1000.0;
        else if (argument == L"--wait") waitSeconds = _wtoi(next().c_str());
        else {
            if (!text.empty()) text += L" ";
            text += argument;
        }
    }
    if (text.empty()) {
        wprintf(L"usage: TypeThruPOC [--method scancode|vk|unicode] [--interval ms] [--wait s] \"text\"\n");
        return 2;
    }
    text = normalizeLineBreaks(text);
    const wchar_t* names[] = {L"scancode", L"vk", L"unicode"};
    wprintf(L"method=%s interval=%.0fms chars=%zu\n",
            method == Method::Unicode ? names[2] : (method == Method::VirtualKey ? names[1] : names[0]),
            interval * 1000.0, text.size());

    KeyMap map(GetKeyboardLayout(0));
    std::wstring missing;
    for (wchar_t ch : Typer::unsupported(text, map, method)) missing += ch;
    if (!missing.empty()) {
        wprintf(L"unsupported characters (not typed): %s\n", missing.c_str());
        return 1;
    }
    wprintf(L"click the target text field now\n");
    for (int i = waitSeconds; i > 0; i--) {
        wprintf(L"%d...\n", i);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    HWND target = GetForegroundWindow();
    StopWatcher watcher(target);
    TypeResult result = Typer::type(text, map, interval, method, watcher);
    watcher.finish();
    if (result.done) wprintf(L"done: sent %d characters\n", result.total);
    else wprintf(L"stopped after %d of %d: %s\n", result.typed, result.total, result.reason.c_str());
    return result.done ? 0 : 1;
}
