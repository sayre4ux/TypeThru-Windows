// Unit tests: pure logic and constructed inputs only; nothing here posts input,
// touches the network, or changes settings (same contract as Tests.swift).
#include "../src/shared.h"
#include "../src/keymap.h"
#include "../src/typer.h"
#include "../src/hotkey.h"
#include "../src/settings.h"
#include "../src/updates.h"
#include "../src/updatekey.h"
#include <windows.h>
#include <stdio.h>
#include <cmath>
#include <string>

using namespace tt;

static int failures = 0;
static int checks = 0;

static void expect(bool condition, const char* what) {
    checks++;
    if (!condition) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

static void expectEqual(int a, int b, const char* what) {
    checks++;
    if (a != b) {
        failures++;
        printf("FAIL: %s (%d != %d)\n", what, a, b);
    }
}

static void testTrailingLineBreaks() {
    expect(withoutTrailingLineBreaks(L"cmd --run\n") == L"cmd --run", "one trailing newline stripped");
    expect(withoutTrailingLineBreaks(L"text\r\n\r\n") == L"text", "crlf pair stripped together");
    expect(withoutTrailingLineBreaks(L"a\nb") == L"a\nb", "inner newline kept");
    expect(withoutTrailingLineBreaks(L"\n") == L"", "only newline becomes empty");
    expect(withoutTrailingLineBreaks(L"\u2028x") == L"\u2028x", "inner separator kept");
}

static void testTiming() {
    int hold = 0, gap = 0;
    timing(0.24, hold, gap);
    expectEqual(hold, 80, "240 ms holds 80");
    expectEqual(gap, 160, "240 ms gaps 160");
    timing(0.02, hold, gap);
    expectEqual(hold, 10, "20 ms holds half");
    expectEqual(gap, 10, "20 ms gaps half");
    timing(0.03, hold, gap);
    expectEqual(hold + gap, 30, "30 ms splits whole");
    timing(-5, hold, gap);
    expectEqual(hold + gap, 20, "negative clamps to 20");
    double huge = 1e30;
    timing(huge, hold, gap);
    expectEqual(hold + gap, 580, "huge clamps to 580");
    timing(std::nan(""), hold, gap);
    expectEqual(hold + gap, 240, "nan falls back to 240");
}

static void testVersions() {
    expect(isNewerVersion(L"0.2", L"0.1.9"), "0.2 newer");
    expect(!isNewerVersion(L"0.1", L"0.1"), "same not newer");
    expect(isNewerVersion(L"v1.0.1-beta", L"1.0.0"), "tag prefix ignored");
    expect(!isNewerVersion(L"0.9", L"1.0"), "older rejected");
    expect(isNewerVersion(L"1.0.0.1", L"1.0.0"), "fourth number counts");
    expect(!isNewerVersion(L"v1.2.3-rc1", L"1.2.3"), "prerelease suffix not newer");
    expect(isNewerVersion(L"1.2.4-rc1", L"1.2.3"), "prerelease of higher version is newer");
}

static void testShortcutRules() {
    Shortcut standard = Shortcut::standard();
    expect(standard.problem().empty(), "Ctrl+backslash is allowed");
    expect(standard.caution().empty(), "Ctrl alone needs no caution");
    Shortcut esc{VK_ESCAPE, MOD_CONTROL};
    expect(!esc.problem().empty(), "Esc refused");
    Shortcut bare{'Q', 0};
    expect(!bare.problem().empty(), "no-modifier non-function key refused");
    Shortcut functionKey{VK_F6, 0};
    expect(functionKey.problem().empty(), "function key alone allowed");
    Shortcut alt{'X', MOD_ALT};
    expect(!alt.caution().empty(), "Alt gets a caution");
}

static void testReplacements() {
    expect(replacements().count(L'\u2022') == 1, "bullet in table");
    int replaced = 0;
    // Everything typeable except the smart symbols: they must all be replaced.
    auto typeable = [](wchar_t ch) {
        return replacements().find(ch) == replacements().end() && ch != L'\u2122';
    };
    std::wstring text = replacingUntypable(L"\u2022 a \u2013 b \u2014 c \u201C d \u2026 e \u2260 f", replaced, typeable);
    expect(replaced == 6, "all untypable symbols replaced");
    expect(text == L"- a - b -- c \" d ... e != f", "replacement text matches macOS table");
    // A character with no replacement stays and is reported by unsupported().
    expect(replacingUntypable(L"\u2603", replaced, [](wchar_t) { return false; }).size() == 1,
           "snowman kept when no substitute");
}

static void testKeyMapOnUsLayout() {
    HKL layout = LoadKeyboardLayoutW(L"00000409", 0);
    if (layout == nullptr) {
        printf("note: US layout not available; keymap spot checks skipped\n");
        return;
    }
    KeyMap map(layout);
    const KeyStroke* a = map.find(L'a');
    expect(a != nullptr && a->vk == 'A' && !a->shift, "a maps to unshifted A key");
    const KeyStroke* upper = map.find(L'A');
    expect(upper != nullptr && upper->vk == 'A' && upper->shift, "A maps to shifted A key");
    const KeyStroke* one = map.find(L'1');
    expect(one != nullptr && one->vk == '1' && !one->shift, "1 unshifted");
    const KeyStroke* bang = map.find(L'!');
    expect(bang != nullptr && bang->shift, "! needs shift");
    expect(Typer::unsupported(L"abc123 !@#\n\t", map, Method::Scancode).empty(),
           "ascii text fully typeable");
    expect(!Typer::unsupported(L"\u2022", map, Method::Scancode).empty(),
           "bullet has no direct key");
    expect(Typer::unsupported(L"\u2022", map, Method::Unicode).empty(),
           "bullet typable in unicode mode");
}

static void testLineBreaks() {
    expect(normalizeLineBreaks(L"a\r\nb") == L"a\nb", "crlf collapses to lf");
    expect(normalizeLineBreaks(L"a\rb") == L"a\nb", "lone cr becomes lf");
    expect(normalizeLineBreaks(L"a\r\r\nb") == L"a\n\nb", "cr cr lf keeps a blank line");
    expect(withoutTrailingLineBreaks(L"run\x0B") == L"run", "vertical tab stripped as trailing break");
    expect(withoutTrailingLineBreaks(L"run\x85") == L"run", "nel stripped as trailing break");
}

static void testSteps() {
    KeyMap map(GetKeyboardLayout(0));
    int hold = 80, gap = 160;
    std::vector<Typer::Step> lower = Typer::stepsFor(L'z', map, Method::Scancode, hold, gap);
    expectEqual(static_cast<int>(lower.size()), 2, "plain key is two inputs");
    if (lower.size() == 2) {
        expect(lower[0].input.ki.dwFlags & KEYEVENTF_SCANCODE, "key down uses scan code");
        expect(lower[0].waitMs == hold, "key held for hold time");
        expect(lower[1].input.ki.dwFlags & KEYEVENTF_KEYUP, "second input is key up");
    }
    std::vector<Typer::Step> upper = Typer::stepsFor(L'Z', map, Method::Scancode, hold, gap);
    expectEqual(static_cast<int>(upper.size()), 4, "shifted key is four inputs");
    if (upper.size() == 4) {
        expect(upper[0].input.ki.wVk == VK_LSHIFT, "shift goes down first");
        expect(!(upper[1].input.ki.dwFlags & KEYEVENTF_KEYUP), "key down after shift");
        expect(upper[3].input.ki.wVk == VK_LSHIFT && (upper[3].input.ki.dwFlags & KEYEVENTF_KEYUP),
               "shift released last");
    }
    std::vector<Typer::Step> unicode = Typer::stepsFor(L'\u00E9', map, Method::Unicode, hold, gap);
    expectEqual(static_cast<int>(unicode.size()), 2, "unicode char is two inputs");
    if (unicode.size() == 2) {
        expect(unicode[0].input.ki.dwFlags == KEYEVENTF_UNICODE, "unicode payload flag");
        expect(unicode[0].input.ki.wScan == 0xE9, "unicode unit carried in wScan");
    }
    // Shift+Return: newline maps to Return with shift held.
    KeyMap shifted = Typer::shiftReturn(map);
    const KeyStroke* enter = shifted.find(L'\n');
    expect(enter != nullptr && enter->vk == VK_RETURN && enter->shift, "newline as Shift+Enter");
}

static void testReleaseGates() {
    expect(updater::isReleasePage(L"https://github.com/sayre4ux/TypeThru-Windows/releases/tag/v0.2"),
           "own release page accepted");
    expect(!updater::isReleasePage(L"https://evil.example.com/sayre4ux/TypeThru-Windows/releases/tag/v0.2"),
           "other host refused");
    expect(!updater::isReleasePage(L"https://github.com/someone/TypeThru-Windows/releases/tag/v0.2"),
           "other owner refused");
    expect(updater::isReleaseFile(L"https://github.com/sayre4ux/TypeThru-Windows/releases/download/v0.2/TypeThru-0.2.0-win64.zip"),
           "own download accepted");
    expect(!updater::isReleaseFile(L"https://objects.githubusercontent.com/v0.2.zip"),
           "redirect host not accepted as an offer");
}

static const char* sampleJson = R"([
 {"tag_name":"v0.2.0","draft":false,"html_url":"https://github.com/sayre4ux/TypeThru-Windows/releases/tag/v0.2.0",
  "assets":[{"name":"TypeThru-0.2.0-win64.zip","browser_download_url":"https://github.com/sayre4ux/TypeThru-Windows/releases/download/v0.2.0/TypeThru-0.2.0-win64.zip"},
            {"name":"TypeThru-0.2.0-win64.zip.sig","browser_download_url":"https://github.com/sayre4ux/TypeThru-Windows/releases/download/v0.2.0/TypeThru-0.2.0-win64.zip.sig"}]},
 {"tag_name":"v9.9.9","draft":true,"html_url":"https://github.com/sayre4ux/TypeThru-Windows/releases/tag/v9.9.9","assets":[]},
 {"tag_name":"v0.1.0","draft":false,"html_url":"https://evil.example.com/x",
  "assets":[{"name":"a.zip","browser_download_url":"https://evil.example.com/a.zip"}]}
])";

static void testNewestFromJson() {
    std::optional<Release> newest = updater::newestFromJson(sampleJson);
    expect(newest.has_value(), "newest release found");
    if (newest) {
        expect(newest->version == L"0.2.0", "draft and foreign releases skipped");
        expect(newest->zipUrl.find(L"TypeThru-0.2.0-win64.zip") != std::wstring::npos, "zip picked");
        expect(newest->sigUrl == newest->zipUrl + L".sig", "sig pairs with zip");
    }
    expect(!updater::newestFromJson("not json").has_value(), "broken json rejected");
    expect(!updater::newestFromJson("[]").has_value(), "empty list gives none");
}

static void testSignature() {
    // CNG BCRYPT_ECCPUBLIC_BLOB for P-256: magic 0x31534345 stored little-endian
    // ('E','C','S','1'), cbKey = 32 per coordinate, then X(32) + Y(32) = 72 bytes.
    expectEqual(sizeof(updateKeyBlob), 72, "public key blob is 72 bytes");
    expectEqual(updateKeyBlob[0], 0x45, "ECS1 magic first byte");
    expectEqual(updateKeyBlob[3], 0x31, "ECS1 magic last byte");
    expectEqual(updateKeyBlob[4], 32, "coordinate length 32");
    // Garbage signatures never verify against the embedded key.
    const unsigned char data[] = "TypeThru";
    unsigned char broken[64] = {};
    expect(!updater::verifySignature(data, sizeof(data), broken, sizeof(broken)),
           "random signature rejected");

    // Round trip with a fresh CNG key pair: sign the data, verify through the app path.
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) == 0
        && BCryptGenerateKeyPair(algorithm, &key, 256, 0) == 0
        && BCryptFinalizeKeyPair(key, 0) == 0) {
        unsigned char digest[32];
        BCRYPT_HASH_HANDLE hash = nullptr;
        BCRYPT_ALG_HANDLE sha = nullptr;
        BCryptOpenAlgorithmProvider(&sha, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        BCryptCreateHash(sha, &hash, nullptr, 0, nullptr, 0, 0);
        BCryptHashData(hash, const_cast<unsigned char*>(data), sizeof(data), 0);
        BCryptFinishHash(hash, digest, sizeof(digest), 0);
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(sha, 0);
        ULONG signatureSize = 0, size = 0;
        BCryptSignHash(key, nullptr, digest, sizeof(digest), nullptr, 0, &signatureSize, 0);
        std::vector<unsigned char> signature(signatureSize);
        BCryptSignHash(key, nullptr, digest, sizeof(digest), signature.data(), signatureSize, &size, 0);
        signature.resize(size);
        ULONG blobSize = 0;
        BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &blobSize, 0);
        std::vector<unsigned char> blob(blobSize);
        BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob.data(), blobSize, &size, 0);
        expect(signature.size() == 64, "CNG produces raw 64-byte signatures");
        expect(updater::verifySignatureWithKey(data, sizeof(data), signature.data(), signature.size(),
                                               blob.data(), static_cast<unsigned long>(size)),
               "valid signature verifies");
        expect(!updater::verifySignatureWithKey(data, sizeof(data) + 1, signature.data(), signature.size(),
                                                blob.data(), static_cast<unsigned long>(size)),
               "tampered data rejected");
    } else {
        printf("note: CNG key generation unavailable; round trip skipped\n");
    }
    if (key) BCryptDestroyKey(key);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
}

int main() {
    testTrailingLineBreaks();
    testTiming();
    testVersions();
    testShortcutRules();
    testReplacements();
    testKeyMapOnUsLayout();
    testSteps();
    testReleaseGates();
    testNewestFromJson();
    testLineBreaks();
    testSignature();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
