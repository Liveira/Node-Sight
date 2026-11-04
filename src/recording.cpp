// All recording calls are owned by the UI/hub thread. Worker threads never write files.
#include "recording.h"
#include <filesystem>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <sstream>
#include <algorithm>
namespace fs = std::filesystem;
namespace {
/** Escape control characters and delimiters; aliases can never inject log lines. */
std::string safe(const std::wstring& value) {
    auto s = wideToUtf8(value);
    for (char& c : s)
        if (static_cast<unsigned char>(c) < 32 || c == '"' || c == '\\')
            c = '_';
    return s;
}
/** Use fixed locale-independent decimals so exported log fields remain machine-readable. */
std::string decimal(double value) {
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << std::fixed << std::setprecision(2) << value;
    return s.str();
}
} // namespace
/** Convert wall time plus an optional receipt-time offset into UTC with millisecond precision. */
std::string utcTimestamp(double offsetSeconds) {
    const auto now =
        std::chrono::system_clock::now() + std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                               std::chrono::duration<double>(offsetSeconds));
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << ms << 'Z';
    return out.str();
}
/** Flush a normal session end during orderly object destruction. */
UsageRecorder::~UsageRecorder() {
    stop("shutdown");
}
/** Make bounded handoff loss visible rather than silently presenting incomplete peaks. */
void UsageRecorder::gap(const std::wstring& id, unsigned dropped) {
    write("SAMPLING_GAP", "node=\"" + safe(id) + "\" dropped=" + std::to_string(dropped));
}
/** Emit the last summary and release state when a profile stops being monitored. */
void UsageRecorder::removeHost(const std::wstring& id) {
    auto i = hosts_.find(id);
    if (i != hosts_.end()) {
        if (i->second.hasSample)
            summary(id, i->second, "SUMMARY");
        hosts_.erase(i);
    }
}
/** Open one bounded append stream and use monotonic time for finite-session deadlines. */
bool UsageRecorder::start(const Config& config) {
    stop("restart");
    config_ = config;
    hosts_.clear();
    bytes_ = 0;
    auto dir = fs::u8path(
        wideToUtf8(config.logDirectory.empty() ? configDirectory() + L"/logs" : config.logDirectory));
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        status_ = L"Cannot create recording folder";
        return false;
    }
    auto file = dir / "node-sight.log";
    path_ = utf8ToWide(file.u8string());
    // Reuse one active path across restarts; rotation bounds the accumulated history.
    bytes_ = fs::exists(file, ec) ? fs::file_size(file, ec) : 0;
    if (ec) {
        status_ = L"Cannot inspect existing recording file";
        return false;
    }
    file_.open(file, std::ios::binary | std::ios::app);
    if (!file_) {
        status_ = L"Cannot open recording file";
        return false;
    }
    active_ = true;
    started_ = lastFlush_ = clock_();
    status_ =
        L"Recording · " +
        (config.logDurationMinutes ? std::to_wstring(config.logDurationMinutes) + L" min" : L"continuous");
    write("SESSION_START", "sample_interval_ms=" + std::to_string(config.intervalMs) +
                               " summary_interval_ms=" + std::to_string(config.logEveryMs) +
                               " duration_minutes=" + std::to_string(config.logDurationMinutes));
    return active_;
}
/** Rotate only this recorder's own fixed file names, from oldest index downward. */
bool UsageRecorder::rotate() {
    file_.flush();
    file_.close();
    auto base = fs::u8path(wideToUtf8(path_));
    std::error_code ec;
    auto rotated = [&](int i) {
        return base.parent_path() / (base.stem().u8string() + "." + std::to_string(i) + ".log");
    };
    fs::remove(rotated(config_.logKeep), ec);
    ec.clear();
    for (int i = config_.logKeep - 1; i >= 1; --i) {
        if (fs::exists(rotated(i), ec)) {
            fs::rename(rotated(i), rotated(i + 1), ec);
            if (ec)
                break;
        }
    }
    if (!ec)
        fs::rename(base, rotated(1), ec);
    if (ec) {
        active_ = false;
        status_ = L"Recording stopped: rotation failed";
        return false;
    }
    file_.open(base, std::ios::binary | std::ios::trunc);
    bytes_ = 0;
    if (!file_) {
        active_ = false;
        status_ = L"Recording stopped: cannot open rotated file";
        return false;
    }
    return true;
}
/** Rotate before exceeding the configured limit and stop cleanly on an I/O failure. */
void UsageRecorder::write(const std::string& event, const std::string& payload) {
    if (!active_)
        return;
    auto line = utcTimestamp() + " " + event + " " + payload + "\n";
    if (bytes_ + line.size() > static_cast<uint64_t>(config_.logMaxMiB) * 1024 * 1024 && !rotate())
        return;
    file_ << line;
    bytes_ += line.size();
    if (!file_) {
        active_ = false;
        status_ = L"Recording stopped: disk write failed";
        file_.close();
    }
}
/** Write current values plus retained peaks and their original client receipt timestamps. */
void UsageRecorder::summary(const std::wstring& id, Host& h, const char* event) {
    const auto& m = h.latest;
    const auto& r = m.raw;
    write(event, "node=\"" + safe(id) + "\" label=\"" + safe(h.alias) +
                     "\" cpu_pct=" + (m.ratesValid ? decimal(m.cpu) : "n/a") + " ram_pct=" + decimal(m.ram) +
                     " ram_used_bytes=" + std::to_string(r.memTotal - std::min(r.memTotal, r.memAvailable)) +
                     " ram_available_bytes=" + std::to_string(r.memAvailable) +
                     " disk_pct=" + decimal(m.disk) + " disk_used_bytes=" + std::to_string(r.diskUsed) +
                     " disk_total_bytes=" + std::to_string(r.diskTotal) +
                     " load_1m=" + (r.load >= 0 ? decimal(r.load) : "n/a") + " swap_used_bytes=" +
                     (r.swapKnown ? std::to_string(r.swapTotal - std::min(r.swapFree, r.swapTotal)) : "n/a") +
                     " swap_total_bytes=" + (r.swapKnown ? std::to_string(r.swapTotal) : "n/a") +
                     " rx_bytes_s=" + (m.ratesValid ? decimal(m.rxRate) : "n/a") +
                     " tx_bytes_s=" + (m.ratesValid ? decimal(m.txRate) : "n/a") +
                     " temperature_c=" + (r.temperature >= 0 ? decimal(r.temperature) : "n/a") +
                     " uptime_s=" + decimal(r.uptime) +
                     " window_peak_cpu_pct=" + (h.cpuPeakAt.empty() ? "n/a" : decimal(h.window.cpu)) +
                     " window_peak_ram_pct=" + decimal(h.window.ram) +
                     " window_peak_rx_bytes_s=" + (h.rxPeakAt.empty() ? "n/a" : decimal(h.window.rx)) +
                     " window_peak_tx_bytes_s=" + (h.txPeakAt.empty() ? "n/a" : decimal(h.window.tx)) +
                     " session_peak_cpu_pct=" + (h.hasRates ? decimal(h.lifetime.cpu) : "n/a") +
                     " session_peak_ram_pct=" + decimal(h.lifetime.ram) +
                     " session_peak_rx_bytes_s=" + (h.hasRates ? decimal(h.lifetime.rx) : "n/a") +
                     " session_peak_tx_bytes_s=" + (h.hasRates ? decimal(h.lifetime.tx) : "n/a") +
                     " window_cpu_peak_at=\"" + h.cpuPeakAt + "\" window_ram_peak_at=\"" + h.ramPeakAt +
                     "\" window_rx_peak_at=\"" + h.rxPeakAt + "\" window_tx_peak_at=\"" + h.txPeakAt + "\"");
    h.window = {};
    h.cpuPeakAt.clear();
    h.ramPeakAt.clear();
    h.rxPeakAt.clear();
    h.txPeakAt.clear();
    h.lastWrite = clock_();
}
/** Consume every new sample, update window/session peaks, and throttle high-use events. */
void UsageRecorder::observe(const std::wstring& id, const std::wstring& alias, const Metrics& m) {
    tick();
    if (!active_ || !m.valid)
        return;
    auto& h = hosts_[id];
    if (h.sampleTime == m.raw.time)
        return;
    h.sampleTime = m.raw.time;
    h.alias = alias;
    h.latest = m;
    h.hasSample = true;
    h.hasRates = h.hasRates || m.ratesValid;
    auto track = [&](UsagePeaks& p) {
        if (m.ratesValid) {
            p.cpu = std::max(p.cpu, m.cpu);
            p.rx = std::max(p.rx, m.rxRate);
            p.tx = std::max(p.tx, m.txRate);
        }
        p.ram = std::max(p.ram, m.ram);
    };
    const auto sampleStamp = utcTimestamp(m.raw.time - clock_());
    if (m.ratesValid && (h.cpuPeakAt.empty() || m.cpu > h.window.cpu))
        h.cpuPeakAt = sampleStamp;
    if (h.ramPeakAt.empty() || m.ram > h.window.ram)
        h.ramPeakAt = sampleStamp;
    if (m.ratesValid && (h.rxPeakAt.empty() || m.rxRate > h.window.rx))
        h.rxPeakAt = sampleStamp;
    if (m.ratesValid && (h.txPeakAt.empty() || m.txRate > h.window.tx))
        h.txPeakAt = sampleStamp;
    track(h.window);
    track(h.lifetime);
    const auto now = clock_();
    if (((m.ratesValid && m.cpu >= config_.spikeCpu) || m.ram >= config_.spikeRam) &&
        now - h.lastSpike >= 10) {
        write("SPIKE", "node=\"" + safe(id) + "\" cpu_pct=" + (m.ratesValid ? decimal(m.cpu) : "n/a") +
                           " ram_pct=" + decimal(m.ram));
        h.lastSpike = now;
    }
    if (now - h.lastWrite >= config_.logEveryMs / 1000.0)
        summary(id, h, "SAMPLE");
}
/** Emit availability transitions once instead of repeating the same state every frame. */
void UsageRecorder::connection(const std::wstring& id, bool online) {
    tick();
    if (!active_)
        return;
    auto& h = hosts_[id];
    if (!h.knownConnection || h.connected != online) {
        write(online ? "CONNECTED" : "DISCONNECTED", "node=\"" + safe(id) + "\"");
        h.knownConnection = true;
        h.connected = online;
    }
}
/** Check time limits and flush at least once a second without manufacturing new data. */
void UsageRecorder::tick() {
    if (!active_)
        return;
    const auto now = clock_();
    if (config_.logDurationMinutes && now - started_ >= config_.logDurationMinutes * 60.0) {
        stop("duration");
        status_ = L"Recording complete: duration reached";
        return;
    }
    if (now - lastFlush_ >= 1) {
        file_.flush();
        lastFlush_ = now;
        if (!file_) {
            active_ = false;
            status_ = L"Recording stopped: flush failed";
            file_.close();
        }
    }
}
/** Write final summaries and close the stream; continuous monitoring is unaffected. */
void UsageRecorder::stop(const std::string& reason) {
    if (active_) {
        for (auto& entry : hosts_)
            if (entry.second.hasSample)
                summary(entry.first, entry.second, "SUMMARY");
        write("SESSION_END", "reason=" + reason);
    }
    active_ = false;
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
    if (reason != "duration")
        status_ = L"Recording is off";
}
/** Return session peaks for a known node without inserting a new map entry. */
UsagePeaks UsageRecorder::peaks(const std::wstring& id) const {
    auto i = hosts_.find(id);
    return i == hosts_.end() ? UsagePeaks{} : i->second.lifetime;
}
