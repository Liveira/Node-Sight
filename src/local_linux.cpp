// Native Linux counters: /proc reads and statvfs only; no shell polling is involved.
#include "metrics.h"
#include "config.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

/** Read the system view of Linux resources; cgroup/container limits are not substituted. */
bool LocalCollector::sample(RawSample& s) {
    s = {};
    s.temperature = -1;
    s.time = monotonicSeconds();
    std::ifstream stat("/proc/stat");
    std::string label;
    uint64_t values[8]{};
    if (!(stat >> label) || label != "cpu")
        return false;
    for (auto& value : values) {
        if (!(stat >> value))
            return false;
        s.total += value;
    }
    s.idle = values[3] + values[4];
    std::ifstream mem("/proc/meminfo");
    std::string line;
    while (std::getline(mem, line)) {
        std::istringstream row(line);
        uint64_t n = 0;
        row >> label >> n;
        if (label == "MemTotal:")
            s.memTotal = n * 1024;
        else if (label == "MemAvailable:")
            s.memAvailable = n * 1024;
        else if (label == "SwapTotal:")
            s.swapTotal = n * 1024;
        else if (label == "SwapFree:")
            s.swapFree = n * 1024;
    }
    std::ifstream("/proc/uptime") >> s.uptime;
    std::ifstream("/proc/loadavg") >> s.load;
    struct statvfs disk{};
    if (statvfs("/", &disk) == 0) {
        s.diskTotal = disk.f_blocks * disk.f_frsize;
        s.diskUsed = (disk.f_blocks - disk.f_bfree) * disk.f_frsize;
    }
    std::string iface;
    std::ifstream routes("/proc/net/route");
    while (std::getline(routes, line)) {
        std::istringstream row(line);
        std::string destination;
        row >> label >> destination;
        if (destination == "00000000") {
            iface = label;
            break;
        }
    }
    std::ifstream net("/proc/net/dev");
    while (std::getline(net, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        auto name = line.substr(0, colon);
        name.erase(std::remove(name.begin(), name.end(), ' '), name.end());
        if ((!iface.empty() && name != iface) || (iface.empty() && name == "lo"))
            continue;
        std::istringstream row(line.substr(colon + 1));
        uint64_t n[16]{};
        for (auto& v : n)
            row >> v;
        s.rx += n[0];
        s.tx += n[8];
    }
    // Discover once; fast collection does not repeatedly scan missing sysfs zones.
    static const std::string thermalPath = [] {
        std::string fallback;
        for (int zone = 0; zone < 32; ++zone) {
            auto base = "/sys/class/thermal/thermal_zone" + std::to_string(zone);
            double value = 0;
            if (!(std::ifstream(base + "/temp") >> value))
                continue;
            if (fallback.empty())
                fallback = base + "/temp";
            std::string type;
            std::ifstream(base + "/type") >> type;
            if (type == "x86_pkg_temp" || type == "cpu-thermal" || type == "cpu_thermal")
                return base + "/temp";
        }
        return fallback;
    }();
    double temperature = 0;
    if (!thermalPath.empty() && (std::ifstream(thermalPath) >> temperature))
        s.temperature = temperature / 1000;
    return s.memTotal > 0;
}
/** Read a local display label only; anonymous exports never call this path. */
std::wstring LocalCollector::computerName() const {
    char name[256]{};
    return gethostname(name, sizeof(name) - 1) == 0 ? utf8ToWide(name) : L"Linux PC";
}
/** The current Linux collector measures the root filesystem. */
std::wstring LocalCollector::drive() const {
    return L"/";
}
/** Read this process's resident pages and cumulative CPU without polling shell tools. */
ProcessCounters processCounters() {
    ProcessCounters result;
    unsigned long pages = 0, resident = 0;
    std::ifstream("/proc/self/statm") >> pages >> resident;
    result.residentBytes = static_cast<uint64_t>(resident) * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
    timespec cpu{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu);
    result.cpuSeconds = cpu.tv_sec + cpu.tv_nsec / 1e9;
    result.logicalProcessors = static_cast<unsigned>(std::max(1L, sysconf(_SC_NPROCESSORS_ONLN)));
    return result;
}
