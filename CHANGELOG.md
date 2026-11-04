# Changelog

## 2.0 — in development

- Restored the original compact terminal appearance: black backgrounds, monospace controls, numbered panes, thin colored borders, dotted graphs and segmented meters on both platforms, including settings.
- Prevented Windows default-button flashes during settings focus changes and clicks; preserved checkbox state and batched page redraws.
- Native Linux GTK 3/Cairo client and read-only local Linux counters.
- Shared monitoring, recording, configuration and rendering core across platforms.
- English interface with dedicated Servers, Appearance, Recording and About pages.
- Up to 32 simultaneous server profiles, independently monitored, hidden or collapsed.
- Sampling at 100, 250 and 500 ms, plus 1, 2, 5 and 10 seconds.
- Minimal, Balanced and Detailed layouts; per-metric and graph visibility.
- Local `.log` recording with finite or continuous sessions, threshold events, precise received-sample peak times and bounded rotation.
- Bounded sample handoff preserves subsecond peaks between UI ticks and explicitly reports overflow.
- App RSS, normalized CPU and observed peak RSS in the footer; detailed server capacities and peaks.
- Atomic UTF-8 preference writes and migration from single-server v1 settings.
- Windows SSH handle allowlists prevent cross-profile pipe inheritance and shutdown/reconnect deadlocks.
- Isolated regression tests for configuration, recording, rotation, concurrent transports and native GTK windows.

The development changes are separate from the published 1.0 release. Historical 1.0 benchmark reports remain labeled as historical measurements.

## 1.0 — 2026-11-02

Initial native Windows widget, single Linux SSH server, optional PC panel, terminal-style charts, pinning, opacity, tray behavior and credential helper.
