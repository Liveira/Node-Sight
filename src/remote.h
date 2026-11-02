// A single persistent SSH subprocess feeds bounded, mutex-protected metric snapshots.
#pragma once
#include "config.h"
#include "metrics.h"
#include <mutex>
#include <thread>

/** Copyable UI-facing state; transport handles never escape the collector. */
struct RemoteState {
    Metrics metrics; // Includes the last valid values, even after disconnection.
    std::wstring message = L"Conectando via SSH...";
    bool connected = false;  // Freshness is checked separately by the UI.
    double lastSample = 0;   // Client monotonic timestamp of the latest valid RV1 line.
    unsigned reconnects = 0; // Number of completed/failed transport attempts.
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

  private:
    /** Perform connection attempts, pipe parsing and interruptible retry delays. */
    void run(Config config);
    std::mutex mutex_; // Protects state_ and publication/removal of job_.
    RemoteState state_;
    HANDLE stopEvent_ = nullptr; // Manual-reset cancellation signal.
    HANDLE job_ = nullptr;       // Borrowed published handle; the worker closes it.
    std::thread thread_;         // Always joined before this object is destroyed.
};
