// Native Windows frontend. Shared monitoring, recording and layout contain no Win32 UI code.
#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <commctrl.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <sstream>
#include "dashboard.h"
#include "settings_win.h"

namespace {
Config config;
MonitorHub hub;
HWND widget = nullptr;
float scale = 1;
int height = 400, scroll = 0;
HFONT fonts[4]{};
HDC bufferDC = nullptr;
HBITMAP buffer = nullptr, oldBitmap = nullptr;
int bufferWidth = 0, bufferHeight = 0;
NOTIFYICONDATAW tray{};
UINT taskbarCreated = 0;
int timedExitMs = 0;
bool demo = false;
DashboardState demoState;
constexpr UINT TRAY = WM_APP + 1, SETTINGS_MESSAGE = WM_APP + 2;
enum { Settings = 100, Pin, Show, Hide, Exit, Reconnect, Record, StopRecording };

/** Convert shared RGB colors into Win32's byte order at the drawing boundary only. */
COLORREF nativeColor(Color c) {
    return RGB((c >> 16) & 255, (c >> 8) & 255, c & 255);
}
/** Scale logical layout values once at the GDI boundary. */
int px(int n) {
    return static_cast<int>(std::lround(n * scale));
}
/** Stock GDI brushes/pens avoid thousands of handle allocations at 100 ms intervals. */
class GdiCanvas final : public Canvas {
  public:
    explicit GdiCanvas(HDC context) : dc(context) {
        SetBkMode(dc, TRANSPARENT);
    }
    void fill(int x, int y, int w, int h, Color color) override {
        RECT r{px(x), px(y), px(x + w), px(y + h)};
        SetDCBrushColor(dc, nativeColor(color));
        FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
    void line(int x, int y, int xx, int yy, Color color) override {
        auto old = SelectObject(dc, GetStockObject(DC_PEN));
        SetDCPenColor(dc, nativeColor(color));
        MoveToEx(dc, px(x), px(y), nullptr);
        LineTo(dc, px(xx), px(yy));
        SelectObject(dc, old);
    }
    void text(int x, int y, int w, int h, const std::wstring& value, int font, Color color,
              bool right = false) override {
        SelectObject(dc, fonts[std::clamp(font, 0, 3)]);
        SetTextColor(dc, nativeColor(color));
        RECT r{px(x), px(y), px(x + w), px(y + h)};
        DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &r,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | (right ? DT_RIGHT : DT_LEFT));
    }

  private:
    HDC dc;
};
/** Select either a real snapshot or an explicitly anonymous documentation fixture. */
DashboardState current() {
    return demo ? demoState : hub.snapshot(config);
}
/** Clamp the viewport to the monitor, resize fonts only when needed, and retain scroll bounds. */
void resizeWidget() {
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(widget, MONITOR_DEFAULTTONEAREST), &monitor);
    float next = GetDpiForWindow(widget) / 96.0f * config.sizePercent / 100.0f;
    if (next != scale || !fonts[0]) {
        scale = next;
        if (bufferDC)
            SelectObject(bufferDC, GetStockObject(SYSTEM_FONT));
        for (auto& font : fonts) {
            if (font)
                DeleteObject(font);
            font = nullptr;
        }
        int sizes[] = {10, 12, 13, 16};
        for (int i = 0; i < 4; ++i)
            fonts[i] = CreateFontW(-px(sizes[i]), 0, 0, 0, i >= 2 ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE,
                                   FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    }
    const auto layout = dashboardLayout(current(), config);
    height =
        std::min(layout.height,
                 std::max(200, static_cast<int>((monitor.rcWork.bottom - monitor.rcWork.top - 24) / scale)));
    scroll = std::clamp(scroll, 0, std::max(0, layout.height - height));
    SetWindowPos(widget, config.pinned ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, px(WIDGET_WIDTH), px(height),
                 SWP_NOMOVE | SWP_NOACTIVATE);
    SetLayeredWindowAttributes(widget, 0, static_cast<BYTE>(config.opacity * 255 / 100), LWA_ALPHA);
    InvalidateRect(widget, nullptr, FALSE);
}
/** Apply validated drafts atomically; no repository or network publishing action is involved. */
bool applySettings(const Config& draft) {
    if (!demo && !draft.save()) {
        MessageBoxW(widget, L"Settings could not be saved. Your current session was kept.", L"Node-Sight",
                    MB_OK | MB_ICONERROR);
        return false;
    }
    config = draft;
    if (!demo)
        hub.configure(config);
    else
        demoState = demoDashboard(config);
    SetTimer(widget, 1, static_cast<UINT>(std::min(config.intervalMs, 1000)), nullptr);
    scroll = 0;
    resizeWidget();
    return true;
}
/** Open/focus the owned native editor with a draft and a shared runtime-status callback. */
void openSettings() {
    openNativeSettings(widget, config, applySettings, [] { return current(); });
}
/** Restore focus only for an explicit tray/menu/shortcut action. */
void showWidget() {
    ShowWindow(widget, SW_SHOWNORMAL);
    SetForegroundWindow(widget);
}
/** Keep monitoring and recording controls accessible when the widget is tray-hidden. */
void contextMenu() {
    auto menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, Settings, L"Settings...");
    AppendMenuW(menu, MF_STRING, Show, L"Show widget");
    AppendMenuW(menu, MF_STRING | (config.pinned ? MF_CHECKED : 0), Pin, L"Always on top");
    AppendMenuW(menu, MF_STRING, Reconnect, L"Reconnect servers");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, Record, L"Start / restart recording");
    AppendMenuW(menu, MF_STRING, StopRecording, L"Stop recording");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, Hide, L"Hide to tray");
    AppendMenuW(menu, MF_STRING, Exit, L"Quit Node-Sight");
    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(widget);
    auto command =
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, widget, nullptr);
    DestroyMenu(menu);
    if (command)
        SendMessageW(widget, WM_COMMAND, command, 0);
    PostMessageW(widget, WM_NULL, 0, 0);
}
/** Write a BMP from the shared renderer without capturing a desktop or personal window. */
int exportPreview(const std::wstring& path, bool serverOnly) {
    config = Config{};
    config.servers = {ServerProfile{}, ServerProfile{}};
    config.local = !serverOnly;
    config.detail = 1;
    demo = true;
    demoState = demoDashboard(config);
    demoState.hosts[1].profile.collapsed = true;
    scale = 1;
    int sizes[] = {10, 12, 13, 16};
    for (int i = 0; i < 4; ++i)
        fonts[i] = CreateFontW(-sizes[i], 0, 0, 0, i >= 2 ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    height = dashboardLayout(demoState, config).height;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = WIDGET_WIDTH;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap) {
        DeleteDC(dc);
        return 1;
    }
    auto old = SelectObject(dc, bitmap);
    GdiCanvas canvas(dc);
    drawDashboard(canvas, demoState, config, 0, true);
    drawFooter(canvas, demoState, height);
    GdiFlush();
    BITMAPFILEHEADER file{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
    const DWORD bytes = WIDGET_WIDTH * height * 4;
    file.bfSize = file.bfOffBits + bytes;
    auto output =
        CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = false;
    DWORD written = 0;
    if (output != INVALID_HANDLE_VALUE) {
        ok = WriteFile(output, &file, sizeof(file), &written, nullptr) &&
             WriteFile(output, &info.bmiHeader, sizeof(info.bmiHeader), &written, nullptr) &&
             WriteFile(output, pixels, bytes, &written, nullptr);
        CloseHandle(output);
    }
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    for (auto& font : fonts) {
        DeleteObject(font);
        font = nullptr;
    }
    return ok ? 0 : 1;
}
/** Probe every enabled destination concurrently, rather than reporting one server as the whole fleet. */
int probe() {
    hub.configure(config);
    auto deadline = monotonicSeconds() + 18;
    DashboardState state;
    bool ready = false;
    do {
        Sleep(50);
        hub.tick(config);
        state = hub.snapshot(config, true);
        ready = !state.hosts.empty();
        for (const auto& view : state.hosts)
            if (!view.metrics.ratesValid)
                ready = false;
        if (ready)
            break;
    } while (monotonicSeconds() < deadline);
    std::ostringstream out;
    out << "{\"enabled_servers\":" << state.activeServers << ",\"visible_nodes\":" << state.hosts.size()
        << ",\"rates_valid\":" << (ready ? "true" : "false")
        << ",\"recording\":" << (state.recording ? "true" : "false") << "}\n";
    auto text = out.str();
    DWORD n;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text.data(), static_cast<DWORD>(text.size()), &n, nullptr);
    hub.stop();
    return ready ? 0 : 1;
}
/** Report the actual native styles without creating another interactive widget. */
int windowInfo() {
    auto w = FindWindowW(L"NodeSightWidget", nullptr);
    auto style = w ? GetWindowLongPtrW(w, GWL_EXSTYLE) : 0;
    RECT r{};
    if (w)
        GetWindowRect(w, &r);
    std::ostringstream out;
    out << "{\"found\":" << (w ? "true" : "false")
        << ",\"tool_window\":" << ((style & WS_EX_TOOLWINDOW) ? "true" : "false")
        << ",\"app_window\":" << ((style & WS_EX_APPWINDOW) ? "true" : "false")
        << ",\"width\":" << r.right - r.left << ",\"height\":" << r.bottom - r.top << "}\n";
    auto text = out.str();
    DWORD n;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text.data(), static_cast<DWORD>(text.size()), &n, nullptr);
    return w && (style & WS_EX_TOOLWINDOW) && !(style & WS_EX_APPWINDOW) ? 0 : 1;
}
/** Own the native message loop; timers collect, painting renders, and shutdown joins workers. */
LRESULT CALLBACK procedure(HWND w, UINT message, WPARAM wp, LPARAM lp) {
    if (taskbarCreated && message == taskbarCreated) {
        Shell_NotifyIconW(NIM_ADD, &tray);
        return 0;
    }
    switch (message) {
    case WM_CREATE: {
        widget = w;
        resizeWidget();
        SetTimer(w, 1, static_cast<UINT>(std::min(config.intervalMs, 1000)), nullptr);
        if (timedExitMs > 0)
            SetTimer(w, 2, static_cast<UINT>(timedExitMs), nullptr);
        tray.cbSize = sizeof(tray);
        tray.hWnd = w;
        tray.uID = 1;
        tray.uFlags = NIF_MESSAGE | NIF_TIP | NIF_ICON;
        tray.uCallbackMessage = TRAY;
        tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(tray.szTip, L"Node-Sight · double-click to restore");
        Shell_NotifyIconW(NIM_ADD, &tray);
        return 0;
    }
    case WM_TIMER:
        if (wp == 2) {
            DestroyWindow(w);
            return 0;
        }
        if (!demo)
            hub.tick(config);
        if (IsWindowVisible(w))
            InvalidateRect(w, nullptr, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(w, &ps);
        RECT rect{};
        GetClientRect(w, &rect);
        if (!bufferDC)
            bufferDC = CreateCompatibleDC(dc);
        if (rect.right != bufferWidth || rect.bottom != bufferHeight) {
            if (buffer) {
                SelectObject(bufferDC, oldBitmap);
                DeleteObject(buffer);
            }
            buffer = CreateCompatibleBitmap(dc, rect.right, rect.bottom);
            oldBitmap = static_cast<HBITMAP>(SelectObject(bufferDC, buffer));
            bufferWidth = rect.right;
            bufferHeight = rect.bottom;
        }
        auto state = current();
        GdiCanvas canvas(bufferDC);
        drawDashboard(canvas, state, config, scroll, demo, height);
        drawFooter(canvas, state, height);
        BitBlt(dc, 0, 0, rect.right, rect.bottom, bufferDC, 0, 0, SRCCOPY);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONUP: {
        int x = static_cast<int>(GET_X_LPARAM(lp) / scale), y = static_cast<int>(GET_Y_LPARAM(lp) / scale);
        if (y < 36) {
            if (x >= HEADER_CLOSE)
                DestroyWindow(w);
            else if (x >= HEADER_HIDE)
                ShowWindow(w, SW_HIDE);
            else if (x >= HEADER_SETTINGS)
                openSettings();
            else if (x >= HEADER_PIN)
                SendMessageW(w, WM_COMMAND, Pin, 0);
            return 0;
        }
        if (y >= HEADER_HEIGHT && y < height - FOOTER_HEIGHT && x > WIDGET_WIDTH - 48) {
            auto layout = dashboardLayout(current(), config);
            for (const auto& item : layout.hosts)
                if (y + scroll >= item.top && y + scroll < item.top + 28) {
                    if (item.id == L"local") {
                        config.localCollapsed = !config.localCollapsed;
                        if (!demo)
                            config.save();
                        resizeWidget();
                        break;
                    }
                    for (auto& profile : config.servers)
                        if (profile.id == item.id) {
                            profile.collapsed = !profile.collapsed;
                            if (!demo)
                                config.save();
                            resizeWidget();
                        }
                    break;
                }
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        auto layout = dashboardLayout(current(), config);
        scroll = std::clamp(scroll - GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 48, 0,
                            std::max(0, layout.height - height));
        InvalidateRect(w, nullptr, FALSE);
        return 0;
    }
    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(w, &point);
        if (point.y < px(36) && point.x < px(HEADER_PIN))
            return HTCAPTION;
        return HTCLIENT;
    }
    case WM_CONTEXTMENU:
        contextMenu();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case Settings:
            openSettings();
            break;
        case Show:
            showWidget();
            break;
        case Hide:
            ShowWindow(w, SW_HIDE);
            break;
        case Exit:
            DestroyWindow(w);
            break;
        case Pin:
            config.pinned = !config.pinned;
            if (!demo)
                config.save();
            resizeWidget();
            break;
        case Reconnect:
            hub.stop();
            hub.configure(config);
            break;
        case Record:
            hub.restartRecording(config);
            break;
        case StopRecording:
            hub.stopRecording();
            break;
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == 'P')
            SendMessageW(w, WM_COMMAND, Pin, 0);
        else if (wp == 'R')
            SendMessageW(w, WM_COMMAND, Reconnect, 0);
        else if (wp == VK_OEM_COMMA && (GetKeyState(VK_CONTROL) & 0x8000))
            openSettings();
        return 0;
    case TRAY:
        if (lp == WM_LBUTTONDBLCLK)
            showWidget();
        else if (lp == WM_RBUTTONUP)
            contextMenu();
        return 0;
    case SETTINGS_MESSAGE:
        openSettings();
        return 0;
    case WM_DPICHANGED:
        resizeWidget();
        return 0;
    case WM_EXITSIZEMOVE: {
        RECT r{};
        GetWindowRect(w, &r);
        config.x = r.left;
        config.y = r.top;
        if (!demo)
            config.save();
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        KillTimer(w, 1);
        KillTimer(w, 2);
        Shell_NotifyIconW(NIM_DELETE, &tray);
        hub.stop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, message, wp, lp);
}
} // namespace

/** GUI and diagnostics deliberately share the same configuration and native collector core. */
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int count = 0;
    auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    std::wstring mode = count > 1 ? args[1] : L"", path = count > 2 ? args[2] : L"", settingsPath;
    for (int i = 1; i + 1 < count; ++i) {
        if (wcscmp(args[i], L"--config") == 0)
            settingsPath = args[i + 1];
        if (wcscmp(args[i], L"--run-for-ms") == 0)
            timedExitMs = std::clamp(_wtoi(args[i + 1]), 1000, 600000);
    }
    LocalFree(args);
    config.load(settingsPath);
    if (mode == L"--probe")
        return probe();
    if (mode == L"--window-info")
        return windowInfo();
    if ((mode == L"--demo-preview" || mode == L"--demo-server-preview") && !path.empty())
        return exportPreview(path, mode == L"--demo-server-preview");
    bool settingsPreview = (mode == L"--settings-preview" || mode == L"--recording-preview");
    HANDLE singleton =
        settingsPreview ? nullptr : CreateMutexW(nullptr, TRUE, L"Local\\NodeSight.Desktop.v2");
    if (singleton && GetLastError() == ERROR_ALREADY_EXISTS) {
        auto w = FindWindowW(L"NodeSightWidget", nullptr);
        if (w) {
            ShowWindow(w, SW_SHOWNORMAL);
            SetForegroundWindow(w);
            if (mode == L"--settings")
                PostMessageW(w, SETTINGS_MESSAGE, 0, 0);
        }
        CloseHandle(singleton);
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    InitCommonControls();
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (mode == L"--settings-demo" || settingsPreview) {
        demo = true;
        config = Config{};
        config.servers = {ServerProfile{}};
        config.logDirectory = L"./logs";
        config.syncSelected();
        demoState = demoDashboard(config);
    } else
        hub.configure(config);
    WNDCLASSW wc{};
    wc.hInstance = instance;
    wc.lpfnWndProc = procedure;
    wc.lpszClassName = L"NodeSightWidget";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    widget = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, wc.lpszClassName, L"Node-Sight", WS_POPUP,
                             work.right - 430, work.top + 20, 390, 400, nullptr, nullptr, instance, nullptr);
    if (!widget) {
        if (singleton)
            CloseHandle(singleton);
        CoUninitialize();
        return 1;
    }
    RECT r{};
    GetWindowRect(widget, &r);
    int x = config.x == -32000 ? work.right - (r.right - r.left) - 16 : config.x,
        y = config.y == -32000 ? work.top + 16 : config.y;
    POINT point{x, y};
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &monitor);
    x = std::clamp(x, static_cast<int>(monitor.rcWork.left),
                   std::max(static_cast<int>(monitor.rcWork.left),
                            static_cast<int>(monitor.rcWork.right - r.right + r.left)));
    y = std::clamp(y, static_cast<int>(monitor.rcWork.top),
                   std::max(static_cast<int>(monitor.rcWork.top),
                            static_cast<int>(monitor.rcWork.bottom - r.bottom + r.top)));
    SetWindowPos(widget, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(widget, SW_SHOWNOACTIVATE);
    if (mode == L"--settings" || mode == L"--settings-demo" || settingsPreview)
        openSettings();
    if (settingsPreview) {
        bool ok = !path.empty() && exportNativeSettings(path, mode == L"--recording-preview" ? 2 : 0);
        DestroyWindow(widget);
        if (singleton)
            CloseHandle(singleton);
        CoUninitialize();
        return ok ? 0 : 1;
    }
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (settingsMessage(&message))
            continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (buffer) {
        SelectObject(bufferDC, oldBitmap);
        DeleteObject(buffer);
    }
    if (bufferDC)
        DeleteDC(bufferDC);
    for (auto font : fonts)
        if (font)
            DeleteObject(font);
    if (singleton)
        CloseHandle(singleton);
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
