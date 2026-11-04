// Regression tests for protocol boundaries, unit conversion, counter deltas and local collection.
// All runtime settings and credentials remain outside the repository.

#include "metrics.h"
#include <cmath>
#include <iostream>
#include <cstdlib>
/** Fail explicitly instead of using assert(), which can be compiled out in Release builds. */
static void check(bool ok, const char* message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
/** Exercise deterministic counter math, malformed protocol input and a real read-only Windows collector
 * sample. */
int main() {
    // Overflow the history and verify oldest-to-newest logical ordering.
    Series ring;
    for (int i = 0; i < 150; ++i)
        ring.push(i);
    check(ring.count == 120 && ring.at(0) == 30 && ring.at(119) == 149, "fixed ring preserves ordering");
    // Parse a complete wire frame and check KiB-to-byte conversion.
    RawSample s;
    check(parseSample("RV1 1000 600 8000 2000 2000 1000 100000 20000 1000 500 200.5 0.30 45.2", 10, s),
          "valid Linux protocol");
    check(s.memTotal == 8192000 && s.diskUsed == 20480000, "KiB to bytes");
    // One sample has values but cannot provide counter-derived rates.
    Metrics m;
    m.update(s);
    check(m.valid && !m.ratesValid && m.cpuHistory.count == 0, "first sample has no fabricated rates");
    // Use known deltas to verify 25% CPU and exact bytes/second.
    s.total += 200;
    s.idle += 150;
    s.rx += 4000;
    s.tx += 2000;
    s.time += 2;
    s.uptime += 2;
    m.update(s);
    check(m.ratesValid && std::abs(m.cpu - 25) < 0.001 && m.rxRate == 2000 && m.txRate == 1000,
          "CPU delta and network bytes/sec");
    check(m.ram == 75 && m.disk == 20, "RAM excludes available cache and disk uses occupied bytes");
    // A reconnect must not derive rates across a missing-data gap, even if host counters keep increasing.
    s.session = 1;
    s.total += 100;
    s.idle += 50;
    s.time += 1;
    s.uptime += 1;
    m.update(s);
    check(!m.ratesValid && m.cpuHistory.count == 0, "transport generation resets interval math");
    // Simulate reboot/rollback; no unsigned spike may enter history.
    s.total = 5;
    s.idle = 3;
    s.uptime = 1;
    s.rx = 2;
    s.tx = 1;
    s.time += 2;
    m.update(s);
    check(!m.ratesValid && m.rxRate == 0 && m.cpuHistory.count == 0, "reboot resets counters and history");
    // Reject negatives, inconsistent totals, truncated data and trailing payload.
    const char* bad[] = {"garbage",
                         "RV1 1 2 1 0 0 0 0 0 0 0 1 0 -1",
                         "RV1 -1 0 1 0 0 0 0 0 0 0 1 0 -1",
                         "RV1 1 0 0 0 0 0 0 0 0 0 1 0 -1",
                         "RV1 1 0 1 2 0 0 0 0 0 0 1 0 -1",
                         "RV1 1 0 1 0 0 0 0 0 0 0 nan 0 -1",
                         "RV1 1 0 1 0 0 0 0 0 0 0 1 0 -1 extra",
                         "RV1 1 0"};
    for (const auto* line : bad)
        check(!parseSample(line, 20, s), "reject malformed telemetry");
    // Read real Windows counters without subprocesses, networking or admin rights.
    LocalCollector local;
    check(local.sample(s) && s.memTotal > 0 && s.diskTotal > 0, "real Windows collector");
    std::cout << "All metrics tests passed.\n";
}
