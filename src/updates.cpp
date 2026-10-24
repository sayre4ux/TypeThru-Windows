#include "updates.h"
#include "updatekey.h"
#include "shared.h"
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>

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
    double number = 0;
    std::string text;                          // for String
    std::vector<Json> items;                   // for Array
    std::map<std::string, Json> fields;        // for Object

    const Json* at(const char* name) const {
        auto it = fields.find(name);
        return it == fields.end() ? nullptr : &it->second;
    }
};

struct JsonParser {
    const char* p;
    const char* end;
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
    bool string(std::string& out) {
        if (p >= end || *p != '"') return false;
        p++;
        out.clear();
        while (p < end && *p != '"') {
            if (*p != '\\') { out += *p++; continue; }
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
                    if (end - p < 5) return false;
                    unsigned cp = 0;
                    for (int i = 1; i <= 4; i++) {
                        char c = p[i];
                        cp <<= 4;
                        if (c >= '0' && c <= '9') cp |= static_cast<unsigned>(c - '0');
                        else if (c >= 'a' && c <= 'f') cp |= static_cast<unsigned>(c - 'a' + 10);
                        else if (c >= 'A' && c <= 'F') cp |= static_cast<unsigned>(c - 'A' + 10);
                        else return false;
                    }
                    p += 5;
                    // Surrogate pair.
                    if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                        unsigned low = 0;
                        bool ok = true;
                        for (int i = 2; i <= 5; i++) {
                            char c = p[i];
                            low <<= 4;
                            if (c >= '0' && c <= '9') low |= static_cast<unsigned>(c - '0');
                            else if (c >= 'a' && c <= 'f') low |= static_cast<unsigned>(c - 'a' + 10);
                            else if (c >= 'A' && c <= 'F') low |= static_cast<unsigned>(c - 'A' + 10);
                            else { ok = false; break; }
                        }
                        if (ok && low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            p += 6;
                        }
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        if (p >= end) return false;
        p++;
        return true;
    }
    bool value(Json& out) {
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
                if (!value(child)) return false;
                out.fields[name] = child;
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
                if (!value(child)) return false;
                out.items.push_back(child);
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
        {
            char* stop = nullptr;
            out.number = strtod(p, &stop);
            if (stop == p) return false;
            out.type = Json::Type::Number;
            p = stop;
            return true;
        }
    }
};

static std::wstring utf8ToWide(const std::string& text) {
    int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

// ---------- URL gates ----------

// The URL must be exactly https://github.com/<owner>/<repo>/<rest> with the repo path
// as a real prefix (not a query or a deeper path) and no traversal.
static bool isOwnGithubPath(const std::wstring& url, const wchar_t* restPrefix) {
    const std::wstring scheme = L"https://github.com";
    if (url.compare(0, scheme.size(), scheme) != 0) return false;
    std::wstring prefix = std::wstring(L"/") + repoOwner + L"/" + repoName + restPrefix;
    if (url.compare(scheme.size(), prefix.size(), prefix) != 0) return false;
    return url.find(L"..") == std::wstring::npos;
}

bool isReleasePage(const std::wstring& url) {
    return isOwnGithubPath(url, L"/releases/");
}

bool isReleaseFile(const std::wstring& url) {
    return isOwnGithubPath(url, L"/releases/download/");
}

std::optional<Release> newestFromJson(const std::string& json) {
    JsonParser parser(json);
    Json root;
    if (!parser.value(root) || root.type != Json::Type::Array) return std::nullopt;
    std::optional<Release> best;
    for (const Json& item : root.items) {
        if (item.type != Json::Type::Object) continue;
        const Json* draft = item.at("draft");
        if (draft && draft->type == Json::Type::Bool && draft->boolean) continue;
        const Json* tag = item.at("tag_name");
        const Json* page = item.at("html_url");
        if (!tag || tag->type != Json::Type::String) continue;
        int numbers[4];
        if (versionNumbers(utf8ToWide(tag->text), numbers) == 0) continue;
        if (!page || page->type != Json::Type::String || !isReleasePage(utf8ToWide(page->text))) continue;
        Release release;
        // "v0.3.0-beta" is offered as "0.3.0", like the macOS app.
        int partCount = versionNumbers(utf8ToWide(tag->text), numbers);
        std::wstring normalized;
        for (int i = 0; i < partCount; i++) {
            if (i) normalized += L".";
            normalized += std::to_wstring(numbers[i]);
        }
        release.version = normalized;
        release.pageUrl = utf8ToWide(page->text);
        // The zip and its signature, attached as TypeThru-<version>-win64.zip and .zip.sig.
        std::map<std::string, std::wstring> assets;
        if (const Json* list = item.at("assets")) {
            for (const Json& asset : list->items) {
                const Json* name = asset.at("name");
                const Json* link = asset.at("browser_download_url");
                if (name && name->type == Json::Type::String && link && link->type == Json::Type::String
                    && isReleaseFile(utf8ToWide(link->text))) {
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
        if (!best || isNewerVersion(release.version, best->version)) best = release;
    }
    return best;
}

// ---------- transport ----------

struct HttpHandle {
    HINTERNET handle = nullptr;
    ~HttpHandle() { if (handle) WinHttpCloseHandle(handle); }
};

static bool httpGet(const std::wstring& url, const wchar_t* accept, size_t limit,
                    std::vector<unsigned char>& out) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {}, path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 255;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2047;
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) return false;

    HttpHandle session, connect, request;
    session.handle = WinHttpOpen(L"TypeThru", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.handle) return false;
    connect.handle = WinHttpConnect(session.handle, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect.handle) return false;
    LPCWSTR acceptTypes[2] = {accept, nullptr};
    request.handle = WinHttpOpenRequest(connect.handle, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                        accept ? acceptTypes : static_cast<LPCWSTR*>(WINHTTP_DEFAULT_ACCEPT_TYPES),
                                        WINHTTP_FLAG_SECURE);
    if (!request.handle) return false;
    if (accept) {
        WinHttpAddRequestHeaders(request.handle, (std::wstring(L"Accept: ") + accept).c_str(),
                                 static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);
    }
    if (!WinHttpSendRequest(request.handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                            0, 0, 0)
        || !WinHttpReceiveResponse(request.handle, nullptr)) {
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
        DWORD read = 0;
        if (!WinHttpReadData(request.handle, buffer, sizeof(buffer), &read)) return false;
        if (read == 0) break;
        if (out.size() + read > limit) return false;
        out.insert(out.end(), buffer, buffer + read);
    }
    return true;
}

bool downloadToMemory(const std::wstring& url, std::vector<unsigned char>& out, size_t limit) {
    return httpGet(url, nullptr, limit, out);
}

UpdateOutcome checkForNewer(const std::wstring& currentVersion, Release& found) {
    std::wstring feed = std::wstring(L"https://api.github.com/repos/") + repoOwner + L"/" + repoName
                      + L"/releases?per_page=10";
    std::vector<unsigned char> body;
    if (!httpGet(feed, L"application/vnd.github+json", 1 << 20, body)) return UpdateOutcome::NetworkError;
    std::optional<Release> newest = newestFromJson(std::string(reinterpret_cast<char*>(body.data()), body.size()));
    if (!newest || !isNewerVersion(newest->version, currentVersion)) return UpdateOutcome::UpToDate;
    found = *newest;
    return UpdateOutcome::Available;
}

// ---------- signature ----------

// ECDSA P-256 over the SHA-256 of `data`, raw r||s signature, with the given
// BCRYPT_ECCPUBLIC_BLOB. Public so tests can round-trip with their own key pair.
bool verifySignatureWithKey(const unsigned char* data, size_t dataLength,
                            const unsigned char* signature, size_t signatureLength,
                            const unsigned char* publicKeyBlob, ULONG publicKeyBlobLength) {
    bool valid = false;
    BCRYPT_ALG_HANDLE hashAlgorithm = nullptr, signAlgorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    do {
        if (BCryptOpenAlgorithmProvider(&hashAlgorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) break;
        if (BCryptOpenAlgorithmProvider(&signAlgorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0) break;
        if (BCryptCreateHash(hashAlgorithm, &hash, nullptr, 0, nullptr, 0, 0) != 0) break;
        if (BCryptHashData(hash, const_cast<unsigned char*>(data),
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

static std::wstring tempDirectory() {
    // Random name: a fixed path under %TEMP% could be pre-created or raced by
    // another process running as the same user.
    wchar_t base[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, base);
    wchar_t random[MAX_PATH] = {};
    GetTempFileNameW(base, L"TTU", 0, random);
    DeleteFileW(random);
    std::wstring dir = std::wstring(random) + L"-type";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

static bool writeAllBytes(const std::wstring& path, const unsigned char* data, size_t length) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
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

static std::wstring base64Decode(const std::vector<unsigned char>& text) {
    std::wstring clean;
    for (unsigned char c : text) {
        if (c != '\r' && c != '\n' && c != ' ' && c != '\t') clean += static_cast<wchar_t>(c);
    }
    DWORD size = 0;
    if (CryptStringToBinaryW(clean.c_str(), static_cast<DWORD>(clean.size()),
                             CRYPT_STRING_BASE64, nullptr, &size, nullptr, nullptr)) {
        std::wstring bytes(size, L'\0');
        if (CryptStringToBinaryW(clean.c_str(), static_cast<DWORD>(clean.size()),
                                 CRYPT_STRING_BASE64,
                                 reinterpret_cast<BYTE*>(bytes.data()), &size, nullptr, nullptr)) {
            bytes.resize(size);
            return bytes;
        }
    }
    return std::wstring();
}

// Whether the file at `path` is our own product, so --finalize-update can only
// replace TypeThru with TypeThru, not copy itself over an arbitrary file.
static bool isTypeThruExe(const std::wstring& path) {
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeExW(FILE_VER_GET_NEUTRAL, path.c_str(), &handle);
    if (size == 0) return false;
    std::vector<unsigned char> buffer(size);
    if (!GetFileVersionInfoExW(FILE_VER_GET_NEUTRAL, path.c_str(), handle, size, buffer.data())) return false;
    wchar_t* productName = nullptr;
    UINT length = 0;
    // 040904b0 is the block our .rc writes (US English, Unicode).
    return VerQueryValueW(buffer.data(), L"\\StringFileInfo\\040904b0\\ProductName",
                          reinterpret_cast<LPVOID*>(&productName), &length)
        && productName && wcscmp(productName, L"TypeThru") == 0;
}

int finalizeUpdate(const std::wstring& oldPath) {
    // The old copy is exiting; give it a moment, replace it, and start the new one.
    for (int i = 0; i < 100; i++) {
        HANDLE file = CreateFileW(oldPath.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            break;
        }
        Sleep(100);
    }
    if (!isTypeThruExe(oldPath)) return 1;   // refuse to replace anything but TypeThru
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (!CopyFileW(self, oldPath.c_str(), FALSE)) return 2;
    ShellExecuteW(nullptr, L"open", oldPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return 0;
}

std::wstring install(const Release& release) {
    if (release.zipUrl.empty() || release.sigUrl.empty() || !isReleaseFile(release.zipUrl)
        || !isReleaseFile(release.sigUrl)) {
        return L"This release has no signed download.";
    }
    std::vector<unsigned char> signatureText, zip;
    if (!downloadToMemory(release.sigUrl, signatureText, 4096)) return L"Could not download the signature.";
    if (!downloadToMemory(release.zipUrl, zip, 64 << 20)) return L"Could not download the update.";
    std::wstring signature = base64Decode(signatureText);
    if (signature.empty()
        || !verifySignature(zip.data(), zip.size(),
                            reinterpret_cast<const unsigned char*>(signature.data()), signature.size())) {
        return L"The download is not signed by TypeThru, so it was not installed.";
    }
    std::wstring dir = tempDirectory();
    std::wstring zipPath = dir + L"\\update.zip";
    if (!writeAllBytes(zipPath, zip.data(), zip.size())) return L"Could not save the update.";
    // DECISION: tar.exe ships with Windows 10 1803+ and reads zip without a dependency.
    wchar_t command[4200] = {};
    swprintf_s(command, L"/C tar.exe -xf \"%s\" -C \"%s\"", zipPath.c_str(), dir.c_str());
    STARTUPINFOW startup = {sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(L"C:\\Windows\\System32\\cmd.exe", command, nullptr, nullptr, FALSE,
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
    std::wstring candidate = dir + L"\\TypeThru.exe";
    if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // Some zips carry a top-level folder.
        WIN32_FIND_DATAW found;
        HANDLE search = FindFirstFileW((dir + L"\\*").c_str(), &found);
        if (search != INVALID_HANDLE_VALUE) {
            do {
                if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY
                    && wcscmp(found.cFileName, L".") != 0 && wcscmp(found.cFileName, L"..") != 0) {
                    std::wstring inner = dir + L"\\" + found.cFileName + L"\\TypeThru.exe";
                    if (GetFileAttributesW(inner.c_str()) != INVALID_FILE_ATTRIBUTES) { candidate = inner; break; }
                }
            } while (FindNextFileW(search, &found));
            FindClose(search);
        }
    }
    if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return L"The update package does not contain TypeThru.exe.";
    }
    wchar_t oldPath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, oldPath, MAX_PATH);
    // DECISION: the command line carries the program path as argv[0]; a bare flag
    // would land in argv[0] and the new copy would start as the tray app instead.
    wchar_t arguments[MAX_PATH * 2 + 64] = {};
    swprintf_s(arguments, L"\"%s\" --finalize-update \"%s\"", candidate.c_str(), oldPath);
    STARTUPINFOW fresh = {sizeof(fresh)};
    PROCESS_INFORMATION process2{};
    if (!CreateProcessW(candidate.c_str(), arguments, nullptr, nullptr, FALSE, 0, nullptr, nullptr,
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
    return isNewerVersion(release.version, currentVersion);
}

}
