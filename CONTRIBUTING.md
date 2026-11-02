# Contributing to Node-Sight

Small, focused changes are welcome. Keep the native desktop footprint and read-only remote collection model in mind.

## Development workflow

1. Build with Visual Studio 2022/2026 Desktop C++, Windows SDK and CMake 3.22+.
2. Run `./build.ps1`; CTest must pass in Release, too.
3. Use the checked-in `.clang-format` for C++ and `.editorconfig` for text files.
4. Comment functions, data ownership, units and non-obvious branches **in English**. Explain intent and constraints instead of restating syntax.
5. Include validation and any measured resource impact in the pull request.

## Boundaries worth preserving

- Histories have fixed capacity; avoid allocations that grow with uptime.
- The UI reads mutex-protected remote snapshots; it does not share mutable counters directly.
- Native GDI objects and process/pipe handles require balanced cleanup.
- Collectors normalize bytes, elapsed seconds and cumulative counters before deriving percentages.
- No commands may modify the monitored Linux host. Do not add sudo, background daemons or remote file installation to the default collector.
- Credentials remain in Windows Credential Manager. Never print them, place them in argv or add them to configuration examples.

## Tests and performance

Run `ctest --test-dir build -C Release --output-on-failure`. Test genuine boundary conditions: reboots, counter resets, malformed frames, sample gaps and cleanup. Do not rely on `assert()` in Release-only checks.

For changes to rendering or collection, warm the actual widget for at least five seconds, then run `tools/measure.ps1 -Seconds 60`. Record the interval, local-panel state, scale, working set, private memory and the separate SSH cost. Use the same machine/settings for comparisons. See [benchmark methodology](docs/PERFORMANCE.md).

## Public screenshots and privacy

Use `NodeSight.exe --demo-preview preview.bmp` or `--demo-server-preview preview.bmp`. These routes use deterministic synthetic data and anonymous labels before drawing any pixels. Live `--render-preview` exports can expose local hostnames and connection identities; do not commit those files.

Use reserved examples such as `server.example.com` and `monitor`. Exclude settings, private keys, tokens, logs, build caches and real connection diagnostics from patches.
