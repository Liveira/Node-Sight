// Portable INI persistence with v1 migration, strict bounds and atomic replacement.
#include "config.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <codecvt>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <locale>
#include <map>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
namespace {
using Ini = std::map<std::string, std::string>;
/** Trim configuration syntax only; embedded spaces in aliases and paths remain intact. */
std::string trim(std::string s) {
    auto a = s.find_first_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
/** Read a bounded UTF-8 INI, including legacy UTF-16LE files written by Win32. */
Ini readIni(const fs::path& path) {
    std::error_code sizeError;
    auto size = fs::file_size(path, sizeError);
    if (sizeError || size > 1024 * 1024)
        return {};
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    std::string data((std::istreambuf_iterator<char>(file)), {});
    if (data.size() > 1024 * 1024)
        return {};
    if (data.size() > 1 && static_cast<unsigned char>(data[0]) == 255 &&
        static_cast<unsigned char>(data[1]) == 254) {
        std::u16string u;
        for (size_t i = 2; i + 1 < data.size(); i += 2)
            u += static_cast<char16_t>(static_cast<unsigned char>(data[i]) |
                                       (static_cast<unsigned char>(data[i + 1]) << 8));
#ifdef _WIN32
        data = wideToUtf8(std::wstring(u.begin(), u.end()));
#else
        data = std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.to_bytes(u);
#endif
    }
    if (data.rfind("\xef\xbb\xbf", 0) == 0)
        data.erase(0, 3);
    Ini result;
    std::istringstream stream(data);
    std::string line, section;
    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        auto equal = line.find('=');
        if (equal == std::string::npos)
            continue;
        result[section + "." + trim(line.substr(0, equal))] = trim(line.substr(equal + 1));
    }
    return result;
}
/** Ensure labels cannot inject new settings sections or log records. */
std::wstring cleanLabel(std::wstring value) {
    value.erase(std::remove_if(value.begin(), value.end(),
                               [](wchar_t c) { return c < 32 || c == L'[' || c == L']'; }),
                value.end());
    if (value.size() > 80)
        value.resize(80);
    return value;
}
} // namespace

/** Decode persisted UTF-8 into the frontend wide-string representation; invalid input fails safely. */
std::wstring utf8ToWide(const std::string& text) {
    try {
#ifdef _WIN32
        int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                       static_cast<int>(text.size()), nullptr, 0);
        std::wstring result(size, 0);
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
        return result;
#else
        return std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.from_bytes(text);
#endif
    } catch (...) {
        return L"Invalid UTF-8";
    }
}
/** Encode shared labels for portable files and GTK, independently of the active locale. */
std::string wideToUtf8(const std::wstring& text) {
    try {
#ifdef _WIN32
        int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                       nullptr, nullptr);
        std::string result(size, 0);
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size,
                            nullptr, nullptr);
        return result;
#else
        return std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.to_bytes(text);
#endif
    } catch (...) {
        return "Invalid text";
    }
}
/** Resolve the executable itself so helper lookup does not depend on the launch directory. */
std::wstring executableDirectory() {
#ifdef _WIN32
    wchar_t path[32768]{};
    GetModuleFileNameW(nullptr, path, 32768);
    return fs::path(path).parent_path().wstring();
#else
    char path[4096]{};
    auto n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    return n > 0 ? utf8ToWide(fs::path(std::string(path, static_cast<size_t>(n))).parent_path().string())
                 : L".";
#endif
}
/** Use per-user platform storage and preserve the private prototype settings when first upgrading. */
std::wstring configDirectory() {
    fs::path path;
#ifdef _WIN32
    wchar_t local[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, nullptr, 0, local)))
        return executableDirectory();
    path = fs::path(local) / L"NodeSight";
#else
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    path = (xdg && *xdg ? fs::path(xdg) : (home ? fs::path(home) / ".config" : fs::temp_directory_path())) /
           "node-sight";
#endif
    std::error_code ec;
    fs::create_directories(path, ec);
#ifdef _WIN32
    const auto old = fs::path(local) / L"ResourceViewer" / L"settings.ini";
    if (!fs::exists(path / L"settings.ini"))
        fs::copy_file(old, path / L"settings.ini", fs::copy_options::none, ec);
    return path.wstring();
#else
    chmod(path.c_str(), 0700);
    return utf8ToWide(path.string());
#endif
}
/** Share one strict interval allowlist across persistence, UIs and the remote shell. */
bool validInterval(int ms) {
    return ms == 100 || ms == 250 || ms == 500 || ms == 1000 || ms == 2000 || ms == 5000 || ms == 10000;
}
/** Format fractional sleep seconds with a dot regardless of the desktop locale. */
std::string intervalArgument(int ms) {
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << static_cast<double>(ms) / 1000.0;
    return s.str();
}
/** Generate a destination-independent identifier for local profile and log correlation. */
std::wstring newProfileId() {
    static std::atomic<unsigned> serial{0};
    return L"node-" + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) + L"-" +
           std::to_wstring(++serial);
}
/** Scope saved Windows credentials by SSH destination, not by a changeable friendly alias. */
std::wstring ServerProfile::credentialTarget() const {
    return L"NodeSight/" + user + L"@" + host + L":" + std::to_wstring(port);
}
/** Reject option-like names, control characters and oversized values before spawning SSH. */
bool validConnection(const ServerProfile& c) {
    if (c.host.empty() || c.user.empty() || c.host.size() > 253 || c.user.size() > 128 || c.port < 1 ||
        c.port > 65535 || c.key.size() > 4096 || c.host[0] == L'-' || c.user[0] == L'-')
        return false;
    for (wchar_t ch : c.host)
        if (!(iswalnum(ch) || ch == L'.' || ch == L'-' || ch == L':' || ch == L'_'))
            return false;
    for (wchar_t ch : c.user)
        if (!(iswalnum(ch) || ch == L'_' || ch == L'-' || ch == L'.'))
            return false;
    return c.key.find_first_of(L"\r\n\"") == std::wstring::npos;
}
/** Mirror a valid selected profile for connection helpers while preserving the full fleet. */
void Config::syncSelected() {
    if (servers.empty())
        servers.push_back(ServerProfile{});
    selected = std::clamp(selected, 0, static_cast<int>(servers.size()) - 1);
    static_cast<ServerProfile&>(*this) = servers[static_cast<size_t>(selected)];
}
/** Give one worker an owned connection copy without retaining the whole profile vector. */
Config Config::forServer(size_t index) const {
    Config result = *this;
    if (index < servers.size())
        static_cast<ServerProfile&>(result) = servers[index];
    result.servers.clear();
    return result;
}
/** Read bounded preferences, apply safe ranges and migrate v1 seconds/single-host settings. */
void Config::load(const std::wstring& overridePath) {
    sourcePath = overridePath;
    const auto data = readIni(sourcePath.empty() ? fs::u8path(wideToUtf8(configDirectory())) / "settings.ini"
                                                 : fs::u8path(wideToUtf8(sourcePath)));
    auto str = [&](std::string k, std::wstring d) {
        auto i = data.find(k);
        return i == data.end() ? d : utf8ToWide(i->second);
    };
    auto num = [&](std::string k, int d) {
        auto i = data.find(k);
        if (i == data.end())
            return d;
        try {
            return std::stoi(i->second);
        } catch (...) {
            return d;
        }
    };
    intervalMs = num("widget.interval_ms", num("widget.interval", 2) * 1000);
    if (!validInterval(intervalMs))
        intervalMs = 2000;
    opacity = std::clamp(num("widget.opacity", 94), 45, 100);
    sizePercent = std::clamp(num("widget.size", 100), 60, 150);
    local = num("widget.local", 1) != 0;
    localCollapsed = num("widget.local_collapsed", 0) != 0;
    pinned = num("widget.pinned", 0) != 0;
    x = num("widget.x", -32000);
    y = num("widget.y", -32000);
    detail = std::clamp(num("widget.detail", 1), 0, 2);
    graphs = num("widget.graphs", 1) != 0;
    showCpu = num("widget.cpu", 1) != 0;
    showRam = num("widget.ram", 1) != 0;
    showDisk = num("widget.disk", 1) != 0;
    showNetwork = num("widget.network", 1) != 0;
    showSensors = num("widget.sensors", 1) != 0;
    showSwap = num("widget.swap", 1) != 0;
    logEnabled = num("recording.enabled", 0) != 0;
    logDurationMinutes = std::clamp(num("recording.duration_minutes", 60), 0, 525600);
    logEveryMs = std::clamp(num("recording.every_ms", 1000), 100, 60000);
    logMaxMiB = std::clamp(num("recording.max_mib", 10), 1, 1024);
    logKeep = std::clamp(num("recording.keep", 5), 1, 20);
    spikeCpu = std::clamp(num("recording.cpu_threshold", 80), 1, 100);
    spikeRam = std::clamp(num("recording.ram_threshold", 90), 1, 100);
    logDirectory = str("recording.directory", configDirectory() + L"/logs");
    servers.clear();
    int count = std::clamp(num("widget.server_count", 0), 0, 32);
    for (int i = 0; i < std::max(1, count); ++i) {
        const auto section = count ? "server" + std::to_string(i) + "." : "widget.";
        ServerProfile p;
        p.id = str(section + "id", i ? newProfileId() : L"primary");
        p.name = cleanLabel(str(section + "name", L"Server " + std::to_wstring(i + 1)));
        p.host = str(section + "host", p.host);
        p.user = str(section + "user", p.user);
        p.key = str(section + "key", p.key);
        p.port = num(section + "port", 22);
        p.enabled = num(section + "enabled", 1) != 0;
        p.visible = num(section + "visible", 1) != 0;
        p.collapsed = num(section + "collapsed", 0) != 0;
        if (!validConnection(p)) {
            p.host = L"server.example.com";
            p.user = L"monitor";
            p.port = 22;
            p.key.clear();
        }
        bool duplicate = false;
        for (const auto& other : servers)
            if (other.id == p.id)
                duplicate = true;
        if (duplicate || p.id.empty() || p.id.size() > 128 ||
            p.id.find_first_of(L"\r\n[]") != std::wstring::npos)
            p.id = newProfileId();
        servers.push_back(p);
    }
    selected = num("widget.selected", 0);
    syncSelected();
}
/** Write a temporary UTF-8 file and replace the old settings atomically; secrets are never serialized. */
bool Config::save(const std::wstring& overridePath) const {
    const auto chosen = overridePath.empty() ? sourcePath : overridePath;
    const auto path = chosen.empty() ? fs::u8path(wideToUtf8(configDirectory())) / "settings.ini"
                                     : fs::u8path(wideToUtf8(chosen));
    const auto temporary = path.parent_path() / (path.filename().u8string() + ".tmp");
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    auto w = [&](const char* key, const auto& value) { out << key << '=' << value << '\n'; };
    out << "; Node-Sight v2: no passwords or private-key contents are stored here.\n[widget]\n";
    w("server_count", servers.size());
    w("selected", selected);
    w("interval_ms", intervalMs);
    w("opacity", opacity);
    w("size", sizePercent);
    w("local", local);
    w("local_collapsed", localCollapsed);
    w("pinned", pinned);
    w("x", x);
    w("y", y);
    w("detail", detail);
    w("graphs", graphs);
    w("cpu", showCpu);
    w("ram", showRam);
    w("disk", showDisk);
    w("network", showNetwork);
    w("sensors", showSensors);
    w("swap", showSwap);
    out << "\n[recording]\n";
    w("enabled", logEnabled);
    w("duration_minutes", logDurationMinutes);
    w("every_ms", logEveryMs);
    w("max_mib", logMaxMiB);
    w("keep", logKeep);
    w("cpu_threshold", spikeCpu);
    w("ram_threshold", spikeRam);
    w("directory", wideToUtf8(logDirectory));
    for (size_t i = 0; i < servers.size(); ++i) {
        const auto& p = servers[i];
        out << "\n[server" << i << "]\n";
        w("id", wideToUtf8(p.id));
        w("name", wideToUtf8(cleanLabel(p.name)));
        w("host", wideToUtf8(p.host));
        w("user", wideToUtf8(p.user));
        w("key", wideToUtf8(p.key));
        w("port", p.port);
        w("enabled", p.enabled);
        w("visible", p.visible);
        w("collapsed", p.collapsed);
    }
    out.flush();
    if (!out)
        return false;
    out.close();
#ifdef _WIN32
    return MoveFileExW(temporary.wstring().c_str(), path.wstring().c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
    chmod(temporary.c_str(), 0600);
    std::error_code ec;
    fs::rename(temporary, path, ec);
    return !ec;
#endif
}
