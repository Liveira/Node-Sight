// Deterministic settings/recording tests use isolated files and an injected monotonic clock.
#include "recording.h"
#include "dashboard.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>
namespace fs = std::filesystem;
void check(bool result, const char* message) {
    if (!result) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
std::string read(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}
int main() {
    auto root = fs::temp_directory_path() / fs::u8path(wideToUtf8(newProfileId()));
    fs::create_directories(root);
    Config c;
    c.load(utf8ToWide((root / "settings.ini").u8string()));
    check(c.servers.size() == 1 && c.intervalMs == 2000, "empty settings get neutral defaults");
    c.intervalMs = 100;
    c.servers.push_back(ServerProfile{});
    c.servers[1].id = L"hidden";
    c.servers[1].visible = false;
    c.servers[1].name = L"Hidden storage";
    c.logDirectory = utf8ToWide((root / "logs").u8string());
    c.detail = 2;
    c.graphs = false;
    check(c.save(), "atomic settings write");
    Config loaded;
    loaded.load(c.sourcePath);
    check(loaded.servers.size() == 2 && loaded.intervalMs == 100 && !loaded.servers[1].visible &&
              loaded.detail == 2 && !loaded.graphs,
          "v2 multi-host preferences round-trip");
    check(validInterval(100) && validInterval(500) && !validInterval(99) && intervalArgument(100) == "0.1",
          "subsecond interval allowlist");
    {
        std::ofstream old(root / "legacy.ini");
        old << "[widget]\nhost=server.example.com\nuser=monitor\ninterval=5\nlocal=0\n";
    }
    Config legacy;
    legacy.load(utf8ToWide((root / "legacy.ini").u8string()));
    check(legacy.servers.size() == 1 && legacy.intervalMs == 5000 && !legacy.local,
          "v1 migration preserves server and seconds");
    double now = 0;
    UsageRecorder recorder([&] { return now; });
    c.logDurationMinutes = 1;
    c.logEveryMs = 1000;
    c.logMaxMiB = 1;
    check(recorder.start(c), "start isolated log");
    Metrics m;
    RawSample s;
    s.memTotal = 1024;
    s.memAvailable = 512;
    s.total = 1000;
    s.idle = 900;
    s.time = 1;
    s.uptime = 100;
    m.update(s);
    recorder.observe(L"hidden", L"Hidden storage", m);
    now = 0.1;
    s.total += 1000;
    s.idle += 100;
    s.time += 0.1;
    s.uptime += 0.1;
    m.update(s);
    recorder.observe(L"hidden", L"Hidden storage", m);
    now = 1.1;
    s.total += 1000;
    s.idle += 900;
    s.time += 1;
    s.uptime += 1;
    m.update(s);
    recorder.observe(L"hidden", L"Hidden storage", m);
    check(recorder.peaks(L"hidden").cpu > 89, "subsecond CPU spike survives slower log summaries");
    now = 61;
    recorder.tick();
    check(!recorder.active(), "finite recording stops using monotonic deadline");
    auto log = read(root / "logs/node-sight.log");
    check(log.find("window_peak_cpu_pct=90.00") != std::string::npos &&
              log.find("reason=duration") != std::string::npos,
          "log has timed peaks and duration marker");
    check(log.find("server.example.com") == std::string::npos && log.find("monitor@") == std::string::npos,
          "recording excludes destination identities");
    c.logDurationMinutes = 0;
    check(recorder.start(c), "continuous session starts");
    now = 360000;
    recorder.tick();
    check(recorder.active(), "continuous recording has no artificial deadline");
    for (int i = 0; i < 20000; ++i)
        recorder.gap(L"synthetic", 1);
    recorder.stop();
    check(fs::exists(root / "logs/node-sight.1.log"), "bounded log rotates when its size limit is reached");
    check(fs::file_size(root / "logs/node-sight.log") <= 1024 * 1024, "active log remains size bounded");
    auto state = demoDashboard(c);
    auto layout = dashboardLayout(state, c);
    check(layout.hosts.size() == 3 && layout.height > 200, "shared layout contains concurrent demo nodes");
    // This is a newly generated directory owned only by this test, never a user-chosen path.
    fs::remove_all(root);
    std::cout << "Configuration, recording, rotation and layout tests passed.\n";
}
