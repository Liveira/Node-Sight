// Windows native counters and process accounting.
#include <winsock2.h>
#include <ws2tcpip.h>
#include "metrics.h"
#include <iphlpapi.h>
#include <netioapi.h>
#include <psapi.h>
/** Read this process's resident pages and cumulative CPU without walking other processes. */
ProcessCounters processCounters() {
    ProcessCounters result;
    PROCESS_MEMORY_COUNTERS pm{sizeof(pm)};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof(pm)))
        result.residentBytes = pm.WorkingSetSize;
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        ULARGE_INTEGER k{}, u{};
        k.LowPart = kernel.dwLowDateTime;
        k.HighPart = kernel.dwHighDateTime;
        u.LowPart = user.dwLowDateTime;
        u.HighPart = user.dwHighDateTime;
        result.cpuSeconds = static_cast<double>(k.QuadPart + u.QuadPart) / 10000000.0;
    }
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    result.logicalProcessors = info.dwNumberOfProcessors;
    return result;
}
/** Combine the two FILETIME halves without truncating the 64-bit CPU time counters. */
static uint64_t ft(FILETIME f) {
    return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}
/** Read real Windows CPU, physical RAM, system-volume space and interface counters without WMI or
 * subprocesses. */
bool LocalCollector::sample(RawSample& s) {
    FILETIME idle{}, kernel{}, user{};
    MEMORYSTATUSEX mem{sizeof(mem)};
    if (!GetSystemTimes(&idle, &kernel, &user) || !GlobalMemoryStatusEx(&mem))
        return false;
    s = {};
    s.swapKnown = false;
    s.temperature = -1;
    s.total = ft(kernel) + ft(user);
    s.idle = ft(idle);
    s.memTotal = mem.ullTotalPhys;
    s.memAvailable = mem.ullAvailPhys;
    s.uptime = static_cast<double>(GetTickCount64()) / 1000.0;
    s.time = monotonicSeconds();
    ULARGE_INTEGER avail{}, total{}, free{};
    if (GetDiskFreeSpaceExW(drive().c_str(), &avail, &total, &free)) {
        s.diskTotal = total.QuadPart;
        s.diskUsed = total.QuadPart - free.QuadPart;
    }
    // Prefer physical interfaces to avoid double-counting virtual bridge traffic.
    MIB_IF_TABLE2* table = nullptr;
    if (GetIfTable2(&table) == NO_ERROR) {
        bool physical = false;
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            const auto& r = table->Table[i];
            if (r.OperStatus == IfOperStatusUp && r.InterfaceAndOperStatusFlags.HardwareInterface)
                physical = true;
        }
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            const auto& r = table->Table[i];
            if (r.OperStatus != IfOperStatusUp || r.Type == IF_TYPE_SOFTWARE_LOOPBACK ||
                r.Type == IF_TYPE_TUNNEL)
                continue;
            if (physical && !r.InterfaceAndOperStatusFlags.HardwareInterface)
                continue;
            s.rx += r.InOctets;
            s.tx += r.OutOctets;
        }
        // Release API-owned interface storage with its matching allocator.
        FreeMibTable(table);
    }
    return true;
}
/** Read the Windows computer name for local display only; anonymous demo mode overrides it before export. */
std::wstring LocalCollector::computerName() const {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD len = MAX_COMPUTERNAME_LENGTH + 1;
    return GetComputerNameW(name, &len) ? name : L"Windows";
}
/** Resolve the Windows installation volume rather than assuming that the OS always lives on C:. */
std::wstring LocalCollector::drive() const {
    wchar_t path[MAX_PATH]{};
    GetWindowsDirectoryW(path, MAX_PATH);
    if (path[1] == L':')
        return std::wstring(path, 2) + L"\\";
    return L"C:\\";
}
