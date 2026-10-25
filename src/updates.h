// Asks GitHub for the newest TypeThru-Windows release, and installs a download only
// when it is signed with the key in updatekey.h. The Windows counterpart of Updates.swift.
#pragma once
#include <string>
#include <optional>
#include <vector>

namespace tt {

struct Release {
    std::wstring version;
    std::wstring pageUrl;   // the release page shown to the user
    std::wstring zipUrl;    // TypeThru-<version>-win64.zip on this repo's release
    std::wstring sigUrl;    // its .sig sidecar
};

enum class UpdateOutcome { UpToDate, Available, NetworkError };

namespace updater {

// The repo this app updates from; the same strings gate every URL below.
extern const wchar_t* repoOwner;
extern const wchar_t* repoName;

bool isReleasePage(const std::wstring& url);
bool isReleaseFile(const std::wstring& url);

// Newest non-draft release in a GitHub /releases API response (JSON), or nullopt.
// Only pages and files of this repo are accepted, so links cannot point elsewhere.
std::optional<Release> newestFromJson(const std::string& json);

// Blocking network check; nullopt Release with Available means "found", UpToDate none.
UpdateOutcome checkForNewer(const std::wstring& currentVersion, Release& found);

// Downloads to memory (bounded) or to a file. False on any transport failure.
bool downloadToMemory(const std::wstring& url, std::vector<unsigned char>& out, size_t limit);
bool downloadToFile(const std::wstring& url, const std::wstring& path);

// ECDSA P-256 (SHA-256) over the data with the embedded public key blob.
bool verifySignature(const unsigned char* data, size_t dataLength,
                     const unsigned char* signature, size_t signatureLength);
// Same check with an explicit key blob, so tests can round-trip with their own pair.
bool verifySignatureWithKey(const unsigned char* data, size_t dataLength,
                            const unsigned char* signature, size_t signatureLength,
                            const unsigned char* publicKeyBlob, unsigned long publicKeyBlobLength);

// Verifies and installs `release`: extract the zip with tar.exe, then hand over to
// the new copy through --finalize-update. Returns an error message, or empty on success.
std::wstring install(const Release& release);

// Runs from the NEW copy: waits for the old exe to exit, replaces it, starts it.
int finalizeUpdate(const std::wstring& oldPath);

// The saved offer from a previous check, so the panel can show it without the network.
void rememberOffer(const std::optional<Release>& release);
std::optional<Release> savedOffer();
bool offerIsNewer(const Release& release, const std::wstring& currentVersion);
// Strict numeric resource-version match plus a downgrade check, shared by install and tests.
bool isInstallableVersion(const std::wstring& embeddedVersion, const std::wstring& offeredVersion,
                          const std::wstring& currentVersion);

}

}
