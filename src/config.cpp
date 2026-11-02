// Per-user preference persistence, conservative validation and text encoding helpers.
// All runtime settings and credentials remain outside the repository.

#include "config.h"
#include <shlobj.h>
#include <wincred.h>
#include <algorithm>
#include <cwctype>

/** Resolve the current executable's directory; sibling helpers must not depend on the process working
 * directory. */
std::wstring executableDirectory() {
    wchar_t p[32768]{};
    GetModuleFileNameW(nullptr, p, 32768);
    std::wstring s = p;
    return s.substr(0, s.find_last_of(L"\\/"));
}
/** Create the per-user settings directory and migrate only the old prototype's INI on first use. */
std::wstring configDirectory() {
    wchar_t p[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, nullptr, 0, p)))
        return executableDirectory();
    std::wstring d = std::wstring(p) + L"\\NodeSight";
    CreateDirectoryW(d.c_str(), nullptr);
    // Preserve settings from the private prototype without embedding them in source.
    const auto legacy = std::wstring(p) + L"\\ResourceViewer\\settings.ini";
    const auto current = d + L"\\settings.ini";
    if (GetFileAttributesW(current.c_str()) == INVALID_FILE_ATTRIBUTES)
        CopyFileW(legacy.c_str(), current.c_str(), TRUE);
    return d;
}
/** Decode bounded SSH diagnostic text from UTF-8 into the UTF-16 representation used by Win32 controls. */
std::wstring utf8ToWide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring o(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), o.data(), n);
    return o;
}
/** Encode a UTF-16 string as UTF-8 without adding a NUL byte to the returned payload. */
std::string wideToUtf8(const std::wstring& s) {
    int n =
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string o(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), o.data(), n, nullptr, nullptr);
    return o;
}
/** Load non-secret preferences with conservative limits; never discover or embed a particular user's key
 * path. */
void Config::load() {
    auto file = configDirectory() + L"\\settings.ini";
    auto read = [&](const wchar_t* k, const std::wstring& fallback) {
        wchar_t b[4096]{};
        GetPrivateProfileStringW(L"widget", k, fallback.c_str(), b, 4096, file.c_str());
        return std::wstring(b);
    };
    host = read(L"host", host);
    user = read(L"user", user);
    key = read(L"key", key);
    auto num = [&](const wchar_t* k, int d) {
        return static_cast<int>(GetPrivateProfileIntW(L"widget", k, d, file.c_str()));
    };
    port = std::clamp(num(L"port", 22), 1, 65535);
    interval = num(L"interval", 2);
    if (interval != 1 && interval != 2 && interval != 5 && interval != 10)
        interval = 2;
    opacity = std::clamp(num(L"opacity", 94), 45, 100);
    local = num(L"local", 1) != 0;
    pinned = num(L"pinned", 0) != 0;
    sizePercent = num(L"size", 100);
    if (sizePercent != 80 && sizePercent != 100 && sizePercent != 125)
        sizePercent = 100;
    x = num(L"x", -32000);
    y = num(L"y", -32000);
    if (!validConnection(*this)) {
        host = L"server.example.com";
        user = L"monitor";
        port = 22;
        key.clear();
    }
    // Preserve a legacy saved password once, without exporting or logging its bytes.
    // Normal new installations have no legacy entry and skip this entire migration.
    PCREDENTIALW current = nullptr;
    if (CredReadW(credentialTarget().c_str(), CRED_TYPE_GENERIC, 0, &current)) {
        SecureZeroMemory(current->CredentialBlob, current->CredentialBlobSize);
        CredFree(current);
    } else {
        const auto legacyTarget = L"ResourceViewer/" + user + L"@" + host + L":" + std::to_wstring(port);
        PCREDENTIALW legacy = nullptr;
        if (CredReadW(legacyTarget.c_str(), CRED_TYPE_GENERIC, 0, &legacy)) {
            auto target = credentialTarget();
            legacy->TargetName = target.data();
            CredWriteW(legacy, 0);
            SecureZeroMemory(legacy->CredentialBlob, legacy->CredentialBlobSize);
            CredFree(legacy);
        }
    }
}
/** Persist only UI preferences and connection coordinates. Password storage is handled by Windows Credential
 * Manager. */
void Config::save() const {
    auto file = configDirectory() + L"\\settings.ini";
    auto write = [&](const wchar_t* k, const std::wstring& v) {
        WritePrivateProfileStringW(L"widget", k, v.c_str(), file.c_str());
    };
    write(L"host", host);
    write(L"user", user);
    write(L"key", key);
    write(L"port", std::to_wstring(port));
    write(L"interval", std::to_wstring(interval));
    write(L"opacity", std::to_wstring(opacity));
    write(L"size", std::to_wstring(sizePercent));
    write(L"local", local ? L"1" : L"0");
    write(L"pinned", pinned ? L"1" : L"0");
    write(L"x", std::to_wstring(x));
    write(L"y", std::to_wstring(y));
}
/** Namespace credentials by SSH user, host and port so unrelated server profiles cannot share a password. */
std::wstring Config::credentialTarget() const {
    return L"NodeSight/" + user + L"@" + host + L":" + std::to_wstring(port);
}
/** Reject empty/option-like destinations and control characters before assembling the SSH command line. */
bool validConnection(const Config& c) {
    if (c.host.empty() || c.user.empty() || c.host.size() > 253 || c.user.size() > 128 || c.port < 1 ||
        c.port > 65535)
        return false;
    if (c.host[0] == L'-' || c.user[0] == L'-')
        return false;
    for (wchar_t ch : c.host)
        if (!(iswalnum(ch) || ch == L'.' || ch == L'-' || ch == L':' || ch == L'_'))
            return false;
    for (wchar_t ch : c.user)
        if (!(iswalnum(ch) || ch == L'_' || ch == L'-' || ch == L'.'))
            return false;
    return c.key.find_first_of(L"\r\n\"") == std::wstring::npos;
}
