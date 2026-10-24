#include "shared.h"
#include <cwctype>

namespace tt {

const std::map<wchar_t, std::wstring>& replacements() {
    // DECISION: byte-for-byte the macOS table; sync with Sources/main.swift Typer.replacements.
    static const std::map<wchar_t, std::wstring> table = {
        {L'\u2022', L"-"}, {L'\u25E6', L"-"}, {L'\u25AA', L"-"}, {L'\u25AB', L"-"}, {L'\u2023', L"-"},
        {L'\u2043', L"-"}, {L'\u25CF', L"-"}, {L'\u25CB', L"-"}, {L'\u25A0', L"-"}, {L'\u25A1', L"-"},
        {L'\u00B7', L"-"}, {L'\u2219', L"-"}, {L'\u2010', L"-"}, {L'\u2011', L"-"}, {L'\u2012', L"-"},
        {L'\u2013', L"-"}, {L'\u2212', L"-"}, {L'\u2014', L"--"}, {L'\u2015', L"--"},
        {L'\u2018', L"'"}, {L'\u2019', L"'"}, {L'\u201A', L"'"}, {L'\u2032', L"'"},
        {L'\u201C', L"\""}, {L'\u201D', L"\""}, {L'\u201E', L"\""}, {L'\u2033', L"\""},
        {L'\u00AB', L"\""}, {L'\u00BB', L"\""},
        {L'\u2026', L"..."}, {L'\u00D7', L"x"}, {L'\u00F7', L"/"},
        {L'\u2192', L"->"}, {L'\u2190', L"<-"}, {L'\u21D2', L"=>"},
        {L'\u2264', L"<="}, {L'\u2265', L">="}, {L'\u2260', L"!="},
        {L'\u00A9', L"(c)"}, {L'\u00AE', L"(R)"}, {L'\u2122', L"(TM)"},
        {L'\u00A0', L" "}, {L'\u2002', L" "}, {L'\u2003', L" "}, {L'\u2009', L" "}, {L'\u202F', L" "},
        {L'\u200B', L""}, {L'\uFEFF', L""},
        // Line and paragraph separators, and Word's manual line break, become a normal line break.
        {L'\u2028', L"\n"}, {L'\u2029', L"\n"}, {L'\u000B', L"\n"}, {L'\u0085', L"\n"},
    };
    return table;
}

const std::vector<TypingTest>& typingTests() {
    static const std::vector<TypingTest> tests = {
        {"Short", "Types abc123 at the slowest speed.", L"abc123", false},
        {"Capitals", "Types AbC123! at the slowest speed, to check Shift.", L"AbC123!", false},
        {"Symbols", "Types all 32 keyboard symbols at the slowest speed.",
         L"!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~", false},
        {"Smart symbols",
         "Types bullets, smart quotes, and dashes as plain keys: - Bullet - en -- em \"double\" "
         "'single' ... 3x4/2 -> <= >= != (c) (R) (TM)",
         L"\u2022 Bullet \u2013 en \u2014 em \u201Cdouble\u201D \u2018single\u2019 \u2026 3\u00D74\u00F72 "
         L"\u2192 \u2264 \u2265 \u2260 \u00A9 \u00AE \u2122",
         false},
        {"Long", "Types four lines with Tab and Return at your selected speed. Use a multi-line text field.",
         L"The quick brown fox jumps over the lazy dog. Pack my box with five dozen liquor jugs!\n"
         L"Invoice #2048: $1,299.50 (incl. 8% tax) due 2026-10-31; ref A-17/B.\n"
         L"\tIndented: test@example.com, C:\\Temp\\notes.txt, {braces} [brackets] <angles>.\n"
         L"Sphinx of black quartz, judge my vow? 0123456789 ~ ` ^ | _ + = \" '",
         true},
    };
    return tests;
}

bool isNewlineChar(wchar_t ch) {
    // The set Swift's Character.isNewline covers: LF, CR, VT, FF, NEL, LS, PS.
    return ch == L'\n' || ch == L'\r' || ch == 0x0B || ch == 0x0C || ch == 0x85
        || ch == L'\u2028' || ch == L'\u2029';
}

std::wstring withoutTrailingLineBreaks(const std::wstring& text) {
    size_t end = text.size();
    while (end > 0 && isNewlineChar(text[end - 1])) end--;
    return text.substr(0, end);
}

// Windows clipboard text carries CRLF, and a C++ loop would press Return twice
// (Swift saw "\r\n" as one character, so the macOS app never needed this).
std::wstring normalizeLineBreaks(const std::wstring& text) {
    std::wstring result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++) {
        wchar_t ch = text[i];
        if (ch == L'\r') {
            if (i + 1 < text.size() && text[i + 1] == L'\n') i++;
            result += L'\n';
        } else {
            result += ch;
        }
    }
    return result;
}

double settleAfter(wchar_t ch) { return isNewlineChar(ch) ? lineBreakSettleSeconds : 0.0; }

void timing(double intervalSeconds, int& holdMs, int& gapMs) {
    // Clamp before converting: a damaged saved setting (huge, infinite, or NaN) must not crash.
    double ms = (std::isnan(intervalSeconds)) ? 240.0 : intervalSeconds * 1000.0;
    if (ms < 20.0) ms = 20.0;
    if (ms > 580.0) ms = 580.0;
    int total = static_cast<int>(ms + 0.5);
    holdMs = total / 2 < 80 ? total / 2 : 80;
    gapMs = total - holdMs;
}

int versionNumbers(const std::wstring& version, int out[4]) {
    for (int i = 0; i < 4; i++) out[i] = 0;
    // Skip a non-numeric prefix such as "v", then read digits and dots only:
    // "1.2.3-rc1" must parse as 1.2.3, not 1.2.3.1.
    int index = 0;
    while (index < static_cast<int>(version.size()) && !std::iswdigit(version[index])) index++;
    int count = 0;
    while (index < static_cast<int>(version.size()) && count < 4) {
        if (std::iswdigit(version[index])) {
            long long value = 0;   // saturate, never overflow
            while (index < static_cast<int>(version.size()) && std::iswdigit(version[index])) {
                value = value * 10 + (version[index] - L'0');
                if (value > 1000000) value = 1000000;
                index++;
            }
            out[count++] = static_cast<int>(value);
            if (index < static_cast<int>(version.size()) && version[index] == L'.') index++;
        } else {
            break;   // a hyphen or any other character ends the version
        }
    }
    return count;
}

bool isNewerVersion(const std::wstring& candidate, const std::wstring& current) {
    int a[4], b[4];
    versionNumbers(candidate, a);
    versionNumbers(current, b);
    for (int i = 0; i < 4; i++) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return false;
}

std::wstring replacingUntypable(const std::wstring& text, int& replacedCount,
                                const std::function<bool(wchar_t)>& typeable) {
    std::wstring result;
    replacedCount = 0;
    for (wchar_t ch : text) {
        const auto& table = replacements();
        auto entry = table.find(ch);
        if (!typeable(ch) && entry != table.end()) {
            bool all = true;
            for (wchar_t sub : entry->second) {
                if (sub != L'\n' && !typeable(sub)) { all = false; break; }
            }
            if (all) {
                result += entry->second;
                replacedCount++;
                continue;
            }
        }
        result += ch;
    }
    return result;
}

}
