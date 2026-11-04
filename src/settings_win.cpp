// Terminal-styled native Windows settings. All editing is performed on an isolated draft.
#include "settings_win.h"
#include "dashboard.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <wincred.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <map>

namespace {
enum {
    PageServers = 200,
    PageAppearance,
    PageRecording,
    PageAbout,
    ServerList = 300,
    Add,
    Remove,
    Name,
    Host,
    User,
    Port,
    Key,
    BrowseKey,
    Enabled,
    Visible,
    Collapsed,
    Password,
    Forget,
    Detail = 400,
    Size,
    Opacity,
    Interval,
    Local,
    Pin,
    Graphs,
    Cpu,
    Ram,
    Disk,
    Network,
    Sensors,
    Swap,
    LogEnabled = 500,
    Duration,
    Forever,
    Every,
    Directory,
    BrowseDirectory,
    MaxMiB,
    Keep,
    CpuThreshold,
    RamThreshold,
    OpenFolder,
    RecordStatus
};
HWND window = nullptr;
constexpr COLORREF BG = RGB(0, 0, 0), SIDE = BG, EDIT = BG,
                   FG = RGB(222, 230, 218), MUTED = RGB(155, 164, 146), ACCENT = RGB(151, 197, 121);
constexpr COLORREF EDGES[] = {RGB(151, 197, 121), RGB(210, 201, 111),
                             RGB(160, 131, 203), RGB(102, 203, 217)};
/** Dialog navigation may request a default pushbutton style. Preserve owner drawing through focus,
 * press and keyboard transitions so Windows never paints an intermediate themed button. */
LRESULT CALLBACK terminalButton(HWND w, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (message == BM_SETSTYLE)
        wp = (wp & ~BS_TYPEMASK) | BS_OWNERDRAW;
    if (GetPropW(w, L"NodeSight.Checkbox")) {
        if (message == BM_GETCHECK)
            return GetPropW(w, L"NodeSight.Checked") ? BST_CHECKED : BST_UNCHECKED;
        if (message == BM_SETCHECK) {
            if (wp == BST_CHECKED)
                SetPropW(w, L"NodeSight.Checked", reinterpret_cast<HANDLE>(1));
            else
                RemovePropW(w, L"NodeSight.Checked");
            InvalidateRect(w, nullptr, FALSE);
            return 0;
        }
    }
    if (message == WM_NCDESTROY) {
        RemovePropW(w, L"NodeSight.Checkbox");
        RemovePropW(w, L"NodeSight.Checked");
        RemoveWindowSubclass(w, terminalButton, 1);
    }
    return DefSubclassProc(w, message, wp, lp);
}
struct Control {
    HWND handle;
    int page;
};
/** All control handles and credential drafts die with the owned settings window. */
struct Settings {
    Config draft;
    SettingsApply apply;
    std::function<DashboardState()> status;
    HWND owner = nullptr;
    int page = 0, selected = 0;
    float scale = 1;
    HFONT font = nullptr, titleFont = nullptr;
    HBRUSH background = nullptr, edit = nullptr, side = nullptr;
    std::vector<Control> controls;
    std::map<std::wstring, std::wstring> passwords;
    ~Settings() {
        for (auto& p : passwords)
            if (!p.second.empty())
                SecureZeroMemory(p.second.data(), p.second.size() * sizeof(wchar_t));
        if (font)
            DeleteObject(font);
        if (titleFont)
            DeleteObject(titleFont);
        if (side)
            DeleteObject(side);
        if (background)
            DeleteObject(background);
        if (edit)
            DeleteObject(edit);
    }
    int px(int value) const {
        return static_cast<int>(value * scale + 0.5f);
    }
    HWND control(const wchar_t* type, const std::wstring& text, int id, int x, int y, int w, int h,
                 DWORD style = 0, int tab = -1) {
        const bool button = wcscmp(type, L"BUTTON") == 0;
        const bool checkbox = button && (style & BS_TYPEMASK) == BS_AUTOCHECKBOX;
        if (button)
            style = (style & ~BS_TYPEMASK) | BS_OWNERDRAW;
        auto handle = CreateWindowExW(0, type, text.c_str(),
                                      WS_CHILD | WS_VISIBLE | (id ? WS_TABSTOP : 0) | style | (wcscmp(type, L"EDIT") == 0 ? WS_BORDER : 0), px(x), px(y),
                                      px(w), px(h), window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                      GetModuleHandleW(nullptr), nullptr);
        SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SetWindowTheme(handle, button ? L"" : L"DarkMode_Explorer", button ? L"" : nullptr);
        if (button) {
            if (checkbox)
                SetPropW(handle, L"NodeSight.Checkbox", reinterpret_cast<HANDLE>(1));
            SetWindowSubclass(handle, terminalButton, 1, 0);
        }
        controls.push_back({handle, tab});
        return handle;
    }
    void label(const std::wstring& text, int x, int y, int w = 450, int tab = -1) {
        control(L"STATIC", text, 0, x, y, w, 20, SS_LEFT, tab);
    }
    void editField(const wchar_t* title, int id, int x, int y, int w, int tab, bool numeric = false,
                   bool password = false) {
        label(title, x, y, w, tab);
        auto h = control(L"EDIT", L"", id, x, y + 24, w, 30,
                         ES_AUTOHSCROLL | (numeric ? ES_NUMBER : 0) | (password ? ES_PASSWORD : 0), tab);
        SendMessageW(h, EM_SETLIMITTEXT, password ? 512 : 2048, 0);
    }
    void check(const wchar_t* text, int id, int x, int y, int w, int tab, bool checked) {
        auto h = control(L"BUTTON", text, id, x, y, w, 25, BS_AUTOCHECKBOX, tab);
        SendMessageW(h, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    void combo(const wchar_t* title, int id, int x, int y, int w, int tab,
               const std::vector<std::wstring>& labels, int index) {
        label(title, x, y, w, tab);
        auto h = control(L"COMBOBOX", L"", id, x, y + 24, w, 240, CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL, tab);
        for (const auto& label : labels)
            SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        SendMessageW(h, CB_SETCURSEL, index, 0);
    }
    std::wstring get(int id) {
        auto h = GetDlgItem(window, id);
        int n = GetWindowTextLengthW(h);
        std::wstring s(n + 1, 0);
        GetWindowTextW(h, s.data(), n + 1);
        s.resize(n);
        return s;
    }
    void set(int id, const std::wstring& s) {
        SetDlgItemTextW(window, id, s.c_str());
    }
    bool checked(int id) {
        return SendDlgItemMessageW(window, id, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    int number(int id, int fallback) {
        try {
            return std::stoi(get(id));
        } catch (...) {
            return fallback;
        }
    }
    int choice(int id) {
        return static_cast<int>(SendDlgItemMessageW(window, id, CB_GETCURSEL, 0, 0));
    }
    /** Show only one page; navigation itself never reconnects a server or saves a password. */
    void showPage(int value) {
        SendMessageW(window, WM_SETREDRAW, FALSE, 0);
        page = value;
        for (const auto& c : controls)
            if (c.page >= 0)
                ShowWindow(c.handle, c.page == page ? SW_SHOW : SW_HIDE);
        SendMessageW(window, WM_SETREDRAW, TRUE, 0);
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
    void list() {
        auto h = GetDlgItem(window, ServerList);
        SendMessageW(h, LB_RESETCONTENT, 0, 0);
        for (const auto& p : draft.servers) {
            auto name = (p.enabled ? L"+ " : L"- ") + p.name + (p.visible ? L"" : L" [hidden]");
            SendMessageW(h, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        }
        SendMessageW(h, LB_SETCURSEL, selected, 0);
    }
    void loadProfile() {
        if (draft.servers.empty())
            return;
        selected = std::clamp(selected, 0, static_cast<int>(draft.servers.size()) - 1);
        const auto& p = draft.servers[selected];
        set(Name, p.name);
        set(Host, p.host);
        set(User, p.user);
        set(Port, std::to_wstring(p.port));
        set(Key, p.key);
        set(Password, passwords[p.id]);
        SendDlgItemMessageW(window, Enabled, BM_SETCHECK, p.enabled, 0);
        SendDlgItemMessageW(window, Visible, BM_SETCHECK, p.visible, 0);
        SendDlgItemMessageW(window, Collapsed, BM_SETCHECK, p.collapsed, 0);
    }
    /** Validate profiles before selection changes so half-entered destinations cannot escape the form. */
    bool saveProfile() {
        if (draft.servers.empty())
            return true;
        auto p = draft.servers[selected];
        p.name = get(Name);
        p.host = get(Host);
        p.user = get(User);
        p.port = number(Port, 0);
        p.key = get(Key);
        if (p.name.empty() || p.name.size() > 80 || !validConnection(p)) {
            MessageBoxW(window, L"Enter a display name, a valid SSH destination and a port from 1 to 65535.",
                        L"Check this server", MB_OK | MB_ICONWARNING);
            return false;
        }
        p.enabled = checked(Enabled);
        p.visible = checked(Visible);
        p.collapsed = checked(Collapsed);
        passwords[p.id] = get(Password);
        draft.servers[selected] = p;
        return true;
    }
    bool saveAll() {
        if (!saveProfile()) {
            showPage(0);
            return false;
        }
        draft.detail = std::clamp(choice(Detail), 0, 2);
        const int sizes[] = {80, 100, 125};
        draft.sizePercent = sizes[std::clamp(choice(Size), 0, 2)];
        const int rates[] = {100, 250, 500, 1000, 2000, 5000, 10000};
        draft.intervalMs = rates[std::clamp(choice(Interval), 0, 6)];
        draft.opacity = std::clamp(number(Opacity, 94), 45, 100);
        draft.local = checked(Local);
        draft.pinned = checked(Pin);
        draft.graphs = checked(Graphs);
        draft.showCpu = checked(Cpu);
        draft.showRam = checked(Ram);
        draft.showDisk = checked(Disk);
        draft.showNetwork = checked(Network);
        draft.showSensors = checked(Sensors);
        draft.showSwap = checked(Swap);
        draft.logEnabled = checked(LogEnabled);
        draft.logDurationMinutes = checked(Forever) ? 0 : std::clamp(number(Duration, 60), 1, 525600);
        const int intervals[] = {100, 250, 500, 1000, 5000, 10000, 60000};
        draft.logEveryMs = intervals[std::clamp(choice(Every), 0, 6)];
        draft.logMaxMiB = std::clamp(number(MaxMiB, 10), 1, 1024);
        draft.logKeep = std::clamp(number(Keep, 5), 1, 20);
        draft.spikeCpu = std::clamp(number(CpuThreshold, 80), 1, 100);
        draft.spikeRam = std::clamp(number(RamThreshold, 90), 1, 100);
        draft.logDirectory = get(Directory);
        if (draft.logDirectory.empty() || draft.logDirectory.find_first_of(L"\r\n") != std::wstring::npos) {
            showPage(2);
            MessageBoxW(window, L"Choose a valid recording folder.", L"Recording folder", MB_OK);
            return false;
        }
        for (const auto& p : draft.servers) {
            auto& secret = passwords[p.id];
            if (secret.empty())
                continue;
            auto target = p.credentialTarget();
            CREDENTIALW credential{};
            credential.Type = CRED_TYPE_GENERIC;
            credential.TargetName = target.data();
            credential.UserName = const_cast<LPWSTR>(p.user.c_str());
            credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
            credential.CredentialBlobSize = static_cast<DWORD>(secret.size() * sizeof(wchar_t));
            credential.CredentialBlob = reinterpret_cast<LPBYTE>(secret.data());
            if (!CredWriteW(&credential, 0)) {
                MessageBoxW(window, L"Windows could not save a server credential.", L"Credential Manager",
                            MB_OK | MB_ICONERROR);
                return false;
            }
            SecureZeroMemory(secret.data(), secret.size() * sizeof(wchar_t));
            secret.clear();
        }
        draft.selected = selected;
        draft.syncSelected();
        if (!apply(draft))
            return false;
        set(Password, L"");
        list();
        return true;
    }
};

/** Native folder selection changes only the draft destination; it does not start recording. */
void browseFolder(Settings& s) {
    BROWSEINFOW info{};
    info.hwndOwner = window;
    info.lpszTitle = L"Choose the local recording folder";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    auto pidl = SHBrowseForFolderW(&info);
    if (pidl) {
        wchar_t path[MAX_PATH]{};
        if (SHGetPathFromIDListW(pidl, path))
            s.set(Directory, path);
        CoTaskMemFree(pidl);
    }
}
LRESULT CALLBACK procedure(HWND w, UINT message, WPARAM wp, LPARAM lp) {
    auto* s = reinterpret_cast<Settings*>(GetWindowLongPtrW(w, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        s = static_cast<Settings*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
        window = w;
    }
    if (!s)
        return DefWindowProcW(w, message, wp, lp);
    switch (message) {
    case WM_CREATE: {
        s->scale = GetDpiForWindow(w) / 96.0f;
        s->background = CreateSolidBrush(BG);
        s->edit = CreateSolidBrush(EDIT);
        s->side = CreateSolidBrush(SIDE);
        s->font = CreateFontW(-s->px(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                              CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        s->titleFont = CreateFontW(-s->px(16), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0,
                                   0, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        auto logo = s->control(L"STATIC", L"node-sight", 0, 20, 24, 150, 32);
        SendMessageW(logo, WM_SETFONT, reinterpret_cast<WPARAM>(s->titleFont), TRUE);
        s->label(L"terminal / config", 20, 58, 140);
        const wchar_t* tabs[] = {L"1 servers", L"2 appearance", L"3 recording", L"4 about / usage"};
        for (int i = 0; i < 4; ++i)
            s->control(L"BUTTON", tabs[i], PageServers + i, 14, 108 + i * 48, 154, 40, BS_OWNERDRAW);
        auto heading = s->control(L"STATIC", L"node-sight / configuration", 0, 198, 24, 620, 34);
        SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(s->titleFont), TRUE);
        s->label(L"# connections / display / usage logs", 198, 64, 650);
        // Server page: stable aliases and visibility are separate from transport identity.
        s->control(L"LISTBOX", L"", ServerList, 198, 116, 195, 350,
                   LBS_NOTIFY | WS_VSCROLL | WS_BORDER | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS, 0);
        s->control(L"BUTTON", L"+ Add server", Add, 198, 478, 112, 30, BS_PUSHBUTTON, 0);
        s->control(L"BUTTON", L"Remove", Remove, 318, 478, 75, 30, BS_PUSHBUTTON, 0);
        s->label(L"Up to 32 server profiles", 198, 522, 195, 0);
        s->editField(L"Display name", Name, 418, 112, 410, 0);
        s->editField(L"Host / address", Host, 418, 180, 410, 0);
        s->editField(L"SSH user", User, 418, 248, 275, 0);
        s->editField(L"Port", Port, 712, 248, 116, 0, true);
        s->editField(L"Private key (empty: defaults / agent)", Key, 418, 316, 315, 0);
        s->control(L"BUTTON", L"Browse", BrowseKey, 746, 340, 82, 30, 0, 0);
        s->check(L"Monitor this server", Enabled, 418, 386, 400, 0, true);
        s->check(L"Show in widget (hidden nodes can keep recording)", Visible, 418, 414, 410, 0, true);
        s->check(L"Start with this node collapsed", Collapsed, 418, 442, 400, 0, false);
        s->editField(L"Optional password (empty: keep saved credential)", Password, 418, 480, 410, 0, false,
                     true);
        s->control(L"BUTTON", L"Forget saved password", Forget, 418, 545, 190, 27, 0, 0);
        // Appearance page: the same preferences are consumed by both native frontends.
        s->combo(L"Information density", Detail, 198, 116, 280, 1,
                 {L"Minimal · node summary", L"Balanced · rates and graphs",
                  L"Detailed · capacities / swap"},
                 s->draft.detail);
        s->combo(L"Widget size", Size, 510, 116, 318, 1,
                 {L"80% · mini", L"100% · compact", L"125% · enlarged"},
                 s->draft.sizePercent < 90    ? 0
                 : s->draft.sizePercent > 110 ? 2
                                              : 1);
        int intervals[] = {100, 250, 500, 1000, 2000, 5000, 10000}, selected = 4;
        for (int i = 0; i < 7; ++i)
            if (s->draft.intervalMs == intervals[i])
                selected = i;
        s->combo(L"Collection interval", Interval, 198, 196, 280, 1,
                 {L"100 ms · 10 samples/s", L"250 ms · 4 samples/s", L"500 ms · 2 samples/s", L"1 second",
                  L"2 seconds", L"5 seconds", L"10 seconds"},
                 selected);
        s->editField(L"Opacity (45–100%)", Opacity, 510, 196, 170, 1, true);
        s->set(Opacity, std::to_wstring(s->draft.opacity));
        s->label(L"100 ms increases CPU, SSH traffic and remote collector work per server.", 198, 267, 630,
                 1);
        s->check(L"Keep above other apps", Pin, 198, 307, 280, 1, s->draft.pinned);
        s->check(L"Monitor this computer", Local, 510, 307, 318, 1, s->draft.local);
        s->check(L"Draw history graphs", Graphs, 198, 345, 280, 1, s->draft.graphs);
        s->label(L"[ metrics to display ]", 198, 402, 600, 1);
        s->check(L"CPU", Cpu, 198, 435, 170, 1, s->draft.showCpu);
        s->check(L"Memory", Ram, 414, 435, 180, 1, s->draft.showRam);
        s->check(L"Disk capacity", Disk, 630, 435, 198, 1, s->draft.showDisk);
        s->check(L"Network rates", Network, 198, 475, 190, 1, s->draft.showNetwork);
        s->check(L"Sensors / uptime", Sensors, 414, 475, 200, 1, s->draft.showSensors);
        s->check(L"Swap", Swap, 630, 475, 180, 1, s->draft.showSwap);
        s->label(L"Detailed adds swap and peak rows. Hidden metrics still reach recordings.", 198, 533,
                 630, 1);
        // Recording page separates collection rate, summary cadence and bounded retention.
        s->check(L"Record automatically while the app is running", LogEnabled, 198, 112, 630, 2,
                 s->draft.logEnabled);
        s->editField(L"Duration (minutes)", Duration, 198, 158, 170, 2, true);
        s->set(Duration, std::to_wstring(std::max(1, s->draft.logDurationMinutes)));
        s->check(L"Continuous / no deadline", Forever, 390, 183, 260, 2, s->draft.logDurationMinutes == 0);
        s->combo(L"Write summaries every", Every, 198, 233, 280, 2,
                 {L"100 ms", L"250 ms", L"500 ms", L"1 second", L"5 seconds", L"10 seconds", L"60 seconds"},
                 3);
        int every[] = {100, 250, 500, 1000, 5000, 10000, 60000};
        for (int i = 0; i < 7; ++i)
            if (s->draft.logEveryMs == every[i])
                SendDlgItemMessageW(w, Every, CB_SETCURSEL, i, 0);
        s->editField(L"Rotate at (MiB)", MaxMiB, 510, 233, 145, 2, true);
        s->set(MaxMiB, std::to_wstring(s->draft.logMaxMiB));
        s->editField(L"Archives to keep", Keep, 680, 233, 148, 2, true);
        s->set(Keep, std::to_wstring(s->draft.logKeep));
        s->editField(L"Local recording folder", Directory, 198, 311, 515, 2);
        s->set(Directory, s->draft.logDirectory);
        s->control(L"BUTTON", L"Browse", BrowseDirectory, 729, 335, 99, 30, 0, 2);
        s->editField(L"CPU spike threshold (%)", CpuThreshold, 198, 390, 280, 2, true);
        s->set(CpuThreshold, std::to_wstring(s->draft.spikeCpu));
        s->editField(L"RAM spike threshold (%)", RamThreshold, 510, 390, 318, 2, true);
        s->set(RamThreshold, std::to_wstring(s->draft.spikeRam));
        s->label(L"Every received sample contributes to peaks. Summaries flush at least once a second.", 198,
                 467, 630, 2);
        s->control(L"STATIC", L"", RecordStatus, 198, 498, 630, 35, SS_LEFT, 2);
        s->control(L"BUTTON", L"Open log folder", OpenFolder, 198, 544, 160, 29, 0, 2);
        // About page gives honest process accounting instead of mixing SSH memory into the app value.
        auto about =
            s->control(L"STATIC", L"Native clients. Shared telemetry core.", 0, 198, 120, 620, 32, 0, 3);
        SendMessageW(about, WM_SETFONT, reinterpret_cast<WPARAM>(s->titleFont), TRUE);
        s->label(L"Windows: Win32 / GDI    ·    Linux: GTK 3 / Cairo", 198, 174, 630, 3);
        s->label(L"One persistent system OpenSSH process per enabled server. No remote agent.", 198, 215, 630,
                 3);
        s->label(L"The footer reports this app's RSS and CPU; each SSH process costs extra memory.", 198, 258,
                 630, 3);
        s->label(L"Logs stay local. Their aliases and resource values may be sensitive when shared.", 198,
                 301, 630, 3);
        s->label(L"CPU is normalized over all logical processors. Peak RAM resets on app restart.", 198, 344,
                 630, 3);
        s->label(L"Node-Sight 2.0 development · MIT license", 198, 420, 630, 3);
        s->label(L"alt+1..4 pages", 22, 526, 140);
        s->label(L"tab   navigate", 22, 550, 140);
        s->label(L"# changes stay on this device", 198, 606, 370);
        s->control(L"BUTTON", L"Apply changes", IDOK, 586, 598, 150, 36, BS_OWNERDRAW);
        s->control(L"BUTTON", L"Close", IDCANCEL, 748, 598, 80, 36, BS_OWNERDRAW);
        s->list();
        s->loadProfile();
        s->showPage(0);
        SetTimer(w, 1, 1000, nullptr);
        return 0;
    }
    case WM_TIMER: {
        auto state = s->status();
        s->set(RecordStatus, state.logStatus + L"\r\n" + state.logPath);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        // Owner-drawn checkboxes retain Space/click activation and the existing BM_* state API.
        auto source = reinterpret_cast<HWND>(lp);
        if (HIWORD(wp) == BN_CLICKED && source && GetPropW(source, L"NodeSight.Checkbox"))
            SendMessageW(source, BM_SETCHECK, SendMessageW(source, BM_GETCHECK, 0, 0) == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED, 0);
        if (id >= PageServers && id <= PageAbout) {
            s->showPage(id - PageServers);
            return 0;
        }
        if (id == ServerList && HIWORD(wp) == LBN_SELCHANGE) {
            int next = static_cast<int>(SendDlgItemMessageW(w, ServerList, LB_GETCURSEL, 0, 0));
            if (next >= 0 && s->saveProfile()) {
                s->selected = next;
                s->loadProfile();
            } else
                SendDlgItemMessageW(w, ServerList, LB_SETCURSEL, s->selected, 0);
            return 0;
        }
        if (id == Add) {
            if (s->draft.servers.size() < 32 && s->saveProfile()) {
                ServerProfile profile;
                profile.id = newProfileId();
                profile.name = L"Server " + std::to_wstring(s->draft.servers.size() + 1);
                s->draft.servers.push_back(profile);
                s->selected = static_cast<int>(s->draft.servers.size()) - 1;
                s->list();
                s->loadProfile();
            }
            return 0;
        }
        if (id == Remove) {
            if (s->draft.servers.size() > 1) {
                s->draft.servers.erase(s->draft.servers.begin() + s->selected);
                s->selected = std::max(0, s->selected - 1);
                s->list();
                s->loadProfile();
            } else
                MessageBoxW(w, L"Keep one profile and disable monitoring if you do not need it.",
                            L"Server profiles", MB_OK);
            return 0;
        }
        if (id == BrowseKey) {
            wchar_t path[4096]{};
            OPENFILENAMEW info{sizeof(info)};
            info.hwndOwner = w;
            info.lpstrFile = path;
            info.nMaxFile = 4096;
            info.lpstrTitle = L"Select an SSH private key";
            info.lpstrFilter = L"All files\0*.*\0";
            info.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&info))
                s->set(Key, path);
            return 0;
        }
        if (id == Forget) {
            if (s->saveProfile()) {
                const auto target = s->draft.servers[s->selected].credentialTarget();
                CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0);
                s->set(Password, L"");
                s->passwords[s->draft.servers[s->selected].id].clear();
            }
            return 0;
        }
        if (id == BrowseDirectory) {
            browseFolder(*s);
            return 0;
        }
        if (id == OpenFolder) {
            auto folder = s->get(Directory);
            ShellExecuteW(w, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        if (id == Forever) {
            EnableWindow(GetDlgItem(w, Duration), !s->checked(Forever));
            return 0;
        }
        if (id == IDOK) {
            s->saveAll();
            return 0;
        }
        if (id == IDCANCEL) {
            DestroyWindow(w);
            return 0;
        }
        break;
    }
    case WM_MEASUREITEM: {
        auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        if (item->CtlID == ServerList) {
            item->itemHeight = s->px(27);
            return TRUE;
        }
        if (item->CtlType == ODT_COMBOBOX) {
            item->itemHeight = s->px(21);
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (draw->CtlID == ServerList) {
            if (draw->itemID == static_cast<UINT>(-1))
                return TRUE;
            wchar_t name[512]{};
            SendMessageW(draw->hwndItem, LB_GETTEXT, draw->itemID, reinterpret_cast<LPARAM>(name));
            bool active = (draw->itemState & ODS_SELECTED) != 0;
            SetDCBrushColor(draw->hDC, active ? RGB(13, 21, 10) : EDIT);
            FillRect(draw->hDC, &draw->rcItem, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            SelectObject(draw->hDC, s->font);
            SetTextColor(draw->hDC, active ? ACCENT : FG);
            SetBkMode(draw->hDC, TRANSPARENT);
            RECT r = draw->rcItem;
            r.left += s->px(10);
            DrawTextW(draw->hDC, name, -1, &r,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            return TRUE;
        }
        if (draw->CtlType == ODT_COMBOBOX) {
            SetDCBrushColor(draw->hDC, BG);
            FillRect(draw->hDC, &draw->rcItem, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            if (draw->itemID != static_cast<UINT>(-1)) {
                wchar_t label[512]{};
                SendMessageW(draw->hwndItem, CB_GETLBTEXT, draw->itemID, reinterpret_cast<LPARAM>(label));
                SelectObject(draw->hDC, s->font);
                SetBkMode(draw->hDC, TRANSPARENT);
                SetTextColor(draw->hDC, draw->itemState & ODS_SELECTED ? ACCENT : FG);
                RECT r = draw->rcItem;
                r.left += s->px(5);
                DrawTextW(draw->hDC, label, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
            return TRUE;
        }
        bool selected = draw->CtlID == IDOK || (draw->CtlID >= PageServers && draw->CtlID <= PageAbout &&
                                                static_cast<int>(draw->CtlID) - PageServers == s->page);
        SetDCBrushColor(draw->hDC, BG);
        FillRect(draw->hDC, &draw->rcItem, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        wchar_t label[128]{};
        GetWindowTextW(draw->hwndItem, label, 128);
        SelectObject(draw->hDC, s->font);
        SetTextColor(draw->hDC, selected ? ACCENT : FG);
        SetBkMode(draw->hDC, TRANSPARENT);
        RECT r = draw->rcItem;
        if (GetPropW(draw->hwndItem, L"NodeSight.Checkbox")) {
            bool checked = SendMessageW(draw->hwndItem, BM_GETCHECK, 0, 0) == BST_CHECKED;
            std::wstring value = (checked ? L"[x] " : L"[ ] ") + std::wstring(label);
            SetTextColor(draw->hDC, checked ? ACCENT : MUTED);
            DrawTextW(draw->hDC, value.c_str(), -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            if (draw->itemState & ODS_FOCUS)
                DrawFocusRect(draw->hDC, &r);
            return TRUE;
        }
        bool navigation = draw->CtlID >= PageServers && draw->CtlID <= PageAbout;
        COLORREF edge = navigation ? EDGES[draw->CtlID - PageServers] : ACCENT;
        auto oldPen = SelectObject(draw->hDC, GetStockObject(DC_PEN));
        auto oldBrush = SelectObject(draw->hDC, GetStockObject(HOLLOW_BRUSH));
        SetDCPenColor(draw->hDC, selected ? edge : RGB(43, 47, 42));
        Rectangle(draw->hDC, r.left, r.top, r.right, r.bottom);
        SelectObject(draw->hDC, oldBrush);
        SelectObject(draw->hDC, oldPen);
        std::wstring command = L"[ " + std::wstring(label) + L" ]";
        SetTextColor(draw->hDC, selected ? edge : MUTED);
        DrawTextW(draw->hDC, command.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (draw->itemState & ODS_FOCUS)
            DrawFocusRect(draw->hDC, &r);
        return TRUE;
    }
    case WM_CTLCOLORSTATIC: {
        RECT rectangle{};
        GetWindowRect(reinterpret_cast<HWND>(lp), &rectangle);
        POINT point{rectangle.left, rectangle.top};
        ScreenToClient(w, &point);
        bool left = point.x < s->px(182);
        SetTextColor(reinterpret_cast<HDC>(wp), left ? MUTED : FG);
        SetBkColor(reinterpret_cast<HDC>(wp), BG);
        return reinterpret_cast<LRESULT>(left ? s->side : s->background);
    }
    case WM_CTLCOLORBTN:
        SetTextColor(reinterpret_cast<HDC>(wp), FG);
        SetBkColor(reinterpret_cast<HDC>(wp), BG);
        return reinterpret_cast<LRESULT>(s->background);
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor(reinterpret_cast<HDC>(wp), FG);
        SetBkColor(reinterpret_cast<HDC>(wp), EDIT);
        return reinterpret_cast<LRESULT>(s->edit);
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(w, &r);
        FillRect(reinterpret_cast<HDC>(wp), &r, s->background);
        r.right = s->px(182);
        SetDCBrushColor(reinterpret_cast<HDC>(wp), SIDE);
        FillRect(reinterpret_cast<HDC>(wp), &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        return 1;
    }
    case WM_PAINT:
    case WM_PRINTCLIENT: {
        PAINTSTRUCT ps{};
        HDC dc = message == WM_PAINT ? BeginPaint(w, &ps) : reinterpret_cast<HDC>(wp);
        RECT client{};
        GetClientRect(w, &client);
        FillRect(dc, &client, s->background);
        SelectObject(dc, s->font);
        SetBkMode(dc, TRANSPARENT);
        auto pen = SelectObject(dc, GetStockObject(DC_PEN));
        auto brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        // Labels interrupt the thin top rules just like the widget's metric panes.
        auto box = [&](int x, int y, int width, int height, const wchar_t* title, COLORREF ink) {
            SetDCPenColor(dc, ink);
            Rectangle(dc, s->px(x), s->px(y), s->px(x + width), s->px(y + height));
            SIZE size{};
            GetTextExtentPoint32W(dc, title, lstrlenW(title), &size);
            RECT label{s->px(x + 10), s->px(y - 9), s->px(x + 22) + size.cx, s->px(y + 9)};
            FillRect(dc, &label, s->background);
            label.left += s->px(5);
            SetTextColor(dc, ink);
            DrawTextW(dc, title, -1, &label, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        };
        box(9, 94, 164, 488, L"menu", RGB(101, 109, 96));
        const wchar_t* titles[] = {L"1 servers", L"2 appearance", L"3 recording", L"4 about / usage"};
        box(188, 94, 650, 488, titles[s->page], EDGES[s->page]);
        SelectObject(dc, brush);
        SelectObject(dc, pen);
        if (message == WM_PAINT)
            EndPaint(w, &ps);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        KillTimer(w, 1);
        window = nullptr;
        SetWindowLongPtrW(w, GWLP_USERDATA, 0);
        delete s;
        return 0;
    }
    return DefWindowProcW(w, message, wp, lp);
}
} // namespace

void openNativeSettings(HWND owner, const Config& config, SettingsApply apply,
                        std::function<DashboardState()> status) {
    if (window) {
        ShowWindow(window, SW_SHOW);
        SetForegroundWindow(window);
        return;
    }
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = procedure;
    wc.lpszClassName = L"NodeSightSettingsV2";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    auto* s = new Settings;
    s->draft = config;
    s->apply = std::move(apply);
    s->status = std::move(status);
    s->owner = owner;
    s->selected = config.selected;
    UINT dpi = GetDpiForWindow(owner);
    int width = MulDiv(870, dpi, 96), height = MulDiv(690, dpi, 96);
    RECT area{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    window = CreateWindowExW(
        WS_EX_DLGMODALFRAME, wc.lpszClassName, L"Node-Sight · Settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, area.left + (area.right - area.left - width) / 2,
        area.top + (area.bottom - area.top - height) / 2, width, height, owner, nullptr, wc.hInstance, s);
    if (!window) {
        delete s;
        return;
    }
    BOOL dark = TRUE;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    DwmSetWindowAttribute(window, 35, &BG, sizeof(BG));
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
}
bool settingsMessage(MSG* message) {
    if (window && message->message == WM_SYSKEYDOWN && message->wParam >= '1' && message->wParam <= '4') {
        SendMessageW(window, WM_COMMAND, PageServers + message->wParam - '1', 0);
        return true;
    }
    return window && IsDialogMessageW(window, message);
}

/** Print native controls into an offscreen bitmap for documentation and layout QA. */
bool exportNativeSettings(const std::wstring& path, int page) {
    if (!window)
        return false;
    auto* settings = reinterpret_cast<Settings*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    // Exercise dialog-manager style requests and checkbox activation in anonymous export mode.
    // A native default-button repaint or a lost checkbox state makes the diagnostic fail.
    for (const auto& control : settings->controls) {
        wchar_t type[32]{};
        GetClassNameW(control.handle, type, 32);
        if (wcscmp(type, L"Button") != 0)
            continue;
        SendMessageW(control.handle, BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);
        if ((GetWindowLongPtrW(control.handle, GWL_STYLE) & BS_TYPEMASK) != BS_OWNERDRAW)
            return false;
        if (GetPropW(control.handle, L"NodeSight.Checkbox")) {
            auto before = SendMessageW(control.handle, BM_GETCHECK, 0, 0);
            SendMessageW(control.handle, BM_CLICK, 0, 0);
            auto toggled = SendMessageW(control.handle, BM_GETCHECK, 0, 0);
            SendMessageW(control.handle, BM_CLICK, 0, 0);
            if (before == toggled || SendMessageW(control.handle, BM_GETCHECK, 0, 0) != before)
                return false;
        }
    }
    settings->showPage(std::clamp(page, 0, 3));
    UpdateWindow(window);
    RECT r{};
    GetClientRect(window, &r);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = r.right;
    info.bmiHeader.biHeight = -r.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    auto dc = CreateCompatibleDC(nullptr);
    auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap) {
        DeleteDC(dc);
        return false;
    }
    auto old = SelectObject(dc, bitmap);
    PrintWindow(window, dc, PW_CLIENTONLY);
    GdiFlush();
    BITMAPFILEHEADER file{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
    DWORD bytes = r.right * r.bottom * 4;
    file.bfSize = file.bfOffBits + bytes;
    auto output =
        CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    bool ok = false;
    if (output != INVALID_HANDLE_VALUE) {
        ok = WriteFile(output, &file, sizeof(file), &written, nullptr) &&
             WriteFile(output, &info.bmiHeader, sizeof(info.bmiHeader), &written, nullptr) &&
             WriteFile(output, pixels, bytes, &written, nullptr);
        CloseHandle(output);
    }
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return ok;
}
