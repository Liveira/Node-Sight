// A single persistent SSH subprocess feeds bounded, mutex-protected metric snapshots.
#pragma once
#include "config.h"
#include "metrics.h"
#include <mutex>
#include <thread>
#include <deque>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <atomic>
#include <condition_variable>
#endif

/** Copyable UI-facing state; transport handles never escape the collector. */
struct RemoteState {
    Metrics metrics; // Includes the last valid values, even after disconnection.
    std::wstring message = L"Connecting via SSH...";
    bool connected = false;  // Freshness is checked separately by the UI.
    double lastSample = 0;   // Client monotonic timestamp of the latest valid RV1 line.
    unsigned reconnects = 0; // Number of completed/failed transport attempts.
};

/** Bounded handoff preserves every received subsecond sample between UI ticks. */
struct SampleBatch {
    std::vector<RawSample> samples;
    unsigned dropped = 0;
};

/** Owns one worker, one cancellation event and at most one SSH process job. */
class RemoteCollector {
  public:
    /** Allocate cancellation state; no connection is made until start(). */
    RemoteCollector();
    /** Stop the worker, close its job and release the cancellation handle. */
    ~RemoteCollector();
    /** Stop a prior session and launch a worker with a private configuration copy. */
    void start(const Config& config);
    /** Cancel waits, terminate only this job's processes and join the worker. */
    void stop();
    /** Copy all visible state under one mutex acquisition. */
    RemoteState snapshot();
    /** Drain the bounded recording handoff; overflow is reported, never silently hidden. */
    SampleBatch takeSamples();

  private:
    /** Perform connection attempts, pipe parsing and interruptible retry delays. */
    void run(Config config);
    std::mutex mutex_; // Protects state_ and publication/removal of job_.
    RemoteState state_;
    std::deque<RawSample> samples_;
    unsigned dropped_ = 0;
#ifdef _WIN32
    HANDLE stopEvent_ = nullptr;
    HANDLE job_ = nullptr;
#else
    std::atomic<bool> stopping_{true};
    std::atomic<int> child_{0};
    std::condition_variable wake_;
    std::mutex waitMutex_;
#endif
    std::thread thread_; // Always joined before this object is destroyed.
};
