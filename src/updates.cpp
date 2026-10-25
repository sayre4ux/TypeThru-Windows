#include "updates.h"
#include "updatekey.h"
#include "shared.h"
#include "version.h"
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <iterator>
#include <string_view>
#include <utility>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "version.lib")

namespace tt::updater {

const wchar_t* repoOwner = L"sayre4ux";
const wchar_t* repoName = L"TypeThru-Windows";

// ---------- tiny JSON reader (enough for the releases endpoint) ----------

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    std::string text;                          // for String
    std::vector<Json> items;                   // for Array
    std::map<std::string, Json> fields;        // for Object

    const Json* at(const char* name) const {
        auto it = fields.find(name);
        return it == fields.end() ? nullptr : &it->second;
    }
};

struct JsonParser {
    static constexpr size_t maxDepth = 32;
    static constexpr size_t maxNodes = 20000;
    static constexpr size_t maxCollectionItems = 4096;
    static constexpr size_t maxStringBytes = 1 << 18;

    const char* p;
    const char* end;
    size_t nodes = 0;
    explicit JsonParser(const std::string& s) : p(s.data()), end(s.data() + s.size()) {}
    void skip() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }
    bool literal(const char* word) {
        size_t n = strlen(word);
        if (static_cast<size_t>(end - p) < n || strncmp(p, word, n) != 0) return false;
        p += n;
        return true;
    }
    void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    static bool hexDigit(char c, unsigned& value) {
        if (c >= '0' && c <= '9') value = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') value = static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') value = static_cast<unsigned>(c - 'A' + 10);
        else return false;
        return true;
    }
    bool hex4(unsigned& value) {
        if (end - p < 4) return false;
        value = 0;
        for (int i = 0; i < 4; i++) {
            unsigned digit = 0;
            if (!hexDigit(p[i], digit)) return false;
            value = (value << 4) | digit;
        }
        p += 4;
        return true;
    }
    bool rawUtf8(size_t& byteCount) const {
        const unsigned char first = static_cast<unsigned char>(*p);
        unsigned cp = 0;
        if (first >= 0xC2 && first <= 0xDF) { cp = first & 0x1F; byteCount = 2; }
        else if (first >= 0xE0 && first <= 0xEF) { cp = first & 0x0F; byteCount = 3; }
        else if (first >= 0xF0 && first <= 0xF4) { cp = first & 0x07; byteCount = 4; }
        else return false;
        if (static_cast<size_t>(end - p) < byteCount) return false;
        for (size_t i = 1; i < byteCount; i++) {
            const unsigned char next = static_cast<unsigned char>(p[i]);
            if ((next & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (next & 0x3F);
        }
        if ((byteCount == 3 && cp < 0x800) || (byteCount == 4 && cp < 0x10000)
            || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        return true;
    }
    bool string(std::string& out) {
        if (p >= end || *p != '"') return false;
        p++;
        out.clear();
        while (p < end && *p != '"') {
            if (out.size() >= maxStringBytes) return false;
            if (*p != '\\') {
                const unsigned char c = static_cast<unsigned char>(*p);
                if (c < 0x20) return false;
                if (c < 0x80) { out += *p++; continue; }
                size_t bytes = 0;
                if (!rawUtf8(bytes)) return false;
                out.append(p, bytes);
                p += bytes;
                continue;
            }
            p++;
            if (p >= end) return false;
            switch (*p) {
                case '"': out += '"'; p++; break;
                case '\\': out += '\\'; p++; break;
                case '/': out += '/'; p++; break;
                case 'n': out += '\n'; p++; break;
                case 't': out += '\t'; p++; break;
                case 'r': out += '\r'; p++; break;
                case 'b': out += '\b'; p++; break;
                case 'f': out += '\f'; p++; break;
                case 'u': {
                    p++;
                    unsigned cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (end - p < 6 || p[0] != '\\' || p[1] != 'u') return false;
                        p += 2;
                        unsigned low = 0;
                        if (!hex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return false;
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        if (p >= end) return false;
        if (out.size() > maxStringBytes) return false;
        p++;
        return true;
    }
    bool number() {
        const char* start = p;
        if (p < end && *p == '-') p++;
        if (p >= end) return false;
        if (*p == '0') {
            p++;
            if (p < end && *p >= '0' && *p <= '9') return false;
        } else {
            if (*p < '1' || *p > '9') return false;
            while (p < end && *p >= '0' && *p <= '9') p++;
        }
        if (p < end && *p == '.') {
            p++;
            if (p >= end || *p < '0' || *p > '9') return false;
            while (p < end && *p >= '0' && *p <= '9') p++;
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            p++;
            if (p < end && (*p == '+' || *p == '-')) p++;
            if (p >= end || *p < '0' || *p > '9') return false;
            while (p < end && *p >= '0' && *p <= '9') p++;
        }
        return p != start;
    }
    bool value(Json& out, size_t depth = 0) {
        if (depth > maxDepth || ++nodes > maxNodes) return false;
        skip();
        if (p >= end) return false;
        if (*p == '{') {
            out.type = Json::Type::Object;
            p++;
            skip();
            if (p < end && *p == '}') { p++; return true; }
            while (true) {
                skip();
                std::string name;
                if (!string(name)) return false;
                skip();
                if (p >= end || *p != ':') return false;
                p++;
                Json child;
                if (!value(child, depth + 1)) return false;
                if (out.fields.size() >= maxCollectionItems
                    || !out.fields.emplace(std::move(name), std::move(child)).second) return false;
                skip();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == '}') { p++; return true; }
                return false;
            }
        }
        if (*p == '[') {
            out.type = Json::Type::Array;
            p++;
            skip();
            if (p < end && *p == ']') { p++; return true; }
            while (true) {
                Json child;
                if (!value(child, depth + 1)) return false;
                if (out.items.size() >= maxCollectionItems) return false;
                out.items.push_back(std::move(child));
                skip();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == ']') { p++; return true; }
                return false;
            }
        }
        if (*p == '"') {
            out.type = Json::Type::String;
            return string(out.text);
        }
        if (literal("true")) { out.type = Json::Type::Bool; out.boolean = true; return true; }
        if (literal("false")) { out.type = Json::Type::Bool; out.boolean = false; return true; }
        if (literal("null")) { out.type = Json::Type::Null; return true; }
        if (!number()) return false;
        out.type = Json::Type::Number;
        return true;
    }
    bool document(Json& out) {
        if (end - p > (1 << 20) || !value(out)) return false;
        skip();
        return p == end;
    }
};

static std::wstring utf8ToWide(const std::string& text) {
    if (text.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size == 0 && !text.empty()) return {};
    std::wstring wide(size, 0);
    if (size && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), size) != size)
        return {};
    return wide;
}

// ---------- URL gates ----------

static bool safePathSegment(const std::wstring& segment) {
    if (segment.empty() || segment == L"." || segment == L"..") return false;
    for (wchar_t c : segment) {
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z')
              || (c >= L'0' && c <= L'9') || c == L'.' || c == L'-' || c == L'_')) return false;
    }
    return true;
}

static bool safeOwnGithubUrl(const std::wstring& url,
                             std::initializer_list<std::wstring_view> expectedPrefix,
                             size_t trailingSegments) {
    const std::wstring root = std::wstring(L"https://github.com/") + repoOwner + L"/" + repoName + L"/";
    if (url.size() > 2048 || url.compare(0, root.size(), root) != 0) return false;
    std::wstring tail = url.substr(root.size());
    for (wchar_t c : tail) {
        if (c < 0x21 || c > 0x7E || c == L'%' || c == L'\\' || c == L'?' || c == L'#') return false;
    }
    std::vector<std::wstring> segments;
    size_t start = 0;
    while (start <= tail.size()) {
        size_t slash = tail.find(L'/', start);
        size_t endSegment = slash == std::wstring::npos ? tail.size() : slash;
        std::wstring segment = tail.substr(start, endSegment - start);
        if (!safePathSegment(segment)) return false;
        segments.push_back(std::move(segment));
        if (slash == std::wstring::npos) break;
        start = slash + 1;
    }
    if (segments.size() != expectedPrefix.size() + trailingSegments) return false;
    size_t i = 0;
    for (std::wstring_view expected : expectedPrefix) {
        if (std::wstring_view(segments[i++]) != expected) return false;
    }
    return true;
}

bool isReleasePage(const std::wstring& url) {
    return safeOwnGithubUrl(url, {L"releases", L"tag"}, 1);
}

bool isReleaseFile(const std::wstring& url) {
    return safeOwnGithubUrl(url, {L"releases", L"download"}, 2);
}

struct VersionParts {
    unsigned value[4] = {};
};

static bool parseVersion(const std::wstring& text, VersionParts& out, bool allowSuffix = false) {
    out = {};
    if (text.empty() || text.size() > 128) return false;
    size_t pos = 0;
    unsigned count = 0;
    while (pos < text.size()) {
        if (count == 4 || text[pos] < L'0' || text[pos] > L'9') return false;
        size_t start = pos;
        unsigned value = 0;
        while (pos < text.size() && text[pos] >= L'0' && text[pos] <= L'9') {
            unsigned digit = static_cast<unsigned>(text[pos++] - L'0');
            if (value > 1000000U / 10U || value * 10U + digit > 1000000U) return false;
            value = value * 10U + digit;
        }
        if (pos - start > 1 && text[start] == L'0') return false;
        out.value[count++] = value;
        if (pos == text.size()) return true;
        if (text[pos] == L'.') {
            pos++;
            if (pos == text.size()) return false;
            continue;
        }
        if (allowSuffix && text[pos] == L'-' && pos + 1 < text.size()) {
            for (size_t i = pos + 1; i < text.size(); i++) {
                wchar_t c = text[i];
                if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z')
                      || (c >= L'0' && c <= L'9') || c == L'.' || c == L'-')) return false;
            }
            return true;
        }
        return false;
    }
    return count != 0;
}

static bool sameVersion(const std::wstring& left, const std::wstring& right) {
    VersionParts a, b;
    return parseVersion(left, a) && parseVersion(right, b)
        && std::equal(a.value, a.value + ARRAYSIZE(a.value), b.value);
}

static bool newerVersion(const std::wstring& candidate, const std::wstring& current) {
    VersionParts a, b;
    if (!parseVersion(candidate, a) || !parseVersion(current, b)) return false;
    for (size_t i = 0; i < 4; i++) {
        if (a.value[i] != b.value[i]) return a.value[i] > b.value[i];
    }
    return false;
}

static bool normalizedTagVersion(const std::wstring& tag, std::wstring& normalized) {
    std::wstring_view version(tag);
    if (!version.empty() && (version.front() == L'v' || version.front() == L'V')) version.remove_prefix(1);
    VersionParts parts;
    if (!parseVersion(std::wstring(version), parts, true)) return false;
    std::wstring_view numeric = version;
    size_t suffix = numeric.find(L'-');
    if (suffix != std::wstring_view::npos) numeric = numeric.substr(0, suffix);
    size_t componentCount = 1;
    for (wchar_t c : numeric) if (c == L'.') componentCount++;
    normalized.clear();
    for (size_t i = 0; i < componentCount; i++) {
        if (i) normalized += L'.';
        normalized += std::to_wstring(parts.value[i]);
    }
    return !normalized.empty();
}

static bool releaseFileForTag(const std::wstring& url, const std::wstring& tag) {
    const std::wstring prefix = std::wstring(L"https://github.com/") + repoOwner + L"/" + repoName
                              + L"/releases/download/" + tag + L"/";
    return isReleaseFile(url) && url.compare(0, prefix.size(), prefix) == 0;
}

static bool releasePageTagForVersion(const std::wstring& url, const std::wstring& version,
                                     std::wstring& tag) {
    const std::wstring prefix = std::wstring(L"https://github.com/") + repoOwner + L"/" + repoName
                              + L"/releases/tag/";
    if (!isReleasePage(url) || url.compare(0, prefix.size(), prefix) != 0) return false;
    tag = url.substr(prefix.size());
    std::wstring normalized;
    return normalizedTagVersion(tag, normalized) && sameVersion(normalized, version);
}

std::optional<Release> newestFromJson(const std::string& json) {
    JsonParser parser(json);
    Json root;
    if (!parser.document(root) || root.type != Json::Type::Array) return std::nullopt;
    std::optional<Release> best;
    for (const Json& item : root.items) {
        if (item.type != Json::Type::Object) continue;
        const Json* draft = item.at("draft");
        if (!draft || draft->type != Json::Type::Bool || draft->boolean) continue;
        const Json* tag = item.at("tag_name");
        const Json* page = item.at("html_url");
        if (!tag || tag->type != Json::Type::String) continue;
        const std::wstring tagWide = utf8ToWide(tag->text);
        std::wstring normalized;
        if (!normalizedTagVersion(tagWide, normalized)) continue;
        const std::wstring expectedPage = std::wstring(L"https://github.com/") + repoOwner + L"/" + repoName
                                        + L"/releases/tag/" + tagWide;
        if (!page || page->type != Json::Type::String
            || utf8ToWide(page->text) != expectedPage || !isReleasePage(expectedPage)) continue;
        Release release;
        // "v0.3.0-beta" is offered as "0.3.0", like the macOS app.
        release.version = normalized;
        release.pageUrl = expectedPage;
        // The zip and its signature, attached as TypeThru-<version>-win64.zip and .zip.sig.
        std::map<std::string, std::wstring> assets;
        if (const Json* list = item.at("assets")) {
            for (const Json& asset : list->items) {
                const Json* name = asset.at("name");
                const Json* link = asset.at("browser_download_url");
                if (name && name->type == Json::Type::String && link && link->type == Json::Type::String
                    && releaseFileForTag(utf8ToWide(link->text), tagWide)) {
                    assets[name->text] = utf8ToWide(link->text);
                }
            }
        }
        // DECISION: prefer the product zip for this exact version; fall back to the
        // first zip with a .sig sibling. A release with no signed zip is not offered.
        std::string exact = "TypeThru-";
        for (wchar_t c : normalized) exact += static_cast<char>(c);   // digits and dots only
        exact += "-win64.zip";
        if (assets.count(exact) && assets.count(exact + ".sig")) {
            release.zipUrl = assets.at(exact);
            release.sigUrl = assets.at(exact + ".sig");
        } else {
            for (const auto& [name, url] : assets) {
                if (name.size() > 4 && name.compare(name.size() - 4, 4, ".zip") == 0
                    && assets.count(name + ".sig")) {
                    release.zipUrl = url;
                    release.sigUrl = assets.at(name + ".sig");
                    break;
                }
            }
        }
        if (release.zipUrl.empty()) continue;
        if (!best || newerVersion(release.version, best->version)) best = release;
    }
    return best;
}

static bool isReleaseJsonDocument(const std::string& json) {
    JsonParser parser(json);
    Json root;
    return parser.document(root) && root.type == Json::Type::Array;
}

// ---------- transport ----------

struct HttpHandle {
    HINTERNET handle = nullptr;
    ~HttpHandle() { if (handle) WinHttpCloseHandle(handle); }
};

static bool httpGet(const std::wstring& url, const wchar_t* accept, size_t limit,
                    std::vector<unsigned char>& out) {
    if (url.empty() || url.size() > 4096 || limit == 0) return false;
    const ULONGLONG startedAt = GetTickCount64();
    constexpr ULONGLONG maxRequestMs = 120000;
    const auto withinDeadline = [startedAt, maxRequestMs]() {
        return GetTickCount64() - startedAt <= maxRequestMs;
    };
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {}, path[2048] = {}, extra[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(ARRAYSIZE(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(ARRAYSIZE(path));
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = static_cast<DWORD>(ARRAYSIZE(extra));
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) return false;
    if (parts.nScheme != INTERNET_SCHEME_HTTPS || parts.nPort != INTERNET_DEFAULT_HTTPS_PORT
        || parts.dwUserNameLength != 0 || parts.dwPasswordLength != 0
        || parts.dwHostNameLength == 0 || parts.dwHostNameLength >= static_cast<DWORD>(ARRAYSIZE(host))
        || parts.dwUrlPathLength >= static_cast<DWORD>(ARRAYSIZE(path))
        || parts.dwExtraInfoLength >= static_cast<DWORD>(ARRAYSIZE(extra))) return false;

    HttpHandle session, connect, request;
    session.handle = WinHttpOpen(L"TypeThru", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.handle) return false;
    if (!WinHttpSetTimeouts(session.handle, 15000, 15000, 30000, 30000)) return false;
    connect.handle = WinHttpConnect(session.handle, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect.handle) return false;
    std::wstring requestPath(path, parts.dwUrlPathLength);
    if (requestPath.empty()) requestPath = L"/";
    requestPath.append(extra, parts.dwExtraInfoLength);
    LPCWSTR acceptTypes[2] = {accept, nullptr};
    request.handle = WinHttpOpenRequest(connect.handle, L"GET", requestPath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                        accept ? acceptTypes : static_cast<LPCWSTR*>(WINHTTP_DEFAULT_ACCEPT_TYPES),
                                        WINHTTP_FLAG_SECURE);
    if (!request.handle) return false;
    if (accept) {
        const std::wstring header = std::wstring(L"Accept: ") + accept;
        if (!WinHttpAddRequestHeaders(request.handle, header.c_str(), static_cast<DWORD>(-1),
                                      WINHTTP_ADDREQ_FLAG_ADD)) return false;
    }
    if (!withinDeadline()
        || !WinHttpSendRequest(request.handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                            0, 0, 0)
        || !withinDeadline() || !WinHttpReceiveResponse(request.handle, nullptr) || !withinDeadline()) {
        return false;
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.handle,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) return false;
    out.clear();
    unsigned char buffer[8192];
    while (true) {
        if (!withinDeadline()) return false;
        DWORD read = 0;
        if (!WinHttpReadData(request.handle, buffer, sizeof(buffer), &read) || !withinDeadline()) return false;
        if (read == 0) break;
        if (out.size() > limit || static_cast<size_t>(read) > limit - out.size()) return false;
        out.insert(out.end(), buffer, buffer + read);
    }
    return true;
}

bool downloadToMemory(const std::wstring& url, std::vector<unsigned char>& out, size_t limit) {
    out.clear();
    return httpGet(url, nullptr, limit, out);
}

UpdateOutcome checkForNewer(const std::wstring& currentVersion, Release& found) {
    std::wstring feed = std::wstring(L"https://api.github.com/repos/") + repoOwner + L"/" + repoName
                      + L"/releases?per_page=10";
    std::vector<unsigned char> body;
    if (!httpGet(feed, L"application/vnd.github+json", 1 << 20, body)) return UpdateOutcome::NetworkError;
    std::string response;
    if (!body.empty()) response.assign(reinterpret_cast<const char*>(body.data()), body.size());
    if (!isReleaseJsonDocument(response)) return UpdateOutcome::NetworkError;
    std::optional<Release> newest = newestFromJson(response);
    if (!newest || !newerVersion(newest->version, currentVersion)) return UpdateOutcome::UpToDate;
    found = *newest;
    return UpdateOutcome::Available;
}

// ---------- signature ----------

// ECDSA P-256 over the SHA-256 of `data`, raw r||s signature, with the given
// BCRYPT_ECCPUBLIC_BLOB. Public so tests can round-trip with their own key pair.
bool verifySignatureWithKey(const unsigned char* data, size_t dataLength,
                            const unsigned char* signature, size_t signatureLength,
                            const unsigned char* publicKeyBlob, ULONG publicKeyBlobLength) {
    constexpr size_t p256CoordinateBytes = 32;
    constexpr size_t rawSignatureBytes = 2 * p256CoordinateBytes;
    if ((!data && dataLength) || !signature || signatureLength != rawSignatureBytes || !publicKeyBlob
        || dataLength > static_cast<size_t>((std::numeric_limits<ULONG>::max)())
        || publicKeyBlobLength != sizeof(BCRYPT_ECCKEY_BLOB) + rawSignatureBytes) return false;
    BCRYPT_ECCKEY_BLOB header{};
    memcpy(&header, publicKeyBlob, sizeof(header));
    if (header.dwMagic != BCRYPT_ECDSA_PUBLIC_P256_MAGIC || header.cbKey != p256CoordinateBytes) return false;
    bool valid = false;
    BCRYPT_ALG_HANDLE hashAlgorithm = nullptr, signAlgorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    do {
        if (BCryptOpenAlgorithmProvider(&hashAlgorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) break;
        if (BCryptOpenAlgorithmProvider(&signAlgorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0) break;
        if (BCryptCreateHash(hashAlgorithm, &hash, nullptr, 0, nullptr, 0, 0) != 0) break;
        static const unsigned char empty = 0;
        if (BCryptHashData(hash, const_cast<unsigned char*>(data ? data : &empty),
                           static_cast<ULONG>(dataLength), 0) != 0) break;
        unsigned char digest[32];
        if (BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0) break;
        if (BCryptImportKeyPair(signAlgorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
                                const_cast<unsigned char*>(publicKeyBlob), publicKeyBlobLength, 0) != 0) break;
        valid = BCryptVerifySignature(key, nullptr, digest, sizeof(digest),
                                      const_cast<unsigned char*>(signature),
                                      static_cast<ULONG>(signatureLength), 0) == 0;
    } while (false);
    if (key) BCryptDestroyKey(key);
    if (hash) BCryptDestroyHash(hash);
    if (signAlgorithm) BCryptCloseAlgorithmProvider(signAlgorithm, 0);
    if (hashAlgorithm) BCryptCloseAlgorithmProvider(hashAlgorithm, 0);
    return valid;
}

bool verifySignature(const unsigned char* data, size_t dataLength,
                     const unsigned char* signature, size_t signatureLength) {
    return verifySignatureWithKey(data, dataLength, signature, signatureLength,
                                  updateKeyBlob, sizeof(updateKeyBlob));
}

// ---------- install ----------

bool isInstallableVersion(const std::wstring& embeddedVersion, const std::wstring& offeredVersion,
                          const std::wstring& currentVersion) {
    return sameVersion(embeddedVersion, offeredVersion) && newerVersion(embeddedVersion, currentVersion);
}

static bool randomSuffix(std::wstring& out) {
    unsigned char bytes[16] = {};
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return false;
    static constexpr wchar_t hex[] = L"0123456789abcdef";
    out.clear();
    out.reserve(sizeof(bytes) * 2);
    for (unsigned char byte : bytes) {
        out += hex[byte >> 4];
        out += hex[byte & 0x0F];
    }
    return true;
}

static bool createRandomDirectory(const std::wstring& parent, std::wstring& out) {
    if (parent.empty()) return false;
    for (int attempt = 0; attempt < 8; attempt++) {
        std::wstring suffix;
        if (!randomSuffix(suffix)) return false;
        out = parent;
        if (out.back() != L'\\' && out.back() != L'/') out += L'\\';
        out += L".typethru-update-" + suffix;
        if (CreateDirectoryW(out.c_str(), nullptr)) return true;
        DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) return false;
    }
    return false;
}

static std::wstring parentDirectory(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    if (slash == 2 && path.size() >= 3 && path[1] == L':') return path.substr(0, 3);
    return path.substr(0, slash);
}

static std::wstring quoteWindowsArgument(const std::wstring& argument) {
    std::wstring quoted = L"\"";
    size_t slashes = 0;
    for (wchar_t c : argument) {
        if (c == L'\\') { slashes++; continue; }
        if (c == L'\"') {
            quoted.append(slashes * 2 + 1, L'\\');
            quoted += c;
        } else {
            quoted.append(slashes, L'\\');
            quoted += c;
        }
        slashes = 0;
    }
    quoted.append(slashes * 2, L'\\');
    quoted += L'\"';
    return quoted;
}

static bool copyFileToUniqueSibling(const std::wstring& source, const std::wstring& target,
                                   std::wstring& copiedPath) {
    const std::wstring parent = parentDirectory(target);
    if (parent.empty()) return false;
    for (int attempt = 0; attempt < 8; attempt++) {
        std::wstring suffix;
        if (!randomSuffix(suffix)) return false;
        copiedPath = parent;
        if (copiedPath.back() != L'\\' && copiedPath.back() != L'/') copiedPath += L'\\';
        copiedPath += L".typethru-replacement-" + suffix + L".exe";
        if (CopyFileW(source.c_str(), copiedPath.c_str(), TRUE)) return true;
        DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) return false;
    }
    return false;
}

static bool writeAllBytes(const std::wstring& path, const unsigned char* data, size_t length) {
    if ((!data && length) || length > (64U << 20)) return false;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    for (size_t done = 0; done < length && ok;) {
        DWORD chunk = 0;
        if (!WriteFile(file, data + done, static_cast<DWORD>(length - done), &chunk, nullptr) || chunk == 0) {
            ok = false;
        }
        done += chunk;
    }
    CloseHandle(file);
    return ok;
}

bool downloadToFile(const std::wstring& url, const std::wstring& path) {
    std::vector<unsigned char> bytes;
    if (!downloadToMemory(url, bytes, 64 << 20)) return false;
    return writeAllBytes(path, bytes.data(), bytes.size());
}

static bool base64Decode(const std::vector<unsigned char>& text, std::vector<unsigned char>& bytes) {
    bytes.clear();
    if (text.size() > 4096) return false;
    std::wstring clean;
    for (unsigned char c : text) {
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
              || c == '+' || c == '/' || c == '=')) return false;
        clean += static_cast<wchar_t>(c);
    }
    if (clean.size() != 88 || clean[86] != L'=' || clean[87] != L'=') return false;
    auto sextet = [](wchar_t c) -> int {
        if (c >= L'A' && c <= L'Z') return static_cast<int>(c - L'A');
        if (c >= L'a' && c <= L'z') return static_cast<int>(c - L'a' + 26);
        if (c >= L'0' && c <= L'9') return static_cast<int>(c - L'0' + 52);
        if (c == L'+') return 62;
        if (c == L'/') return 63;
        return -1;
    };
    for (size_t i = 0; i < 86; i++) if (sextet(clean[i]) < 0) return false;
    if ((sextet(clean[85]) & 0x0F) != 0) return false;
    DWORD size = 0;
    if (CryptStringToBinaryW(clean.c_str(), static_cast<DWORD>(clean.size()),
                             CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT, nullptr, &size, nullptr, nullptr)
        && size == 64) {
        bytes.resize(size);
        if (CryptStringToBinaryW(clean.c_str(), static_cast<DWORD>(clean.size()),
                                 CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT,
                                 bytes.data(), &size, nullptr, nullptr)) {
            if (size == 64) return true;
        }
    }
    bytes.clear();
    return false;
}

static bool readTypeThruVersion(const std::wstring& path, std::wstring& version) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES
        || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return false;
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeExW(FILE_VER_GET_NEUTRAL, path.c_str(), &handle);
    if (size == 0 || size > (1U << 20)) return false;
    std::vector<unsigned char> buffer(size);
    if (!GetFileVersionInfoExW(FILE_VER_GET_NEUTRAL, path.c_str(), handle, size, buffer.data())) return false;
    auto stringValue = [&buffer](const wchar_t* query, wchar_t*& value) {
        UINT length = 0;
        if (!VerQueryValueW(buffer.data(), query, reinterpret_cast<LPVOID*>(&value), &length)
            || !value || length < 2 || length > 256 || value[length - 1] != L'\0') return false;
        for (UINT i = 0; i + 1 < length; i++) if (value[i] == L'\0') return false;
        return true;
    };
    wchar_t* productName = nullptr;
    wchar_t* originalFilename = nullptr;
    wchar_t* productVersion = nullptr;
    wchar_t* fileVersion = nullptr;
    // 040904b0 is the block our .rc writes (US English, Unicode).
    if (!stringValue(L"\\StringFileInfo\\040904b0\\ProductName", productName)
        || !stringValue(L"\\StringFileInfo\\040904b0\\OriginalFilename", originalFilename)
        || !stringValue(L"\\StringFileInfo\\040904b0\\ProductVersion", productVersion)
        || !stringValue(L"\\StringFileInfo\\040904b0\\FileVersion", fileVersion)
        || wcscmp(productName, L"TypeThru") != 0 || wcscmp(originalFilename, L"TypeThru.exe") != 0
        || !sameVersion(productVersion, fileVersion)) return false;
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixedLength = 0;
    if (!VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<LPVOID*>(&fixed), &fixedLength)
        || !fixed || fixedLength < static_cast<UINT>(sizeof(VS_FIXEDFILEINFO))
        || fixed->dwSignature != VS_FFI_SIGNATURE
        || fixed->dwFileType != VFT_APP) return false;
    std::wstring fixedVersion = std::to_wstring(HIWORD(fixed->dwFileVersionMS)) + L"."
                              + std::to_wstring(LOWORD(fixed->dwFileVersionMS)) + L"."
                              + std::to_wstring(HIWORD(fixed->dwFileVersionLS)) + L"."
                              + std::to_wstring(LOWORD(fixed->dwFileVersionLS));
    if (!sameVersion(productVersion, fixedVersion)) return false;
    version = productVersion;
    return true;
}

static bool oldExecutableIsAvailable(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    CloseHandle(file);
    return true;
}

static bool launchTypeThru(const std::wstring& path) {
    std::wstring version;
    if (!readTypeThruVersion(path, version)) return false;
    std::wstring commandLine = quoteWindowsArgument(path);
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(path.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &startup, &process)) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

static int reportFinalizeFailure(int status, const std::wstring& oldPath,
                                 const std::wstring& details, const std::wstring& recoveryPath = {}) {
    const bool reopened = oldExecutableIsAvailable(oldPath) && launchTypeThru(oldPath);
    std::wstring message = L"TypeThru could not finish its update.\n\n" + details;
    if (!recoveryPath.empty()) {
        message += reopened
            ? L"\n\nTypeThru was reopened. The previous version backup is at:\n" + recoveryPath
            : L"\n\nThe previous version is preserved at:\n" + recoveryPath;
    } else if (reopened) {
        message += L"\n\nThe previous version was reopened.";
    }
    MessageBoxW(nullptr, message.c_str(), L"TypeThru Update", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    return status;
}

static bool rollbackReplacement(const std::wstring& oldPath, const std::wstring& backupPath,
                                const std::wstring& installedVersion, const std::wstring& previousVersion) {
    std::wstring currentVersion, savedVersion;
    if (!readTypeThruVersion(oldPath, currentVersion) || !sameVersion(currentVersion, installedVersion)
        || !readTypeThruVersion(backupPath, savedVersion) || !sameVersion(savedVersion, previousVersion)) return false;
    std::wstring failedPath = backupPath + L".failed";
    if (GetFileAttributesW(failedPath.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    if (!ReplaceFileW(oldPath.c_str(), backupPath.c_str(), failedPath.c_str(),
                      REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) return false;
    DeleteFileW(failedPath.c_str());
    return true;
}

int finalizeUpdate(const std::wstring& oldPath) {
    std::wstring selfPath;
    wchar_t self[MAX_PATH * 4] = {};
    DWORD selfLength = GetModuleFileNameW(nullptr, self, static_cast<DWORD>(ARRAYSIZE(self)));
    if (!selfLength || selfLength >= static_cast<DWORD>(ARRAYSIZE(self))) {
        return reportFinalizeFailure(1, oldPath, L"The new application path could not be read.");
    }
    selfPath.assign(self, selfLength);
    std::wstring selfVersion, oldVersion;
    if (!readTypeThruVersion(selfPath, selfVersion) || !readTypeThruVersion(oldPath, oldVersion)
        || !newerVersion(selfVersion, oldVersion)) {
        return reportFinalizeFailure(1, oldPath,
            L"The update is not a newer TypeThru application. The installed copy was left untouched.");
    }

    // The old copy is exiting; give it a moment, replace it, and start the new one.
    bool oldFileAvailable = false;
    for (int i = 0; i < 100; i++) {
        if (oldExecutableIsAvailable(oldPath)) { oldFileAvailable = true; break; }
        Sleep(100);
    }
    if (!oldFileAvailable) {
        return reportFinalizeFailure(2, oldPath,
            L"The installed application did not release its file. The update was not applied.");
    }

    // ReplaceFile is atomic, but the running candidate image may be locked. Copy
    // the already-validated bytes to a unique sibling on the target volume first.
    std::wstring replacement;
    if (!copyFileToUniqueSibling(selfPath, oldPath, replacement)) {
        return reportFinalizeFailure(3, oldPath,
            L"A replacement copy could not be prepared. The installed copy was left untouched.");
    }
    std::wstring backup = replacement + L".backup";
    if (!ReplaceFileW(oldPath.c_str(), replacement.c_str(), backup.c_str(),
                      REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
        DeleteFileW(replacement.c_str());
        return reportFinalizeFailure(4, oldPath,
            L"The installed file could not be replaced. The previous copy was left untouched.");
    }
    std::wstring replacedVersion;
    if (!readTypeThruVersion(oldPath, replacedVersion) || !sameVersion(replacedVersion, selfVersion)
        || !launchTypeThru(oldPath)) {
        if (rollbackReplacement(oldPath, backup, selfVersion, oldVersion)) {
            return reportFinalizeFailure(5, oldPath,
                L"The new copy could not start. The previous version was restored.");
        }
        return reportFinalizeFailure(5, oldPath,
            L"The new copy could not start, and automatic recovery failed.", backup);
    }
    return 0;
}

std::wstring install(const Release& release) {
    std::wstring tag;
    if (release.zipUrl.empty() || release.sigUrl.empty() || !isReleaseFile(release.zipUrl)
        || !releasePageTagForVersion(release.pageUrl, release.version, tag)
        || !releaseFileForTag(release.zipUrl, tag) || !releaseFileForTag(release.sigUrl, tag)
        || release.sigUrl != release.zipUrl + L".sig"
        || !isInstallableVersion(release.version, release.version, TYPETHRU_VERSION_STRING)) {
        return L"This release has no signed download.";
    }
    std::vector<unsigned char> signatureText, zip;
    if (!downloadToMemory(release.sigUrl, signatureText, 4096)) return L"Could not download the signature.";
    if (!downloadToMemory(release.zipUrl, zip, 64 << 20)) return L"Could not download the update.";
    std::vector<unsigned char> signature;
    if (!base64Decode(signatureText, signature)
        || !verifySignature(zip.data(), zip.size(), signature.data(), signature.size())) {
        return L"The download is not signed by TypeThru, so it was not installed.";
    }
    wchar_t currentPathBuffer[MAX_PATH * 4] = {};
    DWORD currentPathLength = GetModuleFileNameW(nullptr, currentPathBuffer,
                                                static_cast<DWORD>(ARRAYSIZE(currentPathBuffer)));
    if (!currentPathLength || currentPathLength >= static_cast<DWORD>(ARRAYSIZE(currentPathBuffer))) {
        return L"Could not locate the installed TypeThru application.";
    }
    std::wstring currentPath(currentPathBuffer, currentPathLength);
    std::wstring dir;
    if (!createRandomDirectory(parentDirectory(currentPath), dir)) {
        return L"Could not prepare a private update folder.";
    }
    std::wstring zipPath = dir + L"\\update.zip";
    if (!writeAllBytes(zipPath, zip.data(), zip.size())) return L"Could not save the update.";
    // DECISION: invoke the inbox extractor by absolute path so PATH and shell parsing cannot redirect it.
    wchar_t systemDirectory[MAX_PATH] = {};
    UINT systemLength = GetSystemDirectoryW(systemDirectory, static_cast<UINT>(ARRAYSIZE(systemDirectory)));
    if (!systemLength || systemLength >= static_cast<UINT>(ARRAYSIZE(systemDirectory))) return L"Could not locate the Windows archive tool.";
    std::wstring tarPath(systemDirectory, systemLength);
    tarPath += L"\\tar.exe";
    std::wstring commandLine = quoteWindowsArgument(tarPath) + L" -xf " + quoteWindowsArgument(zipPath)
                             + L" -C " + quoteWindowsArgument(dir);
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup = {sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(tarPath.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, dir.c_str(), &startup, &process)) {
        return L"Could not open the update package.";
    }
    if (WaitForSingleObject(process.hProcess, 30000) != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return L"Opening the update package took too long.";
    }
    DWORD tarExit = 1;
    GetExitCodeProcess(process.hProcess, &tarExit);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (tarExit != 0) return L"Could not open the update package.";
    std::wstring candidate;
    const auto candidateMatches = [&release](const std::wstring& path) {
        std::wstring version;
        if (!readTypeThruVersion(path, version)
            || !isInstallableVersion(version, release.version, TYPETHRU_VERSION_STRING)) return false;
        return true;
    };
    std::wstring rootCandidate = dir + L"\\TypeThru.exe";
    if (candidateMatches(rootCandidate)) {
        candidate = rootCandidate;
    } else {
        // Some zips carry a top-level folder.
        WIN32_FIND_DATAW found;
        HANDLE search = FindFirstFileW((dir + L"\\*").c_str(), &found);
        if (search != INVALID_HANDLE_VALUE) {
            do {
                if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    && !(found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    && wcscmp(found.cFileName, L".") != 0 && wcscmp(found.cFileName, L"..") != 0) {
                    std::wstring inner = dir + L"\\" + found.cFileName + L"\\TypeThru.exe";
                    if (candidateMatches(inner)) { candidate = inner; break; }
                }
            } while (FindNextFileW(search, &found));
            FindClose(search);
        }
    }
    if (candidate.empty()) {
        return L"The update package does not contain the offered TypeThru version.";
    }
    // DECISION: the command line carries the program path as argv[0]; a bare flag
    // would land in argv[0] and the new copy would start as the tray app instead.
    std::wstring arguments = quoteWindowsArgument(candidate) + L" --finalize-update "
                           + quoteWindowsArgument(currentPath);
    std::vector<wchar_t> mutableArguments(arguments.begin(), arguments.end());
    mutableArguments.push_back(L'\0');
    STARTUPINFOW fresh = {sizeof(fresh)};
    PROCESS_INFORMATION process2{};
    if (!CreateProcessW(candidate.c_str(), mutableArguments.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                        &fresh, &process2)) {
        return L"Could not start the new version.";
    }
    CloseHandle(process2.hThread);
    CloseHandle(process2.hProcess);
    return std::wstring();   // caller quits; the new copy takes over
}

// ---------- saved offer ----------

static const wchar_t* settingsPath = L"Software\\TypeThru";

static void savedString(const wchar_t* name, const std::wstring& value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, settingsPath, name, REG_SZ, value.c_str(),
                    static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

static std::wstring readSavedString(const wchar_t* name) {
    wchar_t value[512] = {};
    DWORD size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, settingsPath, name, RRF_RT_REG_SZ, nullptr, value, &size);
    return value;
}

void rememberOffer(const std::optional<Release>& release) {
    if (release) {
        savedString(L"AvailableVersion", release->version);
        savedString(L"AvailablePage", release->pageUrl);
        SYSTEMTIME time;
        GetSystemTime(&time);
        FILETIME fileTime;
        SystemTimeToFileTime(&time, &fileTime);
        ULARGE_INTEGER value{};
        value.LowPart = fileTime.dwLowDateTime;
        value.HighPart = fileTime.dwHighDateTime;
        RegSetKeyValueW(HKEY_CURRENT_USER, settingsPath, L"LastUpdateCheck", REG_QWORD,
                        &value.QuadPart, sizeof(value.QuadPart));
    } else {
        savedString(L"AvailableVersion", L"");
        savedString(L"AvailablePage", L"");
    }
}

std::optional<Release> savedOffer() {
    Release release;
    release.version = readSavedString(L"AvailableVersion");
    release.pageUrl = readSavedString(L"AvailablePage");
    if (release.version.empty() || !isReleasePage(release.pageUrl)) return std::nullopt;
    return release;
}

bool offerIsNewer(const Release& release, const std::wstring& currentVersion) {
    return newerVersion(release.version, currentVersion);
}

}
