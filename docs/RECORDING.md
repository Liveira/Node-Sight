# Recording and peak interpretation

Open **Settings → Recording**. Enable automatic recording, choose a folder and duration, then **Apply changes**. A duration is entered in minutes; **Continuous / no deadline** uses no time limit. The tray/context menu can also start, restart or stop a session without changing automatic startup preferences.

The recorder runs while the application is running, including when the widget is hidden in the tray. Quitting, logging out or switching off the computer stops collection. Autostart can begin a new session at the next desktop login; this is not a remote daemon that continues while the desktop client is off.

## Files and retention

The active file is **`node-sight.log`**, appended and flushed at least once a second during normal event processing. It is reused across app launches and has `SESSION_START` / `SESSION_END` boundaries. The default folder is a `logs` directory under the per-user configuration folder.

At the chosen size limit, the active file becomes `node-sight.1.log`, previous archives shift upward, and the oldest permitted archive is removed. The default is **10 MiB per file and five archives**, plus one active file: approximately 60 MiB maximum payload. The application does not scan/delete unrelated files in the selected folder. Choose a larger limit or copy archives elsewhere if you need longer retention.

Disk/permission errors stop recording and are shown in the status. Monitoring continues. After a crash, power loss or forced termination, the most recent buffered writes and final session marker may be missing; this is not a transactional audit log.

## Collection rate versus summary rate

Collection can run every **100 ms** while summaries are written every **1 second**. Every received frame is handed through a bounded queue to the recorder; peaks are retained across those frames instead of inspecting only the final value each second. A slower summary setting reduces disk traffic without deliberately discarding the received intermediate peaks.

Summaries cannot contain new measurements faster than the collection rate. The queue holds 256 frames per remote profile. If the desktop is blocked long enough to overflow it, `SAMPLING_GAP dropped=N` marks the loss and the affected peaks are incomplete. Hardware activity between collection instants, or data lost before receipt, cannot be reconstructed.

## Events

| Event | Meaning |
| --- | --- |
| `SESSION_START` | Collection/summary intervals and configured duration |
| `CONNECTED` / `DISCONNECTED` | Profile availability transition |
| `SAMPLE` | Latest received metrics, summary-window peaks, peak timestamps and session peaks |
| `SPIKE` | CPU or RAM crossed the configured high-use condition; limited to one event per node per 10 seconds |
| `SAMPLING_GAP` | A bounded handoff overflowed; the count is explicit |
| `SUMMARY` | Final values and retained session peaks when a node/session ends |
| `SESSION_END` | Normal stop, restart, duration reached or shutdown |

Threshold events indicate that a sampled value met the configured limit; they are not an interpolated measurement of the complete physical spike duration.

## Units and timing

CPU, RAM and disk occupancy are percentages. RAM capacities are binary bytes; network rates and their peaks are bytes/second. Uptime is seconds and temperature is Celsius or `n/a`. A first sample has no valid CPU/network delta and records those fields as `n/a`.

Timestamps are ISO-8601 **UTC** with milliseconds and `Z`. The line timestamp is the write time. Fields such as `window_cpu_peak_at`, `window_ram_peak_at`, `window_rx_peak_at` and `window_tx_peak_at` identify the corresponding **client receipt time**, derived from the sample's monotonic timestamp. They are not the server's wall-clock time or a guarantee of when a physical hardware event began; SSH/network buffering can delay receipt.

The `node` field is a stable local profile identifier and `label` is its friendly alias. Destination addresses, SSH user names, key paths and passwords are not automatically added. An alias can still reveal personal information if you put it there, and resource values may be sensitive: inspect a log before sharing it.

## Example excerpt

Illustrative, synthetic values; a real `SAMPLE` line has additional capacity and network fields:

```text
2026-11-02T12:00:00.000Z SESSION_START sample_interval_ms=100 summary_interval_ms=1000 duration_minutes=0
2026-11-02T12:00:00.200Z CONNECTED node="application"
2026-11-02T12:00:02.000Z SAMPLE node="application" label="Application node" cpu_pct=24.00 ram_pct=64.00 ... window_peak_cpu_pct=91.20 ... window_cpu_peak_at="2026-11-02T12:00:01.400Z"
2026-11-02T12:10:00.000Z SESSION_END reason=user
```

Hidden profiles remain monitored and recorded while **Monitor this server** is enabled. Disabling monitoring closes that profile's SSH connection. Visual metric toggles do not remove the metrics from logs.
