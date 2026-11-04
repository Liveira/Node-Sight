#include "dashboard.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
using namespace Palette;

/** Keep numeric UI formatting consistent on both desktop platforms. */
std::wstring formatNumber(double n, int digits) {
    std::wostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(digits) << n;
    return out.str();
}
/** Display binary IEC units while retaining byte-based counters internally. */
std::wstring formatBytes(double n) {
    const wchar_t* units[] = {L"B", L"KiB", L"MiB", L"GiB", L"TiB"};
    int i = 0;
    while (n >= 1024 && i < 4) {
        n /= 1024;
        ++i;
    }
    return formatNumber(n, n >= 100 || !i ? 0 : 1) + L" " + units[i];
}
/** Choose readable elapsed-time units for uptime and history capacity. */
std::wstring formatDuration(double seconds) {
    auto n = static_cast<uint64_t>(std::max(0.0, seconds));
    if (n >= 86400)
        return std::to_wstring(n / 86400) + L"d " + std::to_wstring(n / 3600 % 24) + L"h";
    if (n >= 3600)
        return std::to_wstring(n / 3600) + L"h " + std::to_wstring(n / 60 % 60) + L"m";
    if (n >= 60)
        return std::to_wstring(n / 60) + L"m " + std::to_wstring(n % 60) + L"s";
    return formatNumber(seconds, seconds < 10 ? 1 : 0) + L"s";
}
namespace {
/** Thin numbered frames reproduce terminal panels without filled cards or rounded chrome. */
void frame(Canvas& g, int x, int y, int w, int h, const std::wstring& title, int index, Color ink) {
    const Color edge = ((ink & 0xfefefe) >> 1);
    g.line(x + 2, y, x + w - 2, y, edge);
    g.line(x, y + 2, x, y + h - 2, edge);
    g.line(x + w, y + 2, x + w, y + h - 2, edge);
    g.line(x + 2, y + h, x + w - 2, y + h, edge);
    for (int side : {0, 1}) {
        const int xx = x + side * w, direction = side ? -2 : 2;
        g.line(xx, y + 2, xx + direction, y, edge);
        g.line(xx, y + h - 2, xx + direction, y + h, edge);
    }
    const int labelWidth = static_cast<int>(title.size()) * 7 + 19;
    g.fill(x + 7, y - 8, labelWidth, 17, background);
    g.text(x + 9, y - 8, 9, 17, std::to_wstring(index), 0, red);
    g.text(x + 18, y - 9, labelWidth - 15, 18, title, 1, text);
}
/** Percentage meters use discrete terminal cells and never extend beyond their supplied width. */
void meter(Canvas& g, int x, int y, int w, double percent, Color ink) {
    int count = std::max(1, w / 4);
    int lit = static_cast<int>(std::lround(count * std::clamp(percent, 0.0, 100.0) / 100));
    for (int i = 0; i < count; ++i)
        g.fill(x + i * 4, y, 2, 4, i < lit ? ink : border);
}
/** Preserve each column's peak; the optional mirrored direction gives RX/TX a shared baseline. */
void graph(Canvas& g, int x, int y, int w, int h, const Series& series, Color ink,
           double maximum = 100, bool mirrored = false) {
    int columns = std::max(1, w / 3), rows = std::max(1, h / 3);
    maximum = std::max(1.0, maximum);
    for (int column = 0; column < columns; ++column) {
        g.fill(x + column * 3, mirrored ? y : y + h - 1, 1, 1, border);
        size_t begin = static_cast<size_t>(column) * HISTORY / static_cast<size_t>(columns);
        size_t end = static_cast<size_t>(column + 1) * HISTORY / static_cast<size_t>(columns);
        size_t offset = HISTORY - series.count;
        if (series.count == 0 || end <= offset)
            continue;
        double value = 0;
        for (size_t i = std::max(begin, offset); i < end && i - offset < series.count; ++i)
            value = std::max(value, static_cast<double>(series.at(i - offset)));
        int lit = static_cast<int>(std::ceil(std::clamp(value / maximum, 0.0, 1.0) * rows));
        for (int row = 0; row < std::max(1, lit); ++row)
            g.fill(x + column * 3, mirrored ? y + row * 3 : y + h - 2 - row * 3, 1, 1,
                   lit ? ink : border);
    }
}
/** Keep the optional PC overview small, as in the original widget; Detailed expands every node. */
bool compact(const HostView& h, const Config& c) {
    return h.profile.collapsed || c.detail == 0 || (h.isLocal && c.detail == 1);
}
/** Drawing, scrolling and hit tests all consume the same metric-dependent vertical geometry. */
int hostHeight(const HostView& host, const Config& c) {
    if (compact(host, c))
        return host.isLocal && !host.profile.collapsed && c.detail == 1 && c.showSensors ? 88 : 66;
    int h = 30;
    if (c.showCpu)
        h += (c.graphs ? 76 : 54) + 14;
    else if (c.showSensors)
        h += 24;
    if (c.showRam || c.showDisk)
        h += (c.graphs ? 90 : 74) + 14;
    if (c.showNetwork)
        h += (c.graphs ? 62 : 50) + 14;
    if (c.detail == 2) {
        if (c.showSwap)
            h += 18;
        h += 36;
    }
    return h + 4;
}
/** Pair terse terminal labels with right-aligned counters, keeping units legible at mini zoom. */
void pair(Canvas& g, int x, int y, int w, const std::wstring& label, const std::wstring& value, Color ink) {
    g.text(x, y, w, 17, label, 0, muted);
    g.text(x, y, w, 17, value, 1, ink, true);
}
/** Render one node as CPU, memory, disk and network terminal panes, preserving all new preferences. */
void hostPanel(Canvas& g, const HostView& host, const Config& c, int top) {
    const auto& m = host.metrics;
    const auto& raw = m.raw;
    constexpr int left = 8, width = WIDGET_WIDTH - 16;
    const int right = left + width;
    g.line(left, top + 8, right, top + 8, border);
    g.fill(left + 6, top, 240, 18, background);
    g.text(left + 9, top, 222, 18, (host.isLocal ? L"pc / " : L"ssh / ") + host.profile.name, 1, text);
    g.fill(right - 98, top, 98, 18, background);
    g.text(right - 98, top, 72, 18, host.connected ? L"online" : L"offline", 0,
           host.connected ? green : red, true);
    g.text(right - 20, top, 20, 18, host.profile.collapsed ? L"[+]" : L"[-]", 0, muted, true);
    if (compact(host, c)) {
        frame(g, left, top + 24, width, hostHeight(host, c) - 28, L"resources", host.isLocal ? 5 : 1, dim);
        const std::wstring cpu = m.ratesValid ? formatNumber(m.cpu) + L"%" : L"--";
        const std::wstring ram = m.valid ? formatNumber(m.ram) + L"%" : L"--";
        if (c.showCpu)
            g.text(left + 9, top + 30, 150, 16, L"cpu " + cpu, 1, green);
        if (c.showRam)
            g.text(left + 184, top + 30, 150, 16, L"mem " + ram, 1, gold);
        if (c.showDisk)
            g.text(left + 9, top + 46, 150, 16,
                   L"disk " + (m.valid ? formatNumber(m.disk) + L"%" : L"--"), 0, muted);
        if (c.showNetwork)
            g.text(left + 184, top + 46, 150, 16,
                   m.ratesValid ? L"↓ " + formatBytes(m.rxRate) + L"/s" : L"↓ --", 0, cyan);
        if (hostHeight(host, c) > 66)
            g.text(left + 9, top + 65, width - 18, 16,
                   m.valid ? L"up " + formatDuration(raw.uptime) : host.message, 0, dim);
        return;
    }
    int y = top + 30;
    if (c.showCpu) {
        int h = c.graphs ? 76 : 54;
        frame(g, left, y, width, h, L"cpu", 1, green);
        if (c.graphs) {
            graph(g, left + 10, y + 15, 220, 40, m.cpuHistory, host.connected ? green : dim);
            g.line(left + 235, y + 12, left + 235, y + h - 12, border);
        } else
            meter(g, left + 10, y + 22, 220, m.cpu, host.connected ? green : dim);
        g.text(right - 103, y + 12, 93, 18,
               m.ratesValid ? L"CPU " + formatNumber(m.cpu) + L"%" : L"CPU --", 1, text, true);
        if (c.showSensors) {
            g.text(right - 103, y + 30, 93, 16,
                   m.valid && raw.temperature >= 0 ? formatNumber(raw.temperature, 0) + L" °C" : L"temp n/a",
                   1, raw.temperature >= 80 ? red : cyan, true);
            if (c.graphs)
                g.text(right - 103, y + 49, 93, 16,
                       raw.load >= 0 ? L"load " + formatNumber(raw.load, 2) : L"load n/a", 0, dim, true);
            g.text(left + 10, y + h - 20, 220, 17,
                   m.valid ? L"up " + formatDuration(raw.uptime) : L"collecting...", 0, dim);
        }
        y += h + 14;
    } else if (c.showSensors) {
        g.text(left + 9, y, width - 18, 18,
               m.valid ? L"up " + formatDuration(raw.uptime) + L"  temp " +
                             (raw.temperature >= 0 ? formatNumber(raw.temperature, 0) + L" °C" : L"n/a")
                       : L"collecting...", 0, muted);
        y += 24;
    }
    if (c.showRam || c.showDisk) {
        const int w = c.showRam && c.showDisk ? (width - 8) / 2 : width;
        const int h = c.graphs ? 90 : 74;
        if (c.showRam) {
            frame(g, left, y, w, h, L"mem", 2, gold);
            pair(g, left + 9, y + 12, w - 18, L"Total:", m.valid ? formatBytes(static_cast<double>(raw.memTotal)) : L"--", text);
            pair(g, left + 9, y + 29, w - 18,
                 m.valid ? L"Used " + formatNumber(m.ram, 0) + L"%" : L"Used:",
                 m.valid ? formatBytes(static_cast<double>(raw.memTotal - std::min(raw.memAvailable, raw.memTotal))) : L"--", gold);
            pair(g, left + 9, y + 46, w - 18, L"Avail:",
                 m.valid ? formatBytes(static_cast<double>(raw.memAvailable)) : L"--", cyan);
            meter(g, left + 9, y + 66, w - 18, m.ram, gold);
            if (c.graphs)
                graph(g, left + 9, y + 74, w - 18, 9, m.ramHistory, gold);
        }
        if (c.showDisk) {
            const int x = c.showRam ? left + w + 8 : left;
            frame(g, x, y, w, h, L"disks", 3, gold);
            pair(g, x + 9, y + 12, w - 18, L"root /", m.valid ? formatNumber(m.disk) + L"%" : L"--", text);
            pair(g, x + 9, y + 29, w - 18, L"Used:", m.valid ? formatBytes(static_cast<double>(raw.diskUsed)) : L"--", cyan);
            pair(g, x + 9, y + 46, w - 18, L"Total:", m.valid ? formatBytes(static_cast<double>(raw.diskTotal)) : L"--", muted);
            meter(g, x + 9, y + 66, w - 18, m.disk, green);
            if (c.graphs)
                graph(g, x + 9, y + 74, w - 18, 9, m.diskHistory, gold);
        }
        y += h + 14;
    }
    if (c.showNetwork) {
        int h = c.graphs ? 62 : 50;
        frame(g, left, y, width, h, L"net", 4, purple);
        if (c.graphs) {
            double maximum = std::max({1024.0, static_cast<double>(m.rxHistory.max()),
                                      static_cast<double>(m.txHistory.max())});
            graph(g, left + 10, y + 13, 172, 18, m.rxHistory, cyan, maximum);
            graph(g, left + 10, y + 33, 172, 18, m.txHistory, purple, maximum, true);
            g.line(left + 191, y + 10, left + 191, y + h - 10, border);
        }
        int x = c.graphs ? left + 201 : left + 10, w = c.graphs ? width - 211 : width - 20;
        g.text(x, y + 12, w, 18, m.ratesValid ? L"↓ " + formatBytes(m.rxRate) + L"/s" : L"↓ --", 1, cyan);
        g.text(x, y + 32, w, 18, m.ratesValid ? L"↑ " + formatBytes(m.txRate) + L"/s" : L"↑ --", 1, purple);
        y += h + 14;
    }
    if (c.detail == 2) {
        if (c.showSwap) {
            g.text(left + 9, y, width - 18, 18,
                   m.valid && raw.swapKnown ? L"swap " + formatBytes(static_cast<double>(raw.swapTotal - std::min(raw.swapFree, raw.swapTotal))) +
                                                L" / " + formatBytes(static_cast<double>(raw.swapTotal)) : L"swap n/a", 0, dim);
            y += 18;
        }
        g.text(left + 9, y, width - 18, 18,
               L"peak cpu " + formatNumber(host.peaks.cpu) + L"%  mem " + formatNumber(host.peaks.ram) + L"%", 0, dim);
        g.text(left + 9, y + 18, width - 18, 18,
               L"peak ↓ " + formatBytes(host.peaks.rx) + L"/s  ↑ " + formatBytes(host.peaks.tx) + L"/s", 0, dim);
    }
}
} // namespace
/** Use the same panel geometry for drawing, collapse hit tests and viewport scrolling. */
DashboardLayout dashboardLayout(const DashboardState& state, const Config& c) {
    DashboardLayout layout;
    int y = HEADER_HEIGHT;
    for (const auto& host : state.hosts) {
        layout.hosts.push_back({host.profile.id, y, y + hostHeight(host, c) - 4});
        y += hostHeight(host, c);
    }
    if (state.hosts.empty())
        y += 80;
    layout.height = y + FOOTER_HEIGHT;
    return layout;
}
/** The fixed terminal command line covers scrolled content beneath the header. */
void drawDashboard(Canvas& g, const DashboardState& state, const Config& c, int scroll, bool demo,
                   int viewportHeight) {
    const auto layout = dashboardLayout(state, c);
    g.fill(0, 0, WIDGET_WIDTH, layout.height, background);
    int y = HEADER_HEIGHT - scroll;
    for (const auto& host : state.hosts) {
        int h = hostHeight(host, c);
        if (viewportHeight == 0 || (y + h > HEADER_HEIGHT && y < viewportHeight - FOOTER_HEIGHT))
            hostPanel(g, host, c, y);
        y += h;
    }
    if (state.hosts.empty())
        g.text(16, HEADER_HEIGHT + 12, WIDGET_WIDTH - 32, 60,
               L"All nodes hidden. Open settings to show one.", 0, muted);
    g.fill(0, 0, WIDGET_WIDTH, HEADER_HEIGHT, background);
    g.text(10, 7, 188, 20, demo ? L"node-sight / demo" : L"node-sight", 2, text);
    g.text(HEADER_PIN, 7, 50, 20, c.pinned ? L"[pin*]" : L"[pin]", 0, c.pinned ? green : dim);
    g.text(HEADER_SETTINGS, 7, 52, 20, L"[config]", 0, muted);
    g.text(HEADER_HIDE, 7, 18, 20, L"_", 1, muted);
    g.text(HEADER_CLOSE, 7, 18, 20, L"×", 1, red);
    g.text(10, 30, 242, 18,
           std::to_wstring(state.activeServers) + L" ssh  " +
               std::to_wstring(c.intervalMs) + L"ms  hist " + formatDuration(HISTORY * c.intervalMs / 1000.0),
           0, dim);
    g.text(254, 30, 96, 18, state.recording ? L"● REC" : L"monitoring", 0,
           state.recording ? red : green, true);
}
/** Fixed footer keeps local process cost and recording status available when nodes are scrolled. */
void drawFooter(Canvas& g, const DashboardState& state, int height) {
    g.fill(0, height - FOOTER_HEIGHT, WIDGET_WIDTH, FOOTER_HEIGHT, background);
    g.line(8, height - FOOTER_HEIGHT, WIDGET_WIDTH - 8, height - FOOTER_HEIGHT, border);
    g.text(9, height - 44, WIDGET_WIDTH - 18, 18,
           L"app " + formatNumber(state.residentMiB) + L" MiB  cpu " + formatNumber(state.cpuPercent, 2) +
               L"%  peak " + formatNumber(state.peakResidentMiB) + L" MiB", 0, dim);
    g.text(9, height - 24, WIDGET_WIDTH - 18, 18, state.logStatus, 0, state.recording ? green : dim);
}
