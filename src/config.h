// Portable preferences. Secrets never appear in this model or in the INI file.
#pragma once
#include <string>
#include <vector>

/** A server can stay monitored while hidden or collapsed in the widget. */
struct ServerProfile {
    std::wstring id = L"primary", name = L"Primary server";
    std::wstring host = L"server.example.com", user = L"monitor", key;
    int port = 22;
    bool enabled = true, visible = true, collapsed = false;
    /** Credential identity is destination-specific, independent of display aliases. */
    std::wstring credentialTarget() const;
};

/** Shared UI/collection settings; inherited connection fields mirror the selected profile. */
struct Config : ServerProfile {
    std::vector<ServerProfile> servers;
    int selected = 0, intervalMs = 2000, opacity = 94, sizePercent = 100;
    int x = -32000, y = -32000, detail = 1;
    bool local = true, pinned = false, graphs = true, localCollapsed = false;
    bool showCpu = true, showRam = true, showDisk = true, showNetwork = true, showSensors = true,
         showSwap = true;
    bool logEnabled = false;
    int logDurationMinutes = 60, logEveryMs = 1000, logMaxMiB = 10, logKeep = 5;
    int spikeCpu = 80, spikeRam = 90;
    std::wstring logDirectory;
    /** Load schema v2, migrating a single-server v1 profile without losing connection data. */
    std::wstring sourcePath; // Optional isolated config path for tests and portable instances.
    void load(const std::wstring& path = L"");
    /** Atomically replace only the non-secret settings file. */
    bool save(const std::wstring& path = L"") const;
    /** Mirror the selected profile into the compatibility connection fields. */
    void syncSelected();
    /** Produce an independent connection copy for one collector worker. */
    Config forServer(size_t index) const;
};

/** Resolve/create the per-user settings directory on Windows or XDG Linux. */
std::wstring configDirectory();
/** Resolve the executable's actual directory, not the working directory. */
std::wstring executableDirectory();
/** UTF-8 is used on disk; Win32 widgets and shared labels use wide strings. */
std::wstring utf8ToWide(const std::string& text);
std::string wideToUtf8(const std::wstring& text);
/** Validate destination characters before creating an argv/command line. */
bool validConnection(const ServerProfile& config);
/** Strict allowlist shared by settings, persistence and the Linux shell collector. */
bool validInterval(int milliseconds);
/** Locale-independent decimal seconds for GNU/BusyBox sleep. */
std::string intervalArgument(int milliseconds);
/** Opaque local profile identifier; it contains no destination information. */
std::wstring newProfileId();
