// Bounded local recording. Sampling and log emission frequencies are independent.
#pragma once
#include "config.h"
#include "metrics.h"
#include <fstream>
#include <map>
#include <functional>

/** Running peaks refer to samples received, not unobserved activity between samples. */
struct UsagePeaks {
    double cpu = 0, ram = 0, rx = 0, tx = 0;
};
class UsageRecorder {
  public:
    explicit UsageRecorder(std::function<double()> clock = monotonicSeconds) : clock_(std::move(clock)) {}
    ~UsageRecorder();
    /** Start a new session. Zero duration means continuous recording with size rotation. */
    bool start(const Config& config);
    /** Flush a final summary and end the session without affecting monitoring. */
    void stop(const std::string& reason = "user");
    /** Observe every sample, retaining subsecond peaks even when summaries are slower. */
    void observe(const std::wstring& id, const std::wstring& alias, const Metrics& metrics);
    /** Emit transitions only, and check the recording deadline even when all hosts are offline. */
    void connection(const std::wstring& id, bool connected);
    void tick();
    /** Explicitly mark bounded queue overflow; affected peaks are incomplete. */
    void gap(const std::wstring& id, unsigned dropped);
    void removeHost(const std::wstring& id);
    bool active() const {
        return active_;
    }
    std::wstring status() const {
        return status_;
    }
    std::wstring path() const {
        return path_;
    }
    UsagePeaks peaks(const std::wstring& id) const;

  private:
    struct Host {
        UsagePeaks lifetime, window;
        Metrics latest;
        std::wstring alias;
        std::string cpuPeakAt, ramPeakAt, rxPeakAt, txPeakAt;
        double sampleTime = -1, lastWrite = 0, lastSpike = -100;
        bool hasSample = false, hasRates = false, knownConnection = false, connected = false;
    };
    void write(const std::string& event, const std::string& payload);
    void summary(const std::wstring& id, Host& host, const char* event);
    bool rotate();
    std::function<double()> clock_;
    Config config_;
    std::ofstream file_;
    std::map<std::wstring, Host> hosts_;
    std::wstring path_, status_ = L"Recording is off";
    bool active_ = false;
    double started_ = 0, lastFlush_ = 0;
    uint64_t bytes_ = 0;
};
/** ISO-8601 UTC, with milliseconds. No locale-specific timestamp parsing is required. */
std::string utcTimestamp(double offsetSeconds = 0);
