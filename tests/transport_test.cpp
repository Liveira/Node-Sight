// Linux process/queue integration without credentials, host-key changes or real network access.
#include "monitor.h"
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <iostream>
#include <cstdlib>
#include <csignal>
namespace fs = std::filesystem;
void require(bool result, const char* message) {
    if (!result) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
int main(int argc, char** argv) {
    require(argc == 2, "fixture directory supplied");
    signal(SIGPIPE, SIG_IGN);
    std::string path = std::string(argv[1]) + ":" + (getenv("PATH") ? getenv("PATH") : "");
    setenv("PATH", path.c_str(), 1);
    Config c;
    c.intervalMs = 100;
    c.local = false;
    c.logEnabled = true;
    c.logDurationMinutes = 0;
    c.logEveryMs = 1000;
    auto root = fs::temp_directory_path() / wideToUtf8(newProfileId());
    fs::create_directories(root);
    c.logDirectory = utf8ToWide(root.u8string());
    ServerProfile first, second;
    first.id = L"first";
    first.name = L"Visible fixture";
    second.id = L"second";
    second.name = L"Hidden fixture";
    second.visible = false;
    c.servers = {first, second};
    MonitorHub hub;
    hub.configure(c);
    bool ready = false;
    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        hub.tick(c);
        auto all = hub.snapshot(c, true);
        ready = all.hosts.size() == 2;
        for (const auto& host : all.hosts)
            ready = ready && host.metrics.ratesValid;
        if (ready)
            break;
    }
    require(ready, "two simultaneous transports deliver valid intervals");
    require(hub.snapshot(c).hosts.size() == 1, "hidden nodes do not occupy the widget");
    auto before = hub.snapshot(c, true).hosts[0].metrics.raw.time;
    c.servers[0].visible = false;
    hub.configure(c);
    require(hub.snapshot(c, true).hosts[0].metrics.raw.time >= before &&
                hub.snapshot(c, true).hosts[0].metrics.ratesValid,
            "hiding preserves the existing connection and counters");
    require(hub.snapshot(c).hosts.empty(), "all-hidden presentation is valid");
    // Let several subsecond frames arrive between UI ticks, then verify they all drain.
    RemoteCollector collector;
    collector.start(c.forServer(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(650));
    auto batch = collector.takeSamples();
    require(batch.samples.size() >= 3 && batch.dropped == 0, "bounded handoff preserves subsecond samples");
    collector.stop();
    hub.tick(c);
    hub.stop();
    std::ifstream in(root / "node-sight.log");
    std::string log((std::istreambuf_iterator<char>(in)), {});
    require(log.find("node=\"first\"") != std::string::npos &&
                log.find("node=\"second\"") != std::string::npos,
            "visible and hidden profiles both reach recording");
    fs::remove_all(root);
    std::cout << "Concurrent transport, visibility preservation and recording handoff passed.\n";
}
