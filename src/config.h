// Non-secret preferences and encoding helpers shared by the UI and SSH adapter.
#pragma once
#include <windows.h>
#include <string>

/** Settings are per Windows user. Passwords are deliberately absent from this structure. */
struct Config {
    // Reserved example domain: a fresh checkout never targets a private machine.
    std::wstring host = L"server.example.com", user = L"monitor", key;
    int port = 22;              // TCP port, validated in the range 1..65535.
    int interval = 2;           // Supported sampling intervals: 1, 2, 5 or 10 seconds.
    int opacity = 94;           // Percentage applied through the native layered-window alpha.
    int sizePercent = 100;      // Supported zoom levels: 80, 100 or 125 percent.
    int x = -32000, y = -32000; // Sentinel means no persisted screen position.
    bool local = true;          // Whether local-PC sampling and its extra panel are enabled.
    bool pinned = false;        // Native always-on-top state.

    /** Read preferences, validate values and preserve useful defaults for absent fields. */
    void load();
    /** Write preferences without serializing credentials or monitoring histories. */
    void save() const;
    /** Derive the Windows Credential Manager target for this SSH destination. */
    std::wstring credentialTarget() const;
};

/** Resolve/create %LOCALAPPDATA%\NodeSight and migrate the prototype INI if necessary. */
std::wstring configDirectory();
/** Return the absolute executable directory used to find the sibling askpass helper. */
std::wstring executableDirectory();
/** Decode SSH UTF-8 diagnostics for display in UTF-16 Win32 controls. */
std::wstring utf8ToWide(const std::string& text);
/** Encode UTF-16 as a UTF-8 payload without a terminator. */
std::string wideToUtf8(const std::wstring& text);
/** Check destination characters, port bounds and key-path control characters. */
bool validConnection(const Config& config);
