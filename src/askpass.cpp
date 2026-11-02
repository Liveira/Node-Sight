// Minimal GUI-subsystem helper: OpenSSH captures its stdout without opening a console window.
// All runtime settings and credentials remain outside the repository.

#include <windows.h>
#include <wincred.h>
#include <string>
/** Serve one OpenSSH askpass request from a namespaced Windows credential; write only to the inherited pipe.
 */
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // The environment carries only a credential identifier, never the password itself.
    wchar_t target[512]{};
    if (!GetEnvironmentVariableW(L"RV_CREDENTIAL_TARGET", target, 512))
        return 1;
    PCREDENTIALW c = nullptr;
    if (!CredReadW(target, CRED_TYPE_GENERIC, 0, &c))
        return 1;
    // Convert stored UTF-16 bytes to the single UTF-8 line expected by askpass.
    const auto chars = static_cast<int>(c->CredentialBlobSize / sizeof(wchar_t));
    auto password = reinterpret_cast<const wchar_t*>(c->CredentialBlob);
    int size = WideCharToMultiByte(CP_UTF8, 0, password, chars, nullptr, 0, nullptr, nullptr);
    std::string output(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, password, chars, output.data(), size, nullptr, nullptr);
    output += '\n';
    DWORD written = 0;
    const bool ok = WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output.data(),
                              static_cast<DWORD>(output.size()), &written, nullptr) != FALSE;
    // Scrub reusable buffers before returning to the operating system.
    SecureZeroMemory(output.data(), output.size());
    SecureZeroMemory(c->CredentialBlob, c->CredentialBlobSize);
    CredFree(c);
    return ok ? 0 : 1;
}
