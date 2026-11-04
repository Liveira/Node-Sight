// Shared counter model. All byte quantities use binary bytes; rates use bytes/second.
#pragma once
#ifdef _WIN32
#include <windows.h>
#endif
#include <array>
#include <cstdint>
#include <string>

// A fixed history bounds memory use regardless of how long the widget stays open.
constexpr size_t HISTORY = 120;

/** Fixed-capacity circular series with oldest-to-newest logical indexing. */
struct Series {
    std::array<float, HISTORY> values{}; // Physical storage, never resized during sampling.
    size_t count = 0;                    // Number of populated slots (at most HISTORY).
    size_t next = 0;                     // Physical slot overwritten by the next push.

    /** Append a finite nonnegative value, evicting the oldest sample if needed. */
    void push(double n);
    /** Read an existing logical point; precondition: i < count. */
    float at(size_t i) const;
    /** Return the maximum populated value, or zero when empty. */
    float max() const;
    /** Clear both storage and ring metadata between independent sessions. */
    void clear() {
        count = next = 0;
        values.fill(0);
    }
};

/** Cumulative platform counters normalized into a common byte/second model. */
struct RawSample {
    uint64_t session = 0;         // Client transport generation; never sent by or trusted from a remote host.
    uint64_t total = 0, idle = 0; // CPU time units must match each other, not across hosts.
    uint64_t memTotal = 0, memAvailable = 0; // Physical bytes; available includes reclaimable cache.
    bool swapKnown = true;                // Windows local collection does not expose Linux swap accounting.
    uint64_t swapTotal = 0, swapFree = 0; // Linux swap bytes; zero when not exposed locally.
    uint64_t diskTotal = 0, diskUsed = 0; // Root/system-volume capacity, not disk I/O throughput.
    uint64_t rx = 0, tx = 0; // Cumulative received/transmitted bytes since interface initialization.
    double uptime = 0;       // Host boot-relative seconds, used to detect remote reboots.
    double load = -1;        // Linux one-minute load average, not a CPU percentage.
    double temperature = -1; // Celsius; -1 means no readable temperature sensor.
    double time = 0;         // Client monotonic receive time for rate denominators.
};

/** Derived values and histories owned by exactly one collector/state lock. */
struct Metrics {
    RawSample raw{}, previous{};
    bool valid = false;                // At least one validated sample is available.
    bool ratesValid = false;           // Two continuous samples are available for deltas.
    double cpu = 0, ram = 0, disk = 0; // Percentage values in the 0..100 range.
    double peakCpu = 0, peakRam = 0, peakRx = 0, peakTx = 0; // Peaks across the current collector session.
    double rxRate = 0, txRate = 0;                           // Bytes/second, independent of display units.
    Series cpuHistory, ramHistory, diskHistory, rxHistory, txHistory;

    /** Calculate interval rates and append histories; reset across counter discontinuities. */
    void update(const RawSample& sample);
    /** Return the entire metrics object to its empty initial state. */
    void reset();
};

/** Validate an RV1 line, converting wire KiB fields to bytes only after bounds checks. */
bool parseSample(const std::string& line, double monotonicSeconds, RawSample& out);
/** Return a clock unaffected by wall-clock/time-zone corrections. */
double monotonicSeconds();

/** Read-only Win32 adapter. No persistent OS service or command shell is required. */
class LocalCollector {
  public:
    /** Populate a complete sample; false means the essential CPU/RAM reads failed. */
    bool sample(RawSample& out);
    /** Get the machine name for local display only. */
    std::wstring computerName() const;
    /** Locate the Windows installation volume, including its trailing backslash. */
    std::wstring drive() const;
};

/** Native process counters. CPU seconds are cumulative across all threads. */
struct ProcessCounters {
    uint64_t residentBytes = 0;
    double cpuSeconds = 0;
    unsigned logicalProcessors = 1;
};
/** Read the current process only; platform implementations never spawn commands. */
ProcessCounters processCounters();
