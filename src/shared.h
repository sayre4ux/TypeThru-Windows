// Pure logic shared with the macOS app (sayre4ux/TypeThru, Sources/main.swift).
// Keep both sides in sync; changing a table here belongs in the same change there.
#pragma once
#include <string>
#include <map>
#include <vector>
#include <functional>

namespace tt {

struct Speed {
    const char* title;
    double interval;   // seconds per character
    const char* detail;
};

// DECISION: same five speeds as macOS so docs, muscle memory, and bug reports carry over.
inline const Speed speeds[] = {
    {"Average typist - 50 WPM",  0.24, "240 ms per key. A typical typist, and the most reliable."},
    {"Fast typist - 100 WPM",    0.12, "120 ms per key. The fastest 5% of typists."},
    {"Record typist - 200 WPM",  0.06, "60 ms per key. World-record pace."},
    {"Superhuman - 400 WPM",     0.03, "30 ms per key. Some remote sessions drop keys."},
    {"Unrealistic - 600 WPM",    0.02, "20 ms per key. Expect dropped keys on slow connections."},
};
inline constexpr int speedCount = 5;

// Symbols with no key on common layouts, typed as plain keys that read the same.
// DECISION: letters (such as U+00E9) are not replaced, because dropping an accent
// changes a word. Line and paragraph separators become a normal line break.
const std::map<wchar_t, std::wstring>& replacements();

struct TypingTest {
    const char* title;
    const char* help;
    std::wstring text;
    bool usesSelectedSpeed;
};

// Built-in samples for checking a method, speed, and layout against the remote session.
const std::vector<TypingTest>& typingTests();

bool isNewlineChar(wchar_t ch);

// Leaves out line breaks at the end, so a copied command does not run by itself.
std::wstring withoutTrailingLineBreaks(const std::wstring& text);

// CRLF and lone CR become LF, so one line break is one key press.
std::wstring normalizeLineBreaks(const std::wstring& text);

// Extra wait after a line break, on top of the typing interval: remote apps can
// handle a line break after the keys that follow it (DECISION: ported from macOS).
inline constexpr double lineBreakSettleSeconds = 0.3;
double settleAfter(wchar_t ch);

// Splits the time per character into key hold and gap, both in ms.
// DECISION: keep the proven 80 ms hold when there is time; faster speeds hold half.
void timing(double intervalSeconds, int& holdMs, int& gapMs);

// "v0.3.0-beta" -> 0.3.0 ; returns the part count.
int versionNumbers(const std::wstring& version, int out[4]);
bool isNewerVersion(const std::wstring& candidate, const std::wstring& current);

// Applies replacements for characters the predicate rejects; counts what changed.
// Pure: the caller supplies typability, so tests need no keyboard layout.
std::wstring replacingUntypable(const std::wstring& text, int& replacedCount,
                                const std::function<bool(wchar_t)>& typeable);

}
