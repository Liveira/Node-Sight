// Native widget entry point, terminal-style renderer, settings dialog and diagnostic exports.
// All runtime settings and credentials remain outside the repository.

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <wincred.h>
#include <psapi.h>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <vector>
#include "metrics.h"
#include "remote.h"
#include "config.h"

namespace {
// Terminal palette and fixed logical dimensions; GDI performs the actual drawing.
constexpr COLORREF BG = RGB(0, 0, 0), CARD = RGB(15, 16, 16), GRID = RGB(43, 47, 42);
constexpr COLORREF TEXT = RGB(222, 230, 218), MUTED = RGB(155, 164, 146), DIM = RGB(101, 109, 96);
constexpr COLORREF GREEN = RGB(151, 197, 121), PURPLE = RGB(160, 131, 203), BLUE = RGB(102, 203, 217),
                   GOLD = RGB(210, 201, 111), RED = RGB(211, 111, 110);
constexpr int WIDTH = 360, FULL_HEIGHT = 468, SHORT_HEIGHT = 382;
constexpr UINT WM_TRAY = WM_APP + 1;
// Command IDs use contiguous ranges to map submenu selections to small value tables.
enum {
    PIN = 101,
    OPTIONS,
    MINIMIZE,
    CLOSE,
    LOCAL,
    RECONNECT = 201,
    SETTINGS,
    SHOW,
    QUIT,
    LOCAL_TOGGLE,
    INTERVAL1 = 220,
    INTERVAL2,
    INTERVAL5,
    INTERVAL10,
    OPACITY70 = 240,
    OPACITY85,
    OPACITY94,
    OPACITY100,
    SIZE80 = 260,
    SIZE100,
    SIZE125
};
// Shared logical geometry for live controls and documentation exports.
struct ButtonRect {
    int id, x, y, w, h;
};
constexpr ButtonRect buttonRects[] = {{PIN, 204, 6, 60, 22},
                                      {OPTIONS, 268, 6, 44, 22},
                                      {MINIMIZE, 315, 6, 18, 22},
                                      {CLOSE, 335, 6, 18, 22},
                                      {LOCAL, 244, 358, 45, 18}};
// The UI thread owns these objects; remote data is read only through locked snapshots.
Config config;
RemoteCollector remote;
LocalCollector localCollector;
Metrics localMetrics;
HWND mainWindow = nullptr, settingsWindow = nullptr;
HINSTANCE instance = nullptr;
float scale = 1;
HFONT fonts[5]{};
HBRUSH bgBrush = nullptr, editBrush = nullptr;
// One reusable backbuffer prevents flicker and per-frame bitmap allocation.
HDC backDC = nullptr;
HBITMAP backBitmap = nullptr, originalBitmap = nullptr;
int backW = 0, backH = 0;
NOTIFYICONDATAW tray{};
UINT taskbarCreated = 0;
std::wstring computer;
int hovered = 0;
double ownMemory = 0;
// Settings-dialog DPI is independent of the widget's optional mini zoom.
UINT settingsDpi = 96;
HFONT settingsFont = nullptr;
// Public documentation is rendered from anonymous fixtures rather than personal live data.
bool demoMode = false;
RemoteState demoState;

/** Convert logical layout coordinates to physical pixels using the current DPI and user scale. */
int px(int n) {
    return static_cast<int>(std::lround(n * scale));
}
/** Format a decimal with a locale-independent dot and a predictable number of fractional digits. */
std::wstring number(double n, int precision = 1) {
    std::wostringstream o;
    o.imbue(std::locale::classic());
    o << std::fixed << std::setprecision(precision) << n;
    return o.str();
}
/** Format bytes with binary IEC units; this function never changes the underlying measurement. */
std::wstring bytes(double n) {
    const wchar_t* units[] = {L"B", L"KiB", L"MiB", L"GiB", L"TiB"};
    int i = 0;
    while (n >= 1024 && i < 4) {
        n /= 1024;
        ++i;
    }
    return number(n, n >= 100 || i == 0 ? 0 : 1) + L" " + units[i];
}
/** Display elapsed boot time as days, hours and minutes without interpreting it as wall-clock time. */
std::wstring uptime(double n) {
    auto sec = static_cast<uint64_t>(std::max(n, 0.0));
    auto d = sec / 86400, h = sec / 3600 % 24, m = sec / 60 % 60;
    if (d)
        return std::to_wstring(d) + L"d " + std::to_wstring(h) + L"h " + std::to_wstring(m) + L"m";
    return std::to_wstring(h) + L"h " + std::to_wstring(m) + L"m";
}
/** Fill a logical rectangle and immediately release the temporary GDI brush. */
void fill(HDC dc, int x, int y, int w, int h, COLORREF color) {
    RECT r{px(x), px(y), px(x + w), px(y + h)};
    HBRUSH b = CreateSolidBrush(color);
    FillRect(dc, &r, b);
    DeleteObject(b);
}
/** Draw a scaled border segment, restoring the caller's selected pen before destroying ours. */
void line(HDC dc, int x1, int y1, int x2, int y2, COLORREF color, int width = 1) {
    HPEN p = CreatePen(PS_SOLID, std::max(1, px(width)), color);
    auto old = SelectObject(dc, p);
    MoveToEx(dc, px(x1), px(y1), nullptr);
    LineTo(dc, px(x2), px(y2));
    SelectObject(dc, old);
    DeleteObject(p);
}
/** Draw one clipped, vertically centered line; ampersands are literal and long labels are ellipsized. */
void text(HDC dc, int x, int y, int w, int h, const std::wstring& s, int font = 1, COLORREF c = TEXT,
          UINT flags = DT_LEFT) {
    SelectObject(dc, fonts[font]);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    RECT r{px(x), px(y), px(x + w), px(y + h)};
    DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &r,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | flags);
}
/** Draw the small connection indicator while preserving the device context's selected objects. */
void circle(HDC dc, int x, int y, int r, COLORREF c) {
    auto b = CreateSolidBrush(c);
    auto oldB = SelectObject(dc, b);
    auto oldP = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, px(x - r), px(y - r), px(x + r), px(y + r));
    SelectObject(dc, oldP);
    SelectObject(dc, oldB);
    DeleteObject(b);
}
// Dot-matrix columns follow the same fixed time window as the old line charts.
/** Render a bounded history as terminal-style dots. Missing history stays empty; network plots mirror RX/TX.
 */
void chart(HDC dc, int x, int y, int w, int h, const Series& s, COLORREF color, double ceiling = 100,
           const Series* second = nullptr, bool connected = true) {
    if (h < 4)
        return;
    const int columns = std::max(1, w / 3);
    ceiling = std::max(1.0, ceiling);
    auto plot = [&](const Series& data, int py, int height, COLORREF ink, bool flipped) {
        const int rowCount = std::max(1, height / 3);
        for (int column = 0; column < columns; ++column) {
            const size_t timeline = static_cast<size_t>(column) * HISTORY / static_cast<size_t>(columns);
            const size_t offset = HISTORY - data.count;
            if (timeline < offset || data.count == 0)
                continue;
            const auto index = std::min(timeline - offset, data.count - 1);
            const double value = std::clamp(static_cast<double>(data.at(index)) / ceiling, 0.0, 1.0);
            const int lit = static_cast<int>(std::ceil(value * rowCount));
            for (int row = 0; row < std::max(1, lit); ++row) {
                COLORREF c = connected ? ink : DIM;
                if (row == 0 && lit == 0)
                    c = GRID;
                const int dotY = flipped ? py + row * 3 : py + height - 2 - row * 3;
                fill(dc, x + column * 3, dotY, 1, 1, c);
            }
        }
    };
    // Only a faint baseline, like a terminal graph; no card fills or grid mesh.
    for (int c = 0; c < columns; ++c)
        fill(dc, x + c * 3, y + (second ? h / 2 : h - 2), 1, 1, GRID);
    if (s.count < 2) {
        if (h >= 24)
            text(dc, x, y + 3, w, h - 6, connected ? L"coletando..." : L"sem sinal", 0, DIM);
        return;
    }
    if (second) {
        const int half = std::max(3, h / 2 - 1);
        plot(s, y, half, color, false);
        plot(*second, y + h / 2 + 1, half, PURPLE, true);
    } else {
        // Bounded samples render as discrete cells instead of a smoothed curve.
        plot(s, y, h, color, false);
    }
}
/** Draw a terminal box and erase a short part of its top border for the numbered section label. */
void panel(HDC dc, int x, int y, int w, int h, const std::wstring& title, int index, COLORREF border) {
    line(dc, x + 2, y, x + w - 2, y, border);
    line(dc, x, y + 2, x, y + h - 2, border);
    line(dc, x + w, y + 2, x + w, y + h - 2, border);
    line(dc, x + 2, y + h, x + w - 2, y + h, border);
    line(dc, x, y + 2, x + 2, y, border);
    line(dc, x + w - 2, y, x + w, y + 2, border);
    line(dc, x, y + h - 2, x + 2, y + h, border);
    line(dc, x + w - 2, y + h, x + w, y + h - 2, border);
    const int labelWidth = static_cast<int>(title.size()) * 8 + 20;
    fill(dc, x + 7, y - 8, labelWidth, 17, BG);
    text(dc, x + 9, y - 8, 9, 17, std::to_wstring(index), 0, RED);
    text(dc, x + 18, y - 9, labelWidth - 18, 18, title, 1, TEXT);
}
/** Render a segmented percentage bar. Clamp invalid ranges so no cell can escape the supplied width. */
void meter(HDC dc, int x, int y, int width, double percent, COLORREF color) {
    const int cells = std::max(1, width / 4);
    const int lit = static_cast<int>(std::lround(cells * std::clamp(percent, 0.0, 100.0) / 100));
    for (int i = 0; i < cells; ++i)
        fill(dc, x + i * 4, y, 2, 4, i < lit ? color : GRID);
}
/** Recreate the small fixed font set after a DPI/size change; each previous HFONT is released first. */
void rebuildFonts() {
    for (auto& f : fonts) {
        if (f)
            DeleteObject(f);
        f = nullptr;
    }
    const int sizes[] = {10, 12, 13, 16, 14};
    for (int i = 0; i < 5; ++i)
        fonts[i] = CreateFontW(-px(sizes[i]), 0, 0, 0, i >= 2 ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               FIXED_PITCH | FF_MODERN, L"Consolas");
}
/** Select the logical window height from the optional local-PC panel state. */
int logicalHeight() {
    return config.local ? FULL_HEIGHT : SHORT_HEIGHT;
}
/** Combine monitor DPI and the saved size percentage, then fit the widget into the monitor work area. */
void updateScale(UINT dpi) {
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(mainWindow, MONITOR_DEFAULTTONEAREST), &mi);
    scale = std::min(static_cast<float>(dpi) / 96.0f * config.sizePercent / 100.0f,
                     static_cast<float>(mi.rcWork.bottom - mi.rcWork.top - 24) / logicalHeight());
    scale = std::max(0.65f, scale);
    rebuildFonts();
}
/** Apply topmost state, child geometry, transparency and button labels without moving or activating the
 * widget. */
void layout() {
    SetWindowPos(mainWindow, config.pinned ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, px(WIDTH),
                 px(logicalHeight()), SWP_NOMOVE | SWP_NOACTIVATE);
    for (const auto& b : buttonRects)
        SetWindowPos(GetDlgItem(mainWindow, b.id), nullptr, px(b.x), px(b.y), px(b.w), px(b.h),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    SetLayeredWindowAttributes(mainWindow, 0, static_cast<BYTE>(config.opacity * 255 / 100), LWA_ALPHA);
    SetWindowTextW(GetDlgItem(mainWindow, PIN), config.pinned ? L"[fixo]" : L"[pin]");
    SetWindowTextW(GetDlgItem(mainWindow, LOCAL), config.local ? L"[-pc]" : L"[+pc]");
    InvalidateRect(mainWindow, nullptr, FALSE);
}
/** Persist the top-left screen coordinates and all preferences after a user-visible layout change. */
void savePosition() {
    RECT r{};
    GetWindowRect(mainWindow, &r);
    config.x = r.left;
    config.y = r.top;
    config.save();
}
/** Toggle native topmost placement and persist the choice; no background focus-stealing loop is needed. */
void togglePin() {
    config.pinned = !config.pinned;
    layout();
    savePosition();
}
/** Show/hide the local panel; restart its timeline when shown to avoid averaging over a hidden interval. */
void toggleLocal() {
    config.local = !config.local;
    if (config.local) {
        localMetrics.reset();
        RawSample s;
        if (localCollector.sample(s))
            localMetrics.update(s);
    }
    updateScale(GetDpiForWindow(mainWindow));
    layout();
    savePosition();
}
/** Paint a consistent snapshot. SSH state is copied under its mutex before any GDI rendering begins. */
void paint(HDC dc) {
    const auto state = demoMode ? demoState : remote.snapshot();
    const auto& m = state.metrics;
    const auto& r = m.raw;
    const bool live =
        state.connected && monotonicSeconds() - state.lastSample <= std::max(8, config.interval * 3 + 3);
    const bool stale = m.valid && !live;
    const COLORREF ink = live ? TEXT : MUTED;
    fill(dc, 0, 0, WIDTH, logicalHeight(), BG);

    // Header: product title, connection identity and local clock.
    text(dc, 10, 6, 190, 22, demoMode ? L"node-sight / demo" : L"node-sight", 2, TEXT);
    circle(dc, 13, 38, 2, live ? GREEN : RED);
    text(dc, 21, 29, 249, 18, config.user + L"@" + config.host, 0, live ? MUTED : RED);
    SYSTEMTIME clock{};
    GetLocalTime(&clock);
    wchar_t clockText[12]{};
    swprintf_s(clockText, L"%02u:%02u:%02u", clock.wHour, clock.wMinute, clock.wSecond);
    if (demoMode)
        wcscpy_s(clockText, L"12:34:56");
    text(dc, 278, 29, 72, 18, clockText, 1, TEXT, DT_RIGHT);

    // CPU: fixed 0..100% history, optional thermal sensor, load and uptime.
    panel(dc, 8, 55, 344, 104, L"cpu", 1, RGB(88, 112, 78));
    fill(dc, 280, 47, 65, 17, BG);
    text(dc, 280, 47, 63, 17, std::to_wstring(config.interval * 1000) + L"ms", 0, MUTED, DT_RIGHT);
    chart(dc, 17, 79, 216, 43, m.cpuHistory, GREEN, 100, nullptr, live);
    line(dc, 243, 71, 243, 132, GRID);
    text(dc, 251, 72, 90, 19, m.ratesValid ? L"CPU " + number(m.cpu) + L"%" : L"CPU    —", 1, ink, DT_RIGHT);
    text(dc, 251, 93, 90, 18, r.temperature >= 0 && m.valid ? number(r.temperature, 0) + L" °C" : L"temp n/d",
         1, r.temperature >= 80 ? RED : BLUE, DT_RIGHT);
    text(dc, 251, 114, 90, 18, m.valid ? L"lav " + number(r.load, 2) : L"lav   —", 0, MUTED, DT_RIGHT);
    text(dc, 17, 136, 214, 17, m.valid ? L"up " + uptime(r.uptime) : L"aguardando servidor...", 0, DIM);
    text(dc, 246, 136, 96, 17,
         live    ? L"online"
         : stale ? L"atrasado"
                 : L"offline",
         0, live ? GREEN : RED, DT_RIGHT);

    // RAM: available memory includes reclaimable Linux cache.
    panel(dc, 8, 171, 166, 104, L"mem", 2, RGB(126, 125, 66));
    text(dc, 17, 183, 62, 18, L"Total:", 1, TEXT);
    text(dc, 78, 183, 86, 18, m.valid ? bytes(static_cast<double>(r.memTotal)) : L"—", 1, ink, DT_RIGHT);
    text(dc, 17, 203, 67, 18, m.valid ? L"Used " + number(m.ram, 0) + L"%" : L"Used:", 0, MUTED);
    text(dc, 83, 203, 81, 18, m.valid ? bytes(static_cast<double>(r.memTotal - r.memAvailable)) : L"—", 1,
         ink, DT_RIGHT);
    text(dc, 17, 223, 65, 18, L"Avail:", 1, MUTED);
    text(dc, 79, 223, 85, 18, m.valid ? bytes(static_cast<double>(r.memAvailable)) : L"—", 1, BLUE, DT_RIGHT);
    meter(dc, 17, 246, 148, m.ram, GOLD);
    chart(dc, 17, 256, 148, 12, m.ramHistory, GOLD, 100, nullptr, live);

    // Storage: capacity occupancy and swap, not I/O throughput.
    panel(dc, 182, 171, 170, 104, L"disks", 3, RGB(126, 125, 66));
    text(dc, 191, 183, 66, 18, L"root /", 1, TEXT);
    text(dc, 259, 183, 83, 18, m.valid && r.diskTotal ? number(m.disk) + L"%" : L"—", 1, ink, DT_RIGHT);
    text(dc, 191, 204, 151, 17,
         m.valid && r.diskTotal
             ? bytes(static_cast<double>(r.diskUsed)) + L" / " + bytes(static_cast<double>(r.diskTotal))
             : L"partição raiz",
         0, MUTED, DT_RIGHT);
    text(dc, 191, 223, 46, 18, L"swap", 1, TEXT);
    const double swapPercent =
        r.swapTotal ? 100.0 * static_cast<double>(r.swapTotal - r.swapFree) / static_cast<double>(r.swapTotal)
                    : 0;
    text(dc, 235, 223, 107, 18,
         m.valid ? number(swapPercent, 0) + L"% " + bytes(static_cast<double>(r.swapTotal - r.swapFree))
                 : L"—",
         0, MUTED, DT_RIGHT);
    meter(dc, 191, 246, 150, m.disk, RED);
    chart(dc, 191, 256, 150, 12, m.diskHistory, GOLD, 100, nullptr, live);

    // Network directions mirror around a shared adaptive scale.
    panel(dc, 8, 287, 344, 64, L"net", 4, RGB(109, 89, 156));
    const double networkCeiling = 1.15 * std::max({1024.0, static_cast<double>(m.rxHistory.max()),
                                                   static_cast<double>(m.txHistory.max())});
    chart(dc, 17, 302, 166, 36, m.rxHistory, BLUE, networkCeiling, &m.txHistory, live);
    line(dc, 191, 298, 191, 341, GRID);
    text(dc, 200, 299, 142, 19, m.ratesValid ? L"↓ " + bytes(m.rxRate) + L"/s" : L"↓ —", 1,
         live ? BLUE : DIM);
    text(dc, 200, 321, 142, 19, m.ratesValid ? L"↑ " + bytes(m.txRate) + L"/s" : L"↑ —", 1,
         live ? PURPLE : DIM);

    // Footer memory covers only the widget; benchmarks account for SSH separately.
    text(dc, 9, 358, 230, 18,
         live ? L"ssh:" + std::to_wstring(config.port) + L"  " + std::to_wstring(config.interval * 2) +
                    L"min de histórico"
              : state.message,
         0, live ? DIM : RED);
    text(dc, 292, 358, 60, 18, number(ownMemory, 0) + L" MiB", 0, DIM, DT_RIGHT);
    // Optional local-PC panel reuses the bounded metric model.
    if (config.local) {
        const auto& l = localMetrics;
        panel(dc, 8, 393, 344, 64, L"pc", 5, RGB(91, 95, 87));
        fill(dc, 177, 385, 166, 17, BG);
        text(dc, 177, 385, 165, 17, computer, 0, DIM, DT_RIGHT);
        text(dc, 17, 407, 143, 18, l.ratesValid ? L"cpu " + number(l.cpu) + L"%" : L"cpu —", 1, GREEN);
        text(dc, 192, 407, 150, 18, l.valid ? L"mem " + number(l.ram) + L"%" : L"mem —", 1, GOLD);
        chart(dc, 102, 411, 69, 10, l.cpuHistory, GREEN);
        chart(dc, 276, 411, 66, 10, l.ramHistory, GOLD);
        text(dc, 17, 431, 154, 17, l.valid ? L"disk " + number(l.disk) + L"%" : L"disk —", 0, MUTED);
        text(dc, 192, 431, 150, 17, l.ratesValid ? L"↓ " + bytes(l.rxRate) + L"/s" : L"net —", 0, BLUE);
    }
}
/** Paint an owner-drawn native button, including keyboard focus and the current pin selection. */
void drawButton(const DRAWITEMSTRUCT& d) {
    wchar_t label[80]{};
    GetWindowTextW(d.hwndItem, label, 80);
    const bool active = d.CtlID == PIN && config.pinned;
    auto b = CreateSolidBrush(active ? RGB(27, 53, 45)
                              : (d.itemState & ODS_SELECTED) || hovered == static_cast<int>(d.CtlID) ? GRID
                                                                                                     : BG);
    FillRect(d.hDC, &d.rcItem, b);
    DeleteObject(b);
    SelectObject(d.hDC, fonts[0]);
    SetBkMode(d.hDC, TRANSPARENT);
    SetTextColor(d.hDC, d.CtlID == CLOSE ? RED : active ? GREEN : MUTED);
    auto r = d.rcItem;
    DrawTextW(d.hDC, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (d.itemState & ODS_FOCUS) {
        InflateRect(&r, -2, -2);
        DrawFocusRect(d.hDC, &r);
    }
}
/** Restore the tray-hidden widget and foreground it only in response to an explicit user action. */
void showWidget() {
    ShowWindow(mainWindow, SW_SHOWNORMAL);
    SetForegroundWindow(mainWindow);
}
/** Open a single owned settings window, clamp it to the monitor, and disable the widget until it closes. */
void openSettings();
/** Build native preference submenus on demand, dispatch the selected command and release all menu handles. */
void contextMenu() {
    HMENU menu = CreatePopupMenu(), interval = CreatePopupMenu(), opacity = CreatePopupMenu(),
          size = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, SHOW, L"Mostrar widget");
    AppendMenuW(menu, MF_STRING | (config.pinned ? MF_CHECKED : 0), PIN, L"Sempre à frente\tP");
    AppendMenuW(menu, MF_STRING | (config.local ? MF_CHECKED : 0), LOCAL_TOGGLE,
                L"Mostrar recursos deste PC");
    int values[] = {1, 2, 5, 10};
    for (int i = 0; i < 4; ++i)
        AppendMenuW(interval, MF_STRING | (config.interval == values[i] ? MF_CHECKED : 0), INTERVAL1 + i,
                    (std::to_wstring(values[i]) + L" segundos").c_str());
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(interval), L"Intervalo de coleta");
    int alphas[] = {70, 85, 94, 100};
    for (int i = 0; i < 4; ++i)
        AppendMenuW(opacity, MF_STRING | (config.opacity == alphas[i] ? MF_CHECKED : 0), OPACITY70 + i,
                    (std::to_wstring(alphas[i]) + L"%").c_str());
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(opacity), L"Opacidade");
    const int sizes[] = {80, 100, 125};
    const wchar_t* labels[] = {L"80% · mini", L"100% · compacto", L"125% · ampliado"};
    for (int i = 0; i < 3; ++i)
        AppendMenuW(size, MF_STRING | (config.sizePercent == sizes[i] ? MF_CHECKED : 0), SIZE80 + i,
                    labels[i]);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(size), L"Tamanho do widget");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, SETTINGS, L"Configurar conexão...\tCtrl+, ");
    AppendMenuW(menu, MF_STRING, RECONNECT, L"Reconectar agora\tR");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, QUIT, L"Sair");
    POINT point;
    GetCursorPos(&point);
    SetForegroundWindow(mainWindow);
    const auto action =
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, mainWindow, nullptr);
    DestroyMenu(menu);
    if (action)
        SendMessageW(mainWindow, WM_COMMAND, action, 0);
    PostMessageW(mainWindow, WM_NULL, 0, 0);
}

/** Copy a dialog edit control into an owned string, reserving space for the Win32 terminating NUL. */
std::wstring field(HWND w, int id) {
    auto h = GetDlgItem(w, id);
    int n = GetWindowTextLengthW(h);
    std::wstring s(n + 1, 0);
    GetWindowTextW(h, s.data(), n + 1);
    s.resize(n);
    return s;
}
/** Scale settings-dialog coordinates independently of the compact widget's user-selected zoom. */
int spx(int n) {
    return MulDiv(n, static_cast<int>(settingsDpi), 96);
}
/** Create an accessible native static label using the settings window's shared font. */
void settingsLabel(HWND w, const wchar_t* label, int x, int y, int width = 440) {
    auto h = CreateWindowExW(0, L"STATIC", label, WS_CHILD | WS_VISIBLE, spx(x), spx(y), spx(width), spx(20),
                             w, nullptr, instance, nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont), TRUE);
}
/** Create a native edit/button control with tab navigation, scaled geometry and consistent typography. */
HWND settingsControl(HWND w, const wchar_t* kind, const wchar_t* value, int id, int x, int y, int width,
                     int height, DWORD style = 0) {
    auto h =
        CreateWindowExW(wcscmp(kind, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0, kind, value,
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, spx(x), spx(y), spx(width), spx(height),
                        w, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont), TRUE);
    return h;
}
/** Handle the owned connection dialog. Validate before saving; plaintext passwords never enter the INI file.
 */
LRESULT CALLBACK SettingsProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    // Build native accessible controls; mask the password edit.
    case WM_CREATE: {
        settingsDpi = GetDpiForWindow(w);
        settingsFont = CreateFontW(-spx(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                   L"Segoe UI");
        settingsLabel(w, L"Conexão com o servidor Linux", 20, 18);
        settingsLabel(w, L"Host / IP", 20, 53, 300);
        settingsLabel(w, L"Porta", 354, 53, 90);
        settingsControl(w, L"EDIT", config.host.c_str(), 501, 20, 75, 320, 27, ES_AUTOHSCROLL);
        settingsControl(w, L"EDIT", std::to_wstring(config.port).c_str(), 503, 354, 75, 90, 27, ES_NUMBER);
        settingsLabel(w, L"Usuário SSH", 20, 115);
        settingsControl(w, L"EDIT", config.user.c_str(), 502, 20, 137, 424, 27, ES_AUTOHSCROLL);
        settingsLabel(w, L"Chave privada (vazio: chaves padrão e ssh-agent)", 20, 177);
        settingsControl(w, L"EDIT", config.key.c_str(), 504, 20, 199, 347, 27, ES_AUTOHSCROLL);
        settingsControl(w, L"BUTTON", L"Buscar...", 510, 377, 199, 67, 27);
        settingsLabel(w, L"Senha opcional (vazio: manter a senha salva)", 20, 240);
        settingsControl(w, L"EDIT", L"", 505, 20, 262, 424, 27, ES_PASSWORD | ES_AUTOHSCROLL);
        SendDlgItemMessageW(w, 505, EM_SETLIMITTEXT, 512, 0);
        settingsLabel(w, L"A senha é guardada no Gerenciador de Credenciais do Windows.", 20, 300);
        settingsLabel(w, L"Nenhum agente é instalado no servidor. Leitura de /proc via SSH.", 20, 321);
        settingsControl(w, L"BUTTON", L"Remover senha salva", 511, 20, 359, 164, 29);
        settingsControl(w, L"BUTTON", L"Salvar e reconectar", IDOK, 194, 359, 150, 29, BS_DEFPUSHBUTTON);
        settingsControl(w, L"BUTTON", L"Cancelar", IDCANCEL, 354, 359, 90, 29);
        SendDlgItemMessageW(w, 501, EM_SETLIMITTEXT, 253, 0);
        SendDlgItemMessageW(w, 502, EM_SETLIMITTEXT, 128, 0);
        SendDlgItemMessageW(w, 503, EM_SETLIMITTEXT, 5, 0);
        SetFocus(GetDlgItem(w, 501));
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetTextColor(reinterpret_cast<HDC>(wp), TEXT);
        SetBkColor(reinterpret_cast<HDC>(wp), BG);
        return reinterpret_cast<LRESULT>(bgBrush);
    case WM_CTLCOLOREDIT:
        SetTextColor(reinterpret_cast<HDC>(wp), TEXT);
        SetBkColor(reinterpret_cast<HDC>(wp), CARD);
        return reinterpret_cast<LRESULT>(editBrush);
    // Route settings-only commands without touching the monitoring event loop.
    case WM_COMMAND:
        switch (LOWORD(wp)) {
            // Select a key without changing the process working directory.
        case 510: {
            wchar_t path[4096]{};
            OPENFILENAMEW ofn{sizeof(ofn)};
            ofn.hwndOwner = w;
            ofn.lpstrFile = path;
            ofn.nMaxFile = 4096;
            ofn.lpstrTitle = L"Selecionar chave SSH privada";
            ofn.lpstrFilter = L"Todos os arquivos\0*.*\0";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&ofn))
                SetDlgItemTextW(w, 504, path);
            return 0;
        }
            // Remove only the saved credential for the destination shown in the form.
        case 511: {
            Config c = config;
            c.host = field(w, 501);
            c.user = field(w, 502);
            c.port = _wtoi(field(w, 503).c_str());
            if (validConnection(c)) {
                CredDeleteW(c.credentialTarget().c_str(), CRED_TYPE_GENERIC, 0);
                SetDlgItemTextW(w, 505, L"");
                MessageBoxW(w, L"Senha removida deste perfil de conexão.", L"Node-Sight", MB_OK);
            }
            return 0;
        }
            // Validate first, then save settings and update the encrypted Windows credential.
        case IDOK: {
            Config c = config;
            c.host = field(w, 501);
            c.user = field(w, 502);
            c.port = _wtoi(field(w, 503).c_str());
            c.key = field(w, 504);
            if (!validConnection(c)) {
                MessageBoxW(w, L"Confira host, usuário e porta (1 a 65535).", L"Conexão inválida",
                            MB_OK | MB_ICONWARNING);
                return 0;
            }
            if (!c.key.empty() && GetFileAttributesW(c.key.c_str()) == INVALID_FILE_ATTRIBUTES) {
                MessageBoxW(w, L"A chave privada não foi encontrada no caminho informado.", L"Chave SSH",
                            MB_OK | MB_ICONWARNING);
                return 0;
            }
            auto password = field(w, 505);
            if (!password.empty()) {
                CREDENTIALW cred{};
                auto target = c.credentialTarget();
                cred.Type = CRED_TYPE_GENERIC;
                cred.TargetName = target.data();
                cred.UserName = c.user.data();
                cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
                cred.CredentialBlobSize = static_cast<DWORD>(password.size() * sizeof(wchar_t));
                cred.CredentialBlob = reinterpret_cast<LPBYTE>(password.data());
                bool ok = CredWriteW(&cred, 0) != FALSE;
                SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
                SetDlgItemTextW(w, 505, L"");
                if (!ok) {
                    MessageBoxW(w, L"Não foi possível salvar a senha no Windows.", L"Credenciais",
                                MB_OK | MB_ICONERROR);
                    return 0;
                }
            }
            config = c;
            config.save();
            remote.start(config);
            DestroyWindow(w);
            InvalidateRect(mainWindow, nullptr, FALSE);
            return 0;
        }
            // Discard unsaved edits.
        case IDCANCEL:
            DestroyWindow(w);
            return 0;
        }
        break;
    // Closing the dialog is equivalent to canceling.
    case WM_CLOSE:
        DestroyWindow(w);
        return 0;
    // Release dialog-only resources and re-enable its owner.
    case WM_DESTROY:
        settingsWindow = nullptr;
        EnableWindow(mainWindow, TRUE);
        SetForegroundWindow(mainWindow);
        if (settingsFont) {
            DeleteObject(settingsFont);
            settingsFont = nullptr;
        }
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}
/** Open one owned settings dialog and clamp its bounds to the monitor work area. */
void openSettings() {
    if (settingsWindow) {
        SetForegroundWindow(settingsWindow);
        return;
    }
    showWidget();
    RECT r{};
    GetWindowRect(mainWindow, &r);
    const auto dpi = GetDpiForWindow(mainWindow);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(mainWindow, MONITOR_DEFAULTTONEAREST), &mi);
    const int width = MulDiv(482, dpi, 96), height = MulDiv(448, dpi, 96);
    const int x =
        std::clamp(static_cast<int>(r.left), static_cast<int>(mi.rcWork.left),
                   std::max(static_cast<int>(mi.rcWork.left), static_cast<int>(mi.rcWork.right) - width));
    const int y =
        std::clamp(static_cast<int>(r.top) + 60, static_cast<int>(mi.rcWork.top),
                   std::max(static_cast<int>(mi.rcWork.top), static_cast<int>(mi.rcWork.bottom) - height));
    settingsWindow = CreateWindowExW(WS_EX_DLGMODALFRAME, L"NodeSightSettings", L"Node-Sight · conexão SSH",
                                     WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, width, height, mainWindow,
                                     nullptr, instance, nullptr);
    if (settingsWindow) {
        EnableWindow(mainWindow, FALSE);
        BOOL dark = TRUE;
        DwmSetWindowAttribute(settingsWindow, 20, &dark, sizeof(dark));
        ShowWindow(settingsWindow, SW_SHOW);
    }
}

/** Dispatch native widget messages. The timer samples data; painting consumes snapshots and reuses its
 * buffer. */
LRESULT CALLBACK MainProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (taskbarCreated && msg == taskbarCreated) {
        Shell_NotifyIconW(NIM_ADD, &tray);
        return 0;
    }
    switch (msg) {
    // Create native controls, one timer and the notification-area entry.
    case WM_CREATE: {
        mainWindow = w;
        updateScale(GetDpiForWindow(w));
        auto button = [&](int id, const wchar_t* title) {
            CreateWindowExW(0, L"BUTTON", title, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10,
                            10, w, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        };
        button(PIN, L"[pin]");
        button(OPTIONS, L"menu");
        button(MINIMIZE, L"_");
        button(CLOSE, L"×");
        button(LOCAL, L"[-pc]");
        layout();
        RawSample s;
        if (localCollector.sample(s))
            localMetrics.update(s);
        SetTimer(w, 1, config.interval * 1000, nullptr);
        tray.cbSize = sizeof(tray);
        tray.hWnd = w;
        tray.uID = 1;
        tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        tray.uCallbackMessage = WM_TRAY;
        tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(tray.szTip, L"Node-Sight · duplo clique para mostrar");
        Shell_NotifyIconW(NIM_ADD, &tray);
        HWND tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP, 0, 0,
                                    0, 0, w, nullptr, instance, nullptr);
        struct Tip {
            int id;
            const wchar_t* text;
        };
        const Tip hints[] = {{PIN, L"Fixar / desafixar à frente de todos os apps (P)"},
                             {OPTIONS, L"Conexão, opacidade e intervalo de coleta"},
                             {MINIMIZE, L"Recolher para a bandeja do Windows"},
                             {CLOSE, L"Encerrar o Node-Sight"},
                             {LOCAL, L"Mostrar / ocultar os recursos deste PC"}};
        for (const auto& hint : hints) {
            TOOLINFOW ti{sizeof(ti)};
            ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            ti.hwnd = w;
            ti.uId = reinterpret_cast<UINT_PTR>(GetDlgItem(w, hint.id));
            ti.lpszText = const_cast<LPWSTR>(hint.text);
            SendMessageW(tips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
        }
        return 0;
    }
    // Sample at the chosen frequency; restart an apparently connected but stale transport.
    case WM_TIMER: {
        RawSample s;
        if (config.local && localCollector.sample(s))
            localMetrics.update(s);
        const auto rs = remote.snapshot();
        if (rs.connected && monotonicSeconds() - rs.lastSample > std::max(15, config.interval * 4 + 5))
            remote.start(config);
        PROCESS_MEMORY_COUNTERS pm{sizeof(pm)};
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof(pm)))
            ownMemory = static_cast<double>(pm.WorkingSetSize) / 1048576;
        if (IsWindowVisible(w))
            InvalidateRect(w, nullptr, FALSE);
        return 0;
    }
    // Resize the backbuffer only when needed and blit one complete frame.
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT r;
        GetClientRect(w, &r);
        if (!backDC)
            backDC = CreateCompatibleDC(dc);
        if (r.right != backW || r.bottom != backH) {
            if (backBitmap) {
                SelectObject(backDC, originalBitmap);
                DeleteObject(backBitmap);
            }
            backBitmap = CreateCompatibleBitmap(dc, r.right, r.bottom);
            originalBitmap = static_cast<HBITMAP>(SelectObject(backDC, backBitmap));
            backW = r.right;
            backH = r.bottom;
        }
        paint(backDC);
        BitBlt(dc, 0, 0, r.right, r.bottom, backDC, 0, 0, SRCCOPY);
        EndPaint(w, &ps);
        return 0;
    }
    // Buffered painting covers every pixel, so suppress redundant erasure.
    case WM_ERASEBKGND:
        return 1;
    // Retain native button keyboard semantics with terminal-themed rendering.
    case WM_DRAWITEM:
        drawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lp));
        return TRUE;
    // All button, menu and tray actions share this command dispatcher.
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id == PIN)
            togglePin();
        else if (id == LOCAL || id == LOCAL_TOGGLE)
            toggleLocal();
        else if (id == OPTIONS)
            contextMenu();
        else if (id == MINIMIZE)
            ShowWindow(w, SW_HIDE);
        else if (id == CLOSE || id == QUIT)
            DestroyWindow(w);
        else if (id == SHOW)
            showWidget();
        else if (id == SETTINGS)
            openSettings();
        else if (id == RECONNECT)
            remote.start(config);
        else if (id >= INTERVAL1 && id <= INTERVAL10) {
            int v[] = {1, 2, 5, 10};
            config.interval = v[id - INTERVAL1];
            SetTimer(w, 1, config.interval * 1000, nullptr);
            localMetrics.reset();
            remote.start(config);
            config.save();
            InvalidateRect(w, nullptr, FALSE);
        } else if (id >= OPACITY70 && id <= OPACITY100) {
            int v[] = {70, 85, 94, 100};
            config.opacity = v[id - OPACITY70];
            layout();
            config.save();
        } else if (id >= SIZE80 && id <= SIZE125) {
            int v[] = {80, 100, 125};
            config.sizePercent = v[id - SIZE80];
            updateScale(GetDpiForWindow(w));
            layout();
            savePosition();
        }
        return 0;
    }
    // Keyboard shortcuts apply only while the widget owns focus.
    case WM_KEYDOWN:
        if (wp == 'P')
            togglePin();
        else if (wp == 'R')
            remote.start(config);
        else if (wp == VK_OEM_COMMA && (GetKeyState(VK_CONTROL) & 0x8000))
            openSettings();
        return 0;
    // Only the left header acts as a draggable caption; controls remain clickable.
    case WM_NCHITTEST: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(w, &p);
        if (p.y < px(51) && p.x < px(200))
            return HTCAPTION;
        return HTCLIENT;
    }
    // Right-click exposes the same options as the header and tray.
    case WM_CONTEXTMENU:
        contextMenu();
        return 0;
    // Restore on explicit double-click; open options on right-click.
    case WM_TRAY:
        if (lp == WM_LBUTTONDBLCLK)
            showWidget();
        else if (lp == WM_RBUTTONUP)
            contextMenu();
        return 0;
    // Recalculate layout when moving between monitors with different DPI.
    case WM_DPICHANGED: {
        updateScale(HIWORD(wp));
        auto r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(w, nullptr, r->left, r->top, px(WIDTH), px(logicalHeight()),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        return 0;
    }
    // Persist position after a drag, avoiding disk writes during mouse movement.
    case WM_EXITSIZEMOVE:
        savePosition();
        return 0;
    // Allow logout/shutdown; no history file needs flushing.
    case WM_QUERYENDSESSION:
        return TRUE;
    // Closing exits; the separate underscore control only hides the widget.
    case WM_CLOSE:
        DestroyWindow(w);
        return 0;
    // Remove the tray icon and join the SSH worker before exiting.
    case WM_DESTROY:
        savePosition();
        KillTimer(w, 1);
        Shell_NotifyIconW(NIM_DELETE, &tray);
        remote.stop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}
/** Collect two real SSH samples and a Windows sample, emit machine-readable JSON, and exit nonzero on
 * failure. */
int probe() {
    remote.start(config);
    const double deadline = monotonicSeconds() + 18;
    RemoteState s;
    do {
        Sleep(100);
        s = remote.snapshot();
        if (s.metrics.ratesValid)
            break;
    } while (monotonicSeconds() < deadline);
    RawSample l;
    const bool local = localCollector.sample(l);
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "{\"connected\":" << (s.connected ? "true" : "false")
        << ",\"rates_valid\":" << (s.metrics.ratesValid ? "true" : "false")
        << ",\"cpu_percent\":" << s.metrics.cpu << ",\"ram_percent\":" << s.metrics.ram
        << ",\"disk_percent\":" << s.metrics.disk << ",\"rx_bytes_sec\":" << s.metrics.rxRate
        << ",\"tx_bytes_sec\":" << s.metrics.txRate << ",\"uptime_sec\":" << s.metrics.raw.uptime
        << ",\"local_ok\":" << (local ? "true" : "false") << "}\n";
    auto str = out.str();
    DWORD n;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), str.data(), static_cast<DWORD>(str.size()), &n, nullptr);
    remote.stop();
    return s.metrics.ratesValid && local ? 0 : 1;
}
// The renderer can export its own drawing for layout QA without screen capture.
/** Export the same GDI renderer to BMP. Demo mode uses anonymous fixtures; live mode may expose local
 * identities. */
int renderPreview(const std::wstring& path, bool demo = false, bool serverOnly = false) {
    scale = 1;
    rebuildFonts();
    computer = localCollector.computerName();
    RawSample sample;
    if (demo) {
        // Override every identity before painting. No SSH process is launched in demo mode.
        demoMode = true;
        config.host = L"server.example.com";
        config.user = L"monitor";
        config.key.clear();
        config.interval = 2;
        config.local = !serverOnly;
        config.pinned = false;
        computer = L"WORKSTATION";
        RawSample server{}, pc{};
        constexpr uint64_t GiB = 1024ULL * 1024 * 1024;
        for (size_t i = 0; i < HISTORY; ++i) {
            const double wave = std::sin(static_cast<double>(i) * 0.16);
            server.total += 1000;
            server.idle += static_cast<uint64_t>(720 - 160 * wave);
            server.memTotal = 8 * GiB;
            server.memAvailable = static_cast<uint64_t>((2.8 + 0.25 * wave) * GiB);
            server.swapTotal = 4 * GiB;
            server.swapFree = server.swapTotal - 192 * 1024 * 1024;
            server.diskTotal = 128 * GiB;
            server.diskUsed = 42 * GiB;
            server.rx += static_cast<uint64_t>((0.8 + 0.6 * wave) * 1024 * 1024);
            server.tx += static_cast<uint64_t>((0.2 + 0.15 * wave) * 1024 * 1024);
            server.uptime = 86400 + 3600 + static_cast<double>(i * 2);
            server.load = 0.8 + 0.3 * wave;
            server.temperature = 48 + 3 * wave;
            server.time = static_cast<double>(i * 2);
            demoState.metrics.update(server);
            pc = server;
            pc.memTotal = 16 * GiB;
            pc.memAvailable = 9 * GiB;
            pc.diskTotal = 512 * GiB;
            pc.diskUsed = 185 * GiB;
            localMetrics.update(pc);
        }
        demoState.connected = true;
        demoState.lastSample = monotonicSeconds();
    } else {
        if (localCollector.sample(sample))
            localMetrics.update(sample);
        remote.start(config);
        const double deadline = monotonicSeconds() + 8;
        while (monotonicSeconds() < deadline)
            Sleep(100);
        if (localCollector.sample(sample))
            localMetrics.update(sample);
    }
    PROCESS_MEMORY_COUNTERS pm{sizeof(pm)};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof(pm)))
        ownMemory = static_cast<double>(pm.WorkingSetSize) / 1048576;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = WIDTH;
    bi.bmiHeader.biHeight = -logicalHeight();
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!dc || !bitmap) {
        if (bitmap)
            DeleteObject(bitmap);
        if (dc)
            DeleteDC(dc);
        remote.stop();
        return 1;
    }
    const auto old = SelectObject(dc, bitmap);
    paint(dc);
    for (const auto& b : buttonRects) {
        std::wstring label;
        switch (b.id) {
        case PIN:
            label = config.pinned ? L"[fixo]" : L"[pin]";
            break;
        case OPTIONS:
            label = L"menu";
            break;
        case MINIMIZE:
            label = L"_";
            break;
        case CLOSE:
            label = L"×";
            break;
        case LOCAL:
            label = config.local ? L"[-pc]" : L"[+pc]";
            break;
        }
        text(dc, b.x, b.y, b.w, b.h, label, 0, b.id == CLOSE ? RED : MUTED, DT_CENTER);
    }
    GdiFlush();
    BITMAPFILEHEADER file{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(bi.bmiHeader);
    const DWORD pixelBytes = WIDTH * logicalHeight() * 4;
    file.bfSize = file.bfOffBits + pixelBytes;
    HANDLE output =
        CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = false;
    DWORD written = 0;
    if (output != INVALID_HANDLE_VALUE) {
        ok = WriteFile(output, &file, sizeof(file), &written, nullptr) && written == sizeof(file) &&
             WriteFile(output, &bi.bmiHeader, sizeof(bi.bmiHeader), &written, nullptr) &&
             written == sizeof(bi.bmiHeader) && WriteFile(output, pixels, pixelBytes, &written, nullptr) &&
             written == pixelBytes;
        CloseHandle(output);
    }
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    for (auto& f : fonts) {
        if (f)
            DeleteObject(f);
        f = nullptr;
    }
    remote.stop();
    return ok ? 0 : 1;
}
/** Report the running widget's real extended styles and bounds without opening or modifying that window. */
int windowInfo() {
    HWND w = FindWindowW(L"NodeSightWidget", nullptr);
    const auto style = w ? GetWindowLongPtrW(w, GWL_EXSTYLE) : 0;
    RECT rect{};
    if (w)
        GetWindowRect(w, &rect);
    const bool tool = (style & WS_EX_TOOLWINDOW) != 0, app = (style & WS_EX_APPWINDOW) != 0;
    std::ostringstream out;
    out << "{\"found\":" << (w ? "true" : "false") << ",\"tool_window\":" << (tool ? "true" : "false")
        << ",\"app_window\":" << (app ? "true" : "false")
        << ",\"visible\":" << (w && IsWindowVisible(w) ? "true" : "false")
        << ",\"width\":" << rect.right - rect.left << ",\"height\":" << rect.bottom - rect.top << "}\n";
    const auto str = out.str();
    DWORD n;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), str.data(), static_cast<DWORD>(str.size()), &n, nullptr);
    return w && tool && !app ? 0 : 1;
}
} // namespace

/** Parse diagnostic modes first, enforce one interactive instance, register windows, and run the message
 * loop. */
int WINAPI wWinMain(HINSTANCE h, HINSTANCE, PWSTR, int) {
    instance = h;
    config.load();
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool isProbe = argc > 1 && wcscmp(argv[1], L"--probe") == 0;
    bool isWindowInfo = argc > 1 && wcscmp(argv[1], L"--window-info") == 0;
    const bool demo = argc > 1 && (wcscmp(argv[1], L"--demo-preview") == 0 ||
                                   wcscmp(argv[1], L"--demo-server-preview") == 0);
    const bool serverOnly = argc > 1 && wcscmp(argv[1], L"--demo-server-preview") == 0;
    std::wstring previewPath =
        argc > 2 && (demo || wcscmp(argv[1], L"--render-preview") == 0) ? argv[2] : L"";
    LocalFree(argv);
    if (isProbe)
        return probe();
    if (isWindowInfo)
        return windowInfo();
    if (!previewPath.empty())
        return renderPreview(previewPath, demo, serverOnly);
    // Diagnostics bypass the interactive single-instance mutex.
    HANDLE singleton = CreateMutexW(nullptr, TRUE, L"Local\\NodeSight.Desktop.v1");
    if (singleton && GetLastError() == ERROR_ALREADY_EXISTS) {
        auto existing = FindWindowW(L"NodeSightWidget", nullptr);
        if (existing) {
            ShowWindow(existing, SW_SHOW);
            SetForegroundWindow(existing);
        }
        CloseHandle(singleton);
        return 0;
    }
    InitCommonControls();
    computer = localCollector.computerName();
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    bgBrush = CreateSolidBrush(BG);
    editBrush = CreateSolidBrush(CARD);
    WNDCLASSW wc{};
    wc.hInstance = h;
    wc.lpfnWndProc = MainProc;
    wc.lpszClassName = L"NodeSightWidget";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);
    wc.lpfnWndProc = SettingsProc;
    wc.lpszClassName = L"NodeSightSettings";
    wc.hbrBackground = bgBrush;
    RegisterClassW(&wc);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    // TOOLWINDOW suppresses taskbar/Alt+Tab entries; LAYERED enables native opacity.
    HWND w = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, L"NodeSightWidget", L"Node-Sight",
                             WS_POPUP | WS_CLIPCHILDREN, work.right - WIDTH - 24, work.top + 16, WIDTH,
                             logicalHeight(), nullptr, nullptr, h, nullptr);
    if (!w) {
        if (singleton)
            CloseHandle(singleton);
        return 1;
    }
    MONITORINFO mi{sizeof(mi)};
    POINT pos{config.x, config.y};
    if (config.x != -32000 && GetMonitorInfoW(MonitorFromPoint(pos, MONITOR_DEFAULTTONEAREST), &mi)) {
        int x = std::clamp(
            config.x, static_cast<int>(mi.rcWork.left),
            std::max(static_cast<int>(mi.rcWork.left), static_cast<int>(mi.rcWork.right) - px(WIDTH)));
        int y = std::clamp(config.y, static_cast<int>(mi.rcWork.top),
                           std::max(static_cast<int>(mi.rcWork.top),
                                    static_cast<int>(mi.rcWork.bottom) - px(logicalHeight())));
        SetWindowPos(w, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    } else
        SetWindowPos(w, nullptr, work.right - px(WIDTH) - 16, work.top + 12, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    remote.start(config);
    ShowWindow(w, SW_SHOWNOACTIVATE);
    UpdateWindow(w);
    // Event-driven dispatch avoids busy polling and continuous redraw.
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (settingsWindow && IsDialogMessageW(settingsWindow, &message))
            continue;
        if (message.message == WM_KEYDOWN && !settingsWindow) {
            if (message.wParam == 'P' || message.wParam == 'R' ||
                (message.wParam == VK_OEM_COMMA && (GetKeyState(VK_CONTROL) & 0x8000))) {
                SendMessageW(w, WM_KEYDOWN, message.wParam, message.lParam);
                continue;
            }
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (backBitmap) {
        SelectObject(backDC, originalBitmap);
        DeleteObject(backBitmap);
    }
    if (backDC)
        DeleteDC(backDC);
    for (auto f : fonts)
        if (f)
            DeleteObject(f);
    DeleteObject(bgBrush);
    DeleteObject(editBrush);
    if (singleton)
        CloseHandle(singleton);
    return static_cast<int>(message.wParam);
}
