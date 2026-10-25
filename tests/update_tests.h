#pragma once
#include "../src/updates.h"
#include <string>

inline void testUpdaterHardening(void (*expect)(bool, const char*)) {
    using tt::updater::isInstallableVersion;
    using tt::updater::isReleaseFile;
    using tt::updater::isReleasePage;
    using tt::updater::newestFromJson;

    const std::string valid = R"([{"draft":false,"tag_name":"v0.2.0","html_url":"https://github.com/sayre4ux/TypeThru-Windows/releases/tag/v0.2.0","assets":[{"name":"TypeThru-0.2.0-win64.zip","browser_download_url":"https://github.com/sayre4ux/TypeThru-Windows/releases/download/v0.2.0/TypeThru-0.2.0-win64.zip"},{"name":"TypeThru-0.2.0-win64.zip.sig","browser_download_url":"https://github.com/sayre4ux/TypeThru-Windows/releases/download/v0.2.0/TypeThru-0.2.0-win64.zip.sig"}],"extra":null}])";
    auto validOffer = newestFromJson(valid);
    expect(validOffer.has_value() && validOffer->version == L"0.2.0",
           "strict parser accepts a valid signed release record");

    expect(!newestFromJson(valid + " trailing").has_value(),
           "JSON trailing garbage is rejected");
    expect(!newestFromJson(valid + " {} ").has_value(),
           "JSON accepts only one complete document");

    std::string duplicate = valid;
    duplicate.replace(duplicate.find("\"draft\":false"), 14,
                      "\"draft\":false,\"draft\":true");
    expect(!newestFromJson(duplicate).has_value(), "duplicate JSON object keys are rejected");

    std::string rawControl = valid;
    rawControl.insert(rawControl.find("v0.2.0") + 5, 1, '\x01');
    expect(!newestFromJson(rawControl).has_value(), "unescaped JSON control characters are rejected");

    std::string brokenHighSurrogate = valid;
    brokenHighSurrogate.replace(brokenHighSurrogate.find("\"extra\":null"), 12,
                                "\"extra\":\"\\uD800\"");
    expect(!newestFromJson(brokenHighSurrogate).has_value(), "unpaired high JSON surrogate is rejected");
    std::string brokenLowSurrogate = valid;
    brokenLowSurrogate.replace(brokenLowSurrogate.find("\"extra\":null"), 12,
                               "\"extra\":\"\\uDC00\"");
    expect(!newestFromJson(brokenLowSurrogate).has_value(), "unpaired low JSON surrogate is rejected");
    std::string brokenPair = valid;
    brokenPair.replace(brokenPair.find("\"extra\":null"), 12,
                       "\"extra\":\"\\uD800\\u0041\"");
    expect(!newestFromJson(brokenPair).has_value(), "invalid JSON surrogate pair is rejected");
    std::string validPair = valid;
    validPair.replace(validPair.find("\"extra\":null"), 12,
                      "\"extra\":\"\\uD83D\\uDE00\"");
    expect(newestFromJson(validPair).has_value(), "valid JSON surrogate pair is accepted");

    std::string badNumber = valid;
    badNumber.replace(badNumber.find("\"extra\":null"), 12, "\"extra\":01");
    expect(!newestFromJson(badNumber).has_value(), "JSON leading-zero numbers are rejected");
    badNumber = valid;
    badNumber.replace(badNumber.find("\"extra\":null"), 12, "\"extra\":NaN");
    expect(!newestFromJson(badNumber).has_value(), "non-JSON numeric tokens are rejected");
    badNumber = valid;
    badNumber.replace(badNumber.find("\"extra\":null"), 12, "\"extra\":1e+");
    expect(!newestFromJson(badNumber).has_value(), "incomplete JSON exponents are rejected");

    std::string deep = valid;
    std::string nested(40, '[');
    nested += "null";
    nested.append(40, ']');
    deep.replace(deep.find("\"extra\":null"), 12, "\"extra\":" + nested);
    expect(!newestFromJson(deep).has_value(), "deeply nested JSON is rejected");
    std::string oversized = valid;
    oversized.append((1 << 20), ' ');
    expect(!newestFromJson(oversized).has_value(), "oversized release JSON is rejected");

    expect(isReleasePage(L"https://github.com/sayre4ux/TypeThru-Windows/releases/tag/v0.2.0"),
           "canonical release page URL is accepted");
    expect(!isReleasePage(L"https://github.com/sayre4ux/TypeThru-Windows/releases/tag/v0.2.0?x=1"),
           "release page query strings are rejected");
    expect(!isReleasePage(L"https://github.com/sayre4ux/TypeThru-Windows/releases/tag/v0.2.0#x"),
           "release page fragments are rejected");
    expect(!isReleasePage(L"https://github.com/sayre4ux/TypeThru-Windows/releases\\tag\\v0.2.0"),
           "backslash release paths are rejected");
    expect(!isReleasePage(L"https://github.com/sayre4ux/TypeThru-Windows/releases/tag/%2e%2e"),
           "percent-encoded path traversal is rejected");
    std::wstring nulUrl = L"https://github.com/sayre4ux/TypeThru-Windows/releases/tag/v0.2.0";
    nulUrl.push_back(L'\0');
    nulUrl += L"/ignored";
    expect(!isReleasePage(nulUrl), "embedded NUL in a release URL is rejected");
    expect(isReleaseFile(L"https://github.com/sayre4ux/TypeThru-Windows/releases/download/v0.2.0/TypeThru-0.2.0-win64.zip"),
           "canonical release asset URL is accepted");
    expect(!isReleaseFile(L"https://github.com/sayre4ux/TypeThru-Windows/releases/download/v0.2.0/TypeThru.zip?download=1"),
           "release asset query strings are rejected");
    expect(!isReleaseFile(L"https://github.com/sayre4ux/TypeThru-Windows/releases/download/v0.2.0/../evil.zip"),
           "release asset traversal segments are rejected");

    expect(isInstallableVersion(L"0.2.0", L"0.2.0", L"0.1.0"),
           "matching newer resource version is installable");
    expect(!isInstallableVersion(L"0.2.1", L"0.2.0", L"0.1.0"),
           "candidate resource version must match the offer");
    expect(!isInstallableVersion(L"0.1.0", L"0.1.0", L"0.2.0"),
           "older signed release cannot be installed");
    expect(!isInstallableVersion(L"0.2.0", L"0.2.0", L"0.2.0"),
           "same-version update is rejected");
    expect(!isInstallableVersion(L"0.2.0-beta", L"0.2.0", L"0.1.0"),
           "noncanonical executable version is rejected");
}
