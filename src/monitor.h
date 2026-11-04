// Shared multi-host orchestration; both desktop frontends consume identical models.
#pragma once
#include "remote.h"
#include "recording.h"
#include <memory>

/** A UI snapshot contains aliases and rates; recording never needs destination identities. */
struct HostView {
    ServerProfile profile;
    Metrics metrics;
    bool connected = false, isLocal = false;
    std::wstring message;
    UsagePeaks peaks;
};
struct DashboardState {
    std::vector<HostView> hosts;
    double residentMiB = 0, cpuPercent = 0, peakResidentMiB = 0;
    unsigned activeServers = 0;
    bool recording = false;
    std::wstring logStatus, logPath;
};

/** One collector per enabled server; visibility does not change collection or recording. */
class MonitorHub {
  public:
    ~MonitorHub();
    /** Reconfigure monitoring. Call only after the settings draft validates successfully. */
    void configure(const Config& config);
    void stop();
    /** Drain sample queues and sample the local OS at the configured cadence. */
    void tick(const Config& config);
    DashboardState snapshot(const Config& config, bool includeHidden = false);
    void restartRecording(const Config& config);
    void stopRecording();
    bool recording() const {
        return recorder_.active();
    }

  private:
    struct Entry {
        ServerProfile profile;
        std::unique_ptr<RemoteCollector> collector;
        Metrics recorded;
        int intervalMs = 0;
    };
    std::vector<Entry> entries_;
    LocalCollector local_;
    Metrics localMetrics_;
    UsageRecorder recorder_;
    Config previousConfig_;
    bool configured_ = false;
    double lastLocal_ = -1, lastProcess_ = -1, lastCpu_ = 0, lastCpuTime_ = 0, rss_ = 0, cpu_ = 0,
           peakRss_ = 0;
};
/** Safe, deterministic UI fixture used by documentation and rendering regressions only. */
DashboardState demoDashboard(const Config& config);
