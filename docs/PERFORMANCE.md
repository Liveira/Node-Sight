# Performance — 2.0 development and historical baseline

## 2.0 development baseline

These runs predate the restored terminal layout and compact PC overview. They remain a development baseline; the revised renderer has not been benchmarked again.

The Windows native widget was measured on the same Ryzen 7 5800X / 16-logical-processor / approximately 16 GiB workstation. Each scenario had **5 seconds of warm-up and 20 one-second observations** (about 20.3 seconds elapsed), followed by orderly timed GUI shutdown. These are short validation runs, not a sustained leak or scalability study.

| Windows development scenario | App avg. RSS | SSH processes | Sum avg. working set | Sum private memory | Sum CPU |
| --- | ---: | ---: | ---: | ---: | ---: |
| 2 s · one SSH profile · recording off | 17.498 MiB | 1 | **29.006 MiB** | 5.719 MiB | 0.01440% |
| 100 ms · two SSH profiles · one hidden · recording on | 16.039 MiB | 2 | **39.043 MiB** | 9.073 MiB | 0.25929% |

Raw reports: [2-second single profile](benchmarks/v2-default-2s.json), [100-ms dual sessions](benchmarks/v2-fast-100ms-two-sessions.json). Each includes the exact non-sensitive visual/sampling preferences and separate process rows. Both used the real visible Windows widget, the local-PC panel, Balanced density, 100% zoom and 100% opacity. The fast scenario had one remote card hidden while it remained recorded.

The two sessions connect independently to the **same reachable Linux target**. They verify concurrency, queue/recording behavior and resource accounting; they do not test a fleet of 32 distinct servers. The final fast session generated 84 SAMPLE records across its enabled nodes and ended with a normal shutdown marker. Peak receipt timestamps and records from the hidden node were checked. Three additional dual-session startup/shutdown cycles passed after fixing cross-profile Windows pipe inheritance.

CPU is normalized over all 16 logical processors. The sum includes the app and its direct SSH children; shared resident pages may be counted more than once. The fast scenario changes interval, connection count and recording simultaneously, so the difference is not an isolated estimate of interval cost. Background development/desktop activity was present. Linux GTK memory is **not** represented by the Windows values.

Windows metric/configuration/recording tests passed. Linux built with GCC 12.2 / GTK 3.24.38 on Debian 12 in an isolated container, passed three CTest suites, and exercised real local counters plus native widget/settings windows under Xvfb. This validates the native Linux build and UI lifecycle, not every Wayland compositor or a graphical desktop on physical hardware.

For longer measurements use `tools/measure.ps1 -Seconds 60` or more after warm-up. Pass `-AppProcessId` and `-SettingsPath` for an isolated preferences file; the report records only an allowlist of non-sensitive settings. Logging performance depends on the filesystem, cadence, thresholds and node count. Native Linux process/desktop overhead requires its own benchmark on the intended machine.

---

## Historical 1.0 measurements

Node-Sight prioritizes a small native footprint. These are **observations on one workstation**, not cross-machine guarantees or comparisons against other applications.

### Test environment

| Item | Value |
| --- | --- |
| Measurement date | November 2, 2026 |
| Client OS | Windows 11 Pro, build 28000 |
| Processor | AMD Ryzen 7 5800X, 8 cores / 16 logical processors |
| Physical memory | Approximately 16 GiB installed |
| Build | MSVC x64 Release, static CRT, size optimization and link-time optimization |
| Transport | One persistent system OpenSSH process connected to a local-network Linux host |
| Desktop | Normal interactive desktop with background applications; not a controlled idle lab |
| Warm-up | At least 5 seconds before each measured scenario |
| Measurement | 60 samples, once per second, approximately 61 seconds wall time per scenario |
| Transparency | 100% opacity during these runs; no forced working-set trimming |

Hostnames, addresses, user names, keys and process IDs are excluded from the public reports. Hardware/OS information above describes only the benchmark environment.

### Results

| Scenario | Interval | Local panel | Scale | Widget avg. WS | SSH avg. WS | Sum avg. WS | Sum avg. private | Sum CPU |
| --- | ---: | :---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Fast | 1 s | On | 125% | 15.590 MiB | 11.484 MiB | **27.074 MiB** | 5.299 MiB | 0.04649% |
| Default | 2 s | On | 100% | 15.573 MiB | 11.492 MiB | **27.065 MiB** | 5.305 MiB | 0.01122% |
| Eco | 5 s | Off | 80% | 15.596 MiB | 11.496 MiB | **27.092 MiB** | 5.408 MiB | 0.00481% |

Raw reports: [fast](benchmarks/fast-1s.json), [default](benchmarks/default-2s.json), [eco](benchmarks/eco-5s.json).

The slower scenario used less CPU in this run. **RAM remained effectively unchanged**, so this dataset does not support a meaningful RAM-saving claim from hiding the PC panel or increasing the interval. Each scenario changes more than one setting; the comparison does not isolate the effect of any single setting. The first scenario also ran while documentation tooling was active.

### What the numbers mean

- **Working set (WS):** resident pages reported by Windows for each process, including shared DLL pages and graphics-related mappings. Adding two working sets can count shared pages twice. The sum is a transparent process-accounting figure, not unique physical RAM.
- **Private memory:** `PrivateMemorySize64`, which tracks private committed memory. It is not the same as resident physical RAM.
- **CPU:** delta in cumulative process CPU seconds, divided by actual elapsed seconds and all 16 logical processors, multiplied by 100. It measures the complete interval, not a single Task Manager refresh.
- **Zero CPU in a raw row:** no CPU-time increment at the accounting resolution in that short interval. It does not establish that the process has zero cost.
- **Memory samples:** arithmetic mean and maximum of one-second observations. Peaks between samples may be missed. Per-process peaks are not summed into a simultaneous global peak.
- **Scope:** client widget and its direct SSH child only. The benchmark does not measure remote shell/awk CPU, network bandwidth, startup latency, GPU/desktop compositor overhead, battery use or long-term leak behavior.

These short runs are not statistical confidence intervals, a stress test or a leak certification. Repeat on your own hardware and workload before drawing stronger conclusions.

### Reproduce

Build Release, close diagnostic preview processes, start the actual widget and connect a reachable host. Set the interval, local panel, scale and opacity through the menu, then warm up for at least five seconds.

```powershell
.\build.ps1
Start-Process .\dist\NodeSight.exe

# After connection and warm-up, sample the real widget plus its direct SSH child.
.\tools\measure.ps1 -Seconds 60 -Label 'my-default-run' -OutputPath "$PWD\build\my-run.json"
```

If several NodeSight processes exist, pass `-AppProcessId` explicitly. The script rejects an ambiguous selection and fails if a measured process disappears, rather than quietly dropping its cost. A disconnect/reconnect during a run invalidates that run; ensure one SSH child was present and repeat after it stabilizes.

For a comparison, repeat each scenario several times, use the same workload and report all repetitions. Public reports contain only an allowlist of visual/sampling preferences, never the full connection INI. Re-running the script does not modify preferences.

### Why the implementation stays small

The executable draws through Win32/GDI, reuses one backbuffer and five fonts, stores five fixed-capacity series per host and redraws on the configured timer. SSH sampling runs on one worker thread through one persistent `ssh.exe` session. The server script reads virtual `/proc` files and caches `df` for fifteen samples. There is no continuous animation loop, metrics database or bundled application runtime.

The chart storage itself is bounded by `120 × 5 × sizeof(float)` per host, excluding ring metadata, snapshots and other application state. Windows libraries, stack/heap allocation and the SSH client account for much more of the observed footprint than the chart arrays.
