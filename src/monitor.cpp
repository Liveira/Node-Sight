#include "monitor.h"
#include <algorithm>
#include <cmath>

/** End recordings and join owned collectors before the shared model disappears. */
MonitorHub::~MonitorHub() {
    stop();
}
/** Stop only this hub's SSH jobs and flush final recording summaries. */
void MonitorHub::stop() {
    recorder_.stop("shutdown");
    // Stop existing jobs before freeing entries; no worker references Config or the UI.
    for (auto& entry : entries_)
        if (entry.collector)
            entry.collector->stop();
    entries_.clear();
    configured_ = false;
}
/** Preserve unaffected transports and recording sessions when only appearance/visibility changes. */
void MonitorHub::configure(const Config& config) {
    std::vector<Entry> next;
    for (size_t i = 0; i < config.servers.size(); ++i) {
        const auto& profile = config.servers[i];
        if (!profile.enabled)
            continue;
        bool reused = false;
        for (auto& entry : entries_)
            if (entry.collector && entry.profile.id == profile.id && entry.profile.host == profile.host &&
                entry.profile.user == profile.user && entry.profile.port == profile.port &&
                entry.profile.key == profile.key && entry.intervalMs == config.intervalMs) {
                entry.profile = profile;
                next.push_back(std::move(entry));
                reused = true;
                break;
            }
        if (!reused) {
            Entry entry;
            entry.profile = profile;
            entry.intervalMs = config.intervalMs;
            entry.collector = std::make_unique<RemoteCollector>();
            entry.collector->start(config.forServer(i));
            next.push_back(std::move(entry));
        }
    }
    for (auto& entry : entries_)
        if (entry.collector) {
            entry.collector->stop();
            recorder_.removeHost(entry.profile.id);
        }
    entries_ = std::move(next);
    if (!configured_ || config.local != previousConfig_.local ||
        config.intervalMs != previousConfig_.intervalMs) {
        if (configured_ && !config.local && previousConfig_.local)
            recorder_.removeHost(L"local");
        localMetrics_.reset();
        lastLocal_ = -1;
    }
    const auto& old = previousConfig_;
    bool changed = !configured_ || config.logEnabled != old.logEnabled ||
                   config.logDurationMinutes != old.logDurationMinutes ||
                   config.logEveryMs != old.logEveryMs || config.logDirectory != old.logDirectory ||
                   config.logMaxMiB != old.logMaxMiB || config.logKeep != old.logKeep ||
                   config.spikeCpu != old.spikeCpu || config.spikeRam != old.spikeRam;
    if (changed) {
        if (config.logEnabled)
            recorder_.start(config);
        else
            recorder_.stop();
    }
    previousConfig_ = config;
    configured_ = true;
    tick(config);
}
/** Begin a fresh recording session without restarting SSH transports. */
void MonitorHub::restartRecording(const Config& config) {
    recorder_.start(config);
}
/** Stop file output while leaving all enabled monitoring sessions active. */
void MonitorHub::stopRecording() {
    recorder_.stop();
}
/** Drain every received frame, then sample the local host and process counters at bounded cadences. */
void MonitorHub::tick(const Config& config) {
    const auto now = monotonicSeconds();
    for (auto& entry : entries_) {
        auto batch = entry.collector->takeSamples();
        if (batch.dropped) {
            recorder_.gap(entry.profile.id, batch.dropped);
            entry.recorded.reset();
        }
        for (const auto& raw : batch.samples) {
            entry.recorded.update(raw);
            recorder_.observe(entry.profile.id, entry.profile.name, entry.recorded);
        }
        auto state = entry.collector->snapshot();
        bool live =
            state.connected && now - state.lastSample < std::max(8.0, config.intervalMs / 1000.0 * 4 + 3);
        recorder_.connection(entry.profile.id, live);
        if (state.connected && !live) {
            for (size_t i = 0; i < config.servers.size(); ++i)
                if (config.servers[i].id == entry.profile.id) {
                    entry.collector->start(config.forServer(i));
                    entry.recorded.reset();
                    break;
                }
        }
    }
    if (config.local && now - lastLocal_ >= config.intervalMs / 1000.0) {
        RawSample raw;
        if (local_.sample(raw)) {
            localMetrics_.update(raw);
            recorder_.observe(L"local", L"This computer", localMetrics_);
        }
        lastLocal_ = now;
    }
    recorder_.tick();
    // Process accounting is intentionally capped at 1 Hz even when sampling is 10 Hz.
    if (now - lastProcess_ >= 1) {
        auto p = processCounters();
        rss_ = static_cast<double>(p.residentBytes) / (1024 * 1024);
        peakRss_ = std::max(peakRss_, rss_);
        if (lastCpuTime_ > 0 && now > lastCpuTime_)
            cpu_ = std::max(0.0, 100 * (p.cpuSeconds - lastCpu_) / (now - lastCpuTime_) /
                                     std::max(1u, p.logicalProcessors));
        lastCpu_ = p.cpuSeconds;
        lastCpuTime_ = now;
        lastProcess_ = now;
    }
}
/** Copy visible cards for painting; diagnostics can explicitly include hidden enabled profiles. */
DashboardState MonitorHub::snapshot(const Config& config, bool includeHidden) {
    DashboardState result;
    const auto now = monotonicSeconds();
    for (const auto& profile : config.servers) {
        if (!profile.enabled)
            continue;
        ++result.activeServers;
        if (!profile.visible && !includeHidden)
            continue;
        HostView view;
        view.profile = profile;
        for (auto& entry : entries_)
            if (entry.profile.id == profile.id) {
                auto state = entry.collector->snapshot();
                view.metrics = state.metrics;
                view.message = state.message;
                view.connected = state.connected &&
                                 now - state.lastSample < std::max(8.0, config.intervalMs / 1000.0 * 4 + 3);
                break;
            }
        view.peaks =
            UsagePeaks{view.metrics.peakCpu, view.metrics.peakRam, view.metrics.peakRx, view.metrics.peakTx};
        result.hosts.push_back(std::move(view));
    }
    if (config.local) {
        HostView view;
        view.profile.name = L"This computer";
        view.profile.id = L"local";
        view.profile.collapsed = config.localCollapsed;
        view.metrics = localMetrics_;
        view.connected = localMetrics_.valid;
        view.isLocal = true;
        view.message = local_.computerName();
        view.peaks =
            UsagePeaks{view.metrics.peakCpu, view.metrics.peakRam, view.metrics.peakRx, view.metrics.peakTx};
        result.hosts.push_back(std::move(view));
    }
    result.residentMiB = rss_;
    result.cpuPercent = cpu_;
    result.peakResidentMiB = peakRss_;
    result.recording = recorder_.active();
    result.logStatus = recorder_.status();
    result.logPath = recorder_.path();
    return result;
}
/** Build anonymous deterministic fixtures without making network connections or reading user identities. */
DashboardState demoDashboard(const Config& config) {
    DashboardState state;
    for (int host = 0; host < 2 + (config.local ? 1 : 0); ++host) {
        HostView view;
        view.profile.id = L"demo-" + std::to_wstring(host);
        view.profile.name = host == 2 ? L"This computer" : host ? L"Storage node" : L"Application node";
        view.profile.host = L"server.example.com";
        view.profile.user = L"monitor";
        view.connected = true;
        view.isLocal = host == 2;
        RawSample s;
        constexpr uint64_t GiB = 1024ULL * 1024 * 1024;
        for (size_t i = 0; i < HISTORY; ++i) {
            double wave = std::sin(i * 0.16 + host);
            s.total += 1000;
            s.idle += static_cast<uint64_t>(750 - 180 * wave);
            s.memTotal = (host + 1) * 8 * GiB;
            s.memAvailable = static_cast<uint64_t>((0.35 + 0.08 * wave) * s.memTotal);
            s.diskTotal = 128 * GiB;
            s.diskUsed = (40 + host * 10) * GiB;
            s.swapTotal = 4 * GiB;
            s.swapFree = 3 * GiB;
            s.rx += static_cast<uint64_t>((0.9 + 0.6 * wave) * 1024 * 1024);
            s.tx += 100000;
            s.uptime = 90000 + static_cast<double>(i * 2);
            s.time = i * 2.0;
            s.load = 0.8;
            s.temperature = 48 + 4 * wave;
            view.metrics.update(s);
        }
        view.peaks = {view.metrics.peakCpu, view.metrics.peakRam, view.metrics.peakRx, view.metrics.peakTx};
        state.hosts.push_back(std::move(view));
    }
    state.activeServers = 2;
    state.residentMiB = 16;
    state.peakResidentMiB = 16;
    state.cpuPercent = 0.02;
    state.logStatus = L"Demo · synthetic data";
    return state;
}
