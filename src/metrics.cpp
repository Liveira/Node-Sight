// Allocation-bounded histories, platform-neutral counter math and the Windows metrics adapter.
// All runtime settings and credentials remain outside the repository.

#include "metrics.h"
#include <chrono>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <locale>

/** Append one finite, nonnegative sample to fixed storage; overwrite the oldest point when the ring is full.
 */
void Series::push(double n) {
    values[next] = static_cast<float>(std::isfinite(n) ? std::max(0.0, n) : 0.0);
    next = (next + 1) % HISTORY;
    count = std::min(count + 1, HISTORY);
}
/** Read a logical oldest-to-newest index. Callers must keep i below count. */
float Series::at(size_t i) const {
    return values[(next + HISTORY - count + i) % HISTORY];
}
/** Find the peak among populated points only; unused ring slots do not affect automatic chart scaling. */
float Series::max() const {
    float m = 0;
    for (size_t i = 0; i < count; ++i)
        m = std::max(m, at(i));
    return m;
}
/** Use boot-relative monotonic time so wall-clock adjustments cannot distort network-rate denominators. */
double monotonicSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
/** Discard all counters, derived values and histories when a new sampling session begins. */
void Metrics::reset() {
    *this = Metrics{};
}
/** Convert cumulative counters to interval rates and bounded percentages, resetting history across
 * discontinuities. */
void Metrics::update(const RawSample& s) {
    // Reboots, counter rollback or duplicate timestamps invalidate interval math.
    bool nextRates = valid && s.session == raw.session && s.time > raw.time && s.uptime >= raw.uptime &&
                     s.total >= raw.total && s.idle >= raw.idle;
    previous = raw;
    raw = s;
    cpu = 0;
    rxRate = txRate = 0;
    if (nextRates) {
        // CPU counter units differ across platforms; the idle/total ratio is unitless.
        const auto dt = s.total - previous.total, di = s.idle - previous.idle;
        if (dt)
            cpu = 100.0 * (1.0 - static_cast<double>(std::min(di, dt)) / static_cast<double>(dt));
        // Resetting NIC counters yield zero rather than unsigned underflow.
        const double elapsed = s.time - previous.time;
        if (s.rx >= previous.rx)
            rxRate = static_cast<double>(s.rx - previous.rx) / elapsed;
        if (s.tx >= previous.tx)
            txRate = static_cast<double>(s.tx - previous.tx) / elapsed;
    }
    ram = s.memTotal ? 100.0 * static_cast<double>(s.memTotal - std::min(s.memAvailable, s.memTotal)) /
                           static_cast<double>(s.memTotal)
                     : 0;
    disk = s.diskTotal ? 100.0 * static_cast<double>(std::min(s.diskUsed, s.diskTotal)) /
                             static_cast<double>(s.diskTotal)
                       : 0;
    peakCpu = std::max(peakCpu, cpu);
    peakRam = std::max(peakRam, ram);
    peakRx = std::max(peakRx, rxRate);
    peakTx = std::max(peakTx, txRate);
    ratesValid = nextRates;
    valid = true;
    // A reconnect/reboot starts a new timeline; no fabricated zero spikes.
    if (!nextRates) {
        cpuHistory.clear();
        rxHistory.clear();
        txHistory.clear();
        ramHistory.clear();
        diskHistory.clear();
    }
    if (nextRates) {
        cpuHistory.push(cpu);
        rxHistory.push(rxRate);
        txHistory.push(txRate);
    }
    ramHistory.push(ram);
    if (s.diskTotal)
        diskHistory.push(disk);
}
/** Validate a complete RV1 line into a temporary sample. Leave the caller's output unchanged on any parse
 * error. */
bool parseSample(const std::string& line, double now, RawSample& out) {
    if (line.size() > 1024 || line.rfind("RV1 ", 0) != 0)
        return false;
    // Parse into temporary state so a truncated frame cannot corrupt live counters.
    std::istringstream in(line);
    in.imbue(std::locale::classic());
    std::string magic, extra;
    RawSample s;
    // Parse unsigned counters through signed values so negative input is rejected.
    long long n[10]{};
    in >> magic;
    for (auto& v : n) {
        if (!(in >> v) || v < 0)
            return false;
    }
    if (!(in >> s.uptime >> s.load >> s.temperature) || (in >> extra))
        return false;
    if (!std::isfinite(s.uptime) || !std::isfinite(s.load) || !std::isfinite(s.temperature) || s.uptime < 0 ||
        s.load < 0)
        return false;
    if (n[1] > n[0] || n[2] == 0 || n[3] > n[2] || n[5] > n[4] || n[7] > n[6])
        return false;
    // Check multiplication bounds before converting KiB wire values to bytes.
    constexpr uint64_t maxKB = UINT64_MAX / 1024;
    for (int i = 2; i <= 7; ++i)
        if (static_cast<uint64_t>(n[i]) > maxKB)
            return false;
    s.total = n[0];
    s.idle = n[1];
    s.memTotal = n[2] * 1024ULL;
    s.memAvailable = n[3] * 1024ULL;
    s.swapTotal = n[4] * 1024ULL;
    s.swapFree = n[5] * 1024ULL;
    s.diskTotal = n[6] * 1024ULL;
    s.diskUsed = n[7] * 1024ULL;
    s.rx = n[8];
    s.tx = n[9];
    s.time = now;
    out = s;
    return true;
}
