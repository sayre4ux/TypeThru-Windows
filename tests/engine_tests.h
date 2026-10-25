// Regression tests use fake transports only. No SendInput or PostMessage calls.
static void testClipboardBounds() {
    const wchar_t valid[] = L"private text";
    expect(clipboardTextFromBytes(valid, sizeof(valid)) == valid, "bounded clipboard text accepted");
    const wchar_t broken[] = {L'a', L'b'};
    expect(clipboardTextFromBytes(broken, sizeof(broken)).empty(), "unterminated clipboard rejected");
    expect(clipboardTextFromBytes(valid, 3).empty(), "odd UTF16 allocation rejected");
    expect(clipboardTextFromBytes(nullptr, 0).empty(), "null clipboard rejected");
    expect(clipboardTextFromBytes(valid, (16u << 20) + 2).empty(), "oversized clipboard rejected before read");
    expect(!Shortcut{0, MOD_CONTROL}.problem().empty(), "zero shortcut VK rejected");
    expect(!Shortcut{0x100, MOD_CONTROL}.problem().empty(), "out of range VK rejected");
    expect(!Shortcut{VK_LSHIFT, MOD_CONTROL}.problem().empty(), "modifier-only shortcut rejected");
    expect(!Shortcut{'A', 0x80000000u | MOD_CONTROL}.problem().empty(), "unknown modifier bits rejected");
}

static bool sameInputKey(const INPUT& a, const INPUT& b) {
    return a.ki.wVk == b.ki.wVk && a.ki.wScan == b.ki.wScan
        && (a.ki.dwFlags & ~KEYEVENTF_KEYUP) == (b.ki.dwFlags & ~KEYEVENTF_KEYUP);
}

static void testInputFailures() {
    KeyMap map(LoadKeyboardLayoutW(L"00000409", 0));
    auto shifted = Typer::stepsFor(L'A', map, Method::Scancode, 80, 160);
    auto pair = Typer::unicodePairSteps(0xD83D, 0xDE00, 80, 160);
    for (const auto& steps : {shifted, pair}) {
        for (int failureAt = 0; failureAt < static_cast<int>(steps.size()); failureAt++) {
            std::vector<INPUT> held;
            int calls = 0;
            bool orphanRelease = false;
            auto send = [&](const INPUT& input) {
                if (calls++ == failureAt) return false;
                if (input.ki.dwFlags & KEYEVENTF_KEYUP) {
                    auto found = std::find_if(held.begin(), held.end(), [&](const INPUT& down) {
                        return sameInputKey(input, down);
                    });
                    if (found == held.end()) orphanRelease = true;
                    else held.erase(found);
                } else held.push_back(input);
                return true;
            };
            auto error = Typer::executeSteps(steps, send, [] { return std::wstring(); }, [](int) {});
            expect(!error.empty(), "every failed injection is reported");
            expect(held.empty() && !orphanRelease, "failure releases only successfully pressed keys");
        }
        for (int cancelAt : {0, 10, 30, 100, 130}) {
            int elapsed = 0;
            std::vector<INPUT> held;
            auto send = [&](const INPUT& input) {
                if (input.ki.dwFlags & KEYEVENTF_KEYUP) {
                    auto found = std::find_if(held.begin(), held.end(), [&](const INPUT& down) {
                        return sameInputKey(input, down);
                    });
                    if (found != held.end()) held.erase(found);
                } else held.push_back(input);
                return true;
            };
            auto error = Typer::executeSteps(steps, send,
                [&] { return elapsed >= cancelAt ? std::wstring(L"stopped") : std::wstring(); },
                [&](int ms) { elapsed += ms; });
            expect(error == L"stopped" && held.empty(), "cancellation releases injected keys");
            expect(elapsed <= cancelAt + 10, "cancellation checked during hold and gap");
        }
    }
    int waited = 0, sent = 0;
    expect(Typer::executeSteps(shifted, [&](const INPUT&) { sent++; return true; },
        [] { return std::wstring(); }, [&](int ms) { waited += ms; }).empty(), "successful sequence completes");
    expect(sent == 4 && waited == 280, "success preserves shift margins and original timings");
    expect(pair.size() == 4 && pair[1].waitMs == 80 && pair[3].waitMs == 160,
           "Unicode surrogate sequence uses selected timing");
    expect(Typer::unicodePairSteps(L'A', 0xDE00, 80, 160).empty(), "invalid surrogate construction rejected");
    expect(Typer::unsupported(L"\xD83D\xDE00", map, Method::Unicode).empty(), "valid pair passes Unicode preflight");
    expect(!Typer::unsupported(L"a\xD83D", map, Method::Unicode).empty(), "unpaired high surrogate rejected");
    expect(!Typer::unsupported(L"a\xDE00", map, Method::Unicode).empty(), "unpaired low surrogate rejected");
    expect(!Typer::unsupported(L"a\b", map, Method::Unicode).empty(), "Unicode backspace is not literal text");
    expect(!Typer::unsupported(L"a\x1B", map, Method::Unicode).empty(), "Unicode escape control rejected");
    KeyMap shiftedReturn = Typer::shiftReturn(map);
    for (wchar_t newline : {L'\n', L'\r', wchar_t(0x0B), wchar_t(0x0C), wchar_t(0x85), wchar_t(0x2028), wchar_t(0x2029)}) {
        auto steps = Typer::stepsFor(newline, shiftedReturn, Method::Unicode, 80, 160);
        expect(steps.size() == 4 && steps[0].input.ki.wVk == VK_LSHIFT
            && steps[1].input.ki.wVk == VK_RETURN, "all newline forms have safe physical Shift+Enter steps");
        expect(!Typer::typeable(newline, shiftedReturn, Method::PostWindow), "posted messages refuse Shift+Enter");
    }
}

static void testDeadKeyIsolation() {
    HKL international = LoadKeyboardLayoutW(L"00020409", 0);
    expect(international != nullptr, "US International layout available for dead-key regression");
    if (!international) return;
    // Probing an accent must not compose with the next key in the map.
    expect(KeyMap::keyCharacter(VK_OEM_7, false, international) == 0, "dead key is not a direct character");
    expect(KeyMap::keyCharacter('E', false, international) == L'e', "dead key probe leaves next character unchanged");
    KeyMap map(international);
    expect(map.find(L'e') && !map.find(L'\u00E9'), "map does not invent a composed one-key character");
}
