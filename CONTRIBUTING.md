# Contributing to Node-Sight

Small, focused changes are welcome. Keep the native desktop footprint and read-only remote collection model in mind.

## Development workflow

1. Windows: Visual Studio Desktop C++ and Windows SDK. Linux: C++17, GTK 3 development files and pkg-config. Use CMake with support for your toolchain.
2. Run `./build.ps1` on Windows, or the documented CMake Release commands on Linux; CTest must pass in Release, too.
3. Use the checked-in `.clang-format` for C++ and `.editorconfig` for text files.
4. Comment functions, data ownership, units and non-obvious branches **in English**. Explain intent and constraints instead of restating syntax.
5. Include validation and any measured resource impact in the pull request.

## Boundaries worth preserving

- Histories have fixed capacity; avoid allocations that grow with uptime.
- The UI reads mutex-protected remote snapshots; it does not share mutable counters directly.
- Native GDI objects and process/pipe handles require balanced cleanup.
- Collectors normalize bytes, elapsed seconds and cumulative counters before deriving percentages.
- No commands may modify the monitored Linux host. Do not add sudo, background daemons or remote file installation to the default collector.
- Windows optional passwords remain in Credential Manager; Linux uses SSH keys/agent. Never print secret contents, place passwords in argv or add them to examples.
- Use a strict Windows handle inheritance allowlist and close-on-exec POSIX pipes. Independent SSH sessions must not keep each other's streams alive.
- Preserve unaffected sessions for presentation-only changes. Hidden enabled nodes still reach recording.
- Recording must remain bounded in both memory and disk use; receipt-time gaps cannot be represented as complete peaks.

## Tests and performance

Run `ctest --test-dir build -C Release --output-on-failure`. Test genuine boundary conditions: reboots, counter resets, malformed frames, sample gaps and cleanup. Do not rely on `assert()` in Release-only checks.

Linux also builds a concurrent-transport test with a fake SSH executable copied into a test-only PATH. Do not point fixture code at real credentials. Native GTK lifecycle/settings smoke tests can run with `xvfb-run -a ./build/NodeSight --smoke-test` and `--settings-smoke-test`.

For changes to rendering or collection, warm the actual widget for at least five seconds, then run `tools/measure.ps1 -Seconds 60`. Record the interval, local-panel state, scale, working set, private memory and the separate SSH cost. Use the same machine/settings for comparisons. See [benchmark methodology](docs/PERFORMANCE.md).

## Public screenshots and privacy

Use `NodeSight.exe --demo-preview preview.bmp` or `--demo-server-preview preview.bmp` on Windows, or the same flags with PNG output on Linux. Windows settings exports are `--settings-preview` and `--recording-preview`. These routes use synthetic data and neutral identities before drawing. Desktop screenshots and private diagnostic/config files can expose real identities; do not commit those files.

Use reserved examples such as `server.example.com` and `monitor`. Exclude settings, private keys, tokens, logs, build caches and real connection diagnostics from patches.
