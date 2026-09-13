# WaveX Testing Guide

Run and extend the firmware tests from the devcontainer. This guide owns test
commands, regression-test rules, coverage limits and open test work. Phase
gates and hardware acceptance remain in [roadmap.md](roadmap.md).

## Contents

- [Prerequisites and commands](#prerequisites-and-commands)
- [What the suites exercise](#what-the-suites-exercise)
- [Writing meaningful regressions](#writing-meaningful-regressions)
- [LVGL widget tests](#lvgl-widget-tests)
- [Coverage and open work](#coverage-and-open-work)
- [Hardware verification](#hardware-verification)

## Prerequisites and commands

Use `./devcontainer.sh` from the repository root. Initialize the tracked
submodules first; GoogleTest is vendored under
`firmware/shared/tests/_deps/googletest-src` and reused by all suites.
See the [project README](../README.md) for container setup.

From the container's `/workspaces/WaveX`:

```bash
make test -j$(nproc)           # shared, Daisy and ESP32 host tests
make test-shared -j$(nproc)
make test-daisy -j$(nproc)
make test-esp32 -j$(nproc)
make test-asan -j$(nproc)      # AddressSanitizer + UndefinedBehaviorSanitizer
make test-clean -j$(nproc)
```

Sanitizer builds use separate `build-asan/` directories. Run them for
changes to indexing, parsing, pointer lifetime or source playback regions.
A passing value assertion can hide an out-of-bounds read.

For a focused executable, configure the owning suite explicitly after adding
a test file; its CMake glob is evaluated at configure time:

```bash
cmake -S firmware/shared/tests -B firmware/shared/tests/build
cmake --build firmware/shared/tests/build --target wav_header_parser_test --parallel $(nproc)
ctest --test-dir firmware/shared/tests/build -R WavHeaderParser --output-on-failure
```

Use `ctest --test-dir <build-dir> -N` to inspect the discovered test names,
or run `<build-dir>/bin/<test-name> --gtest_filter=Suite.Case`. All three
host trees compile first-party code as C++17. They do not cross-compile the
device images.

For a separate sanitizer experiment, configure another build directory with
`-DWAVEX_TEST_SANITIZE=address` or `=thread`. TSan and ASan are separate
runs. A sanitizer that fails to start has not validated the tests.

## What the suites exercise

| Suite | Production behavior covered | Boundaries replaced or omitted |
|---|---|---|
| Shared | UART/fixed-packet framing and CRC, sequence tracking, protocol round trips, WAV/WXCF/WXI parsing, sample registry, MIDI and shared control math | In-memory file readers; no serial peripheral or real SD |
| Daisy | Voice rendering/envelopes/filter, instrument resolution, sequencer and tempo cores, allocators, CV laws, browsing and message dispatch | FatFs/board boundaries mocked; full audio callback and UART/DMA drivers remain HAL-bound |
| ESP32 | Packet routing and listener delivery, application init flow, file browsing, waveform cache/fetch/panel state, panel-key model | FreeRTOS/IDF and most LVGL calls mocked; complete UI task and page lifecycle are not executed |
| ESP32 widget | Waveform pixels rendered with the real vendored LVGL software renderer | Memory-backed display; no MIPI, PPA, touch driver or real panel |

Inspect the owning `tests/CMakeLists.txt` and each test's linked sources
before claiming coverage. A dispatch mock proves the command reached its
boundary; it does not prove that the real loader, callback or driver handled
it correctly. The shared and Daisy `tests/integration/` directories do
not themselves provide end-to-end coverage merely by existing.

The audit baseline on 2026-09-06 passed 287 shared, 463 Daisy and 207 ESP32
cases (957 total). This is a reproducible baseline, not a coverage percentage
or a hardware result. Fix-specific validation belongs in the change's commit
and changelog.

## Writing meaningful regressions

A regression must fail against the pre-fix production implementation. Keep
new tests present, substitute only the old implementation in an isolated
scratch checkout, rebuild the focused target and observe the intended
failure. Restore and rerun the fixed implementation. Do not overwrite an
active shared checkout while another worker is using it.

For HAL-bound defects, document the untested boundary and the exact bench
check needed. A test of a copied algorithm or a mock replacing the broken
method is not evidence for that fix.

Use these rules:

- Test observable contracts: a rejected packet reaches no handler, a failed
  load preserves the current binding, a freed allocation cannot still be
  referenced, a stale response cannot replace a newer request.
- Cover normal operation and failure transitions: empty, exact boundary,
  truncated, overflow, full queue, timeout, retry, replacement and teardown.
- Use exact-sized malformed buffers with ASan. Padding every short input to
  maximum size masks the overread being tested.
- Assert all material fields with distinct values in round-trip tests;
  default-zero fields do not catch swaps or omitted serialization.
- Mock hardware I/O, not the production behavior under investigation.
  A mock must model the failure or concurrency property its test claims.
- Keep test names explicit and fixtures independent. Reconfigure CMake for
  new `*_test.cpp` files; check discovery before treating a new file as run.
- Keep tests for compiled-out SPI and forward-built formats. Delete tests
  only when their production code is removed.

The default ESP32 mocks are useful for deterministic dispatch/value tests,
but do not emulate the complete FreeRTOS scheduler or SMP interleavings.
Concurrency tests need real host synchronization at the tested boundary.

## LVGL widget tests

`firmware/esp32/tests/widget/` renders real LVGL into a memory buffer.
Use it for leaf widgets before declaring rendering untestable on the host.

The widget CMake directory clears inherited include paths so the ordinary
suite's stub `mocks/lvgl.h` cannot shadow the real library. Keep this
separation when adding widget targets.

Measure structural relationships, not palette choices: a loud channel
should occupy more pixels than a silent channel; lane clipping should
preserve separation; strip rendering should match a whole-screen render.
Counting rows containing ink is insufficient when the grid crosses every row.
Mutation-check the relevant property, for example by swapping channel lanes
and confirming the stereo tests fail.

Complete page tests still need their `main` dependencies and lifecycle
modeled. Leaf-widget success does not validate navigation, listener teardown,
touch coordinates or rendering time on the panel.

## Coverage and open work

Prioritize defect classes over line counts. The previous >80% core / >60%
hardware numbers were goals, not measured results. No repository-wide
coverage percentage is established by `make test`.

Outstanding work:

- Expand exact-sized payload sweeps through the **real** ESP32 packet
  handlers and shared parsers. Assert both bounds safety and no dispatch on
  invalid input. Error-handler tests must not override the handler they
  claim to exercise.
- Stress listener registration/removal and complete snapshot handoffs with
  real synchronization under TSan. Check allocation-failure behavior
  independently of the normal mutex mock.
- Extract the HAL-free streaming ring from `audio_engine.cpp` when working
  in that path, then stress producer/consumer wrap, reset and shutdown.
  Host memory-model checks do not prove M7 interrupt/cache correctness.
- Add randomized voice-region properties with guarded storage: reads stay
  inside the selected source, interpolation does not cross trim bounds,
  phase remains valid, and initialization restores every voice.
- Exercise navigation/input with realistic page lifecycle boundaries.
  Use on-target assertions or focused architectural checks for LVGL call
  provenance; value-only tests cannot prove task ownership.
- Keep full `audio_engine.cpp` and `daisy_uart_link.cpp` listed as gaps.
  Their excluded `audio_engine_test.cpp` and `daisy_uart_link_test.cpp`
  placeholders are not executed coverage.

Concrete discovered firmware bugs are tracked once in
[roadmap.md](roadmap.md#firmware-audit-remediation--2026-09-06);
do not duplicate their task status here.

## Hardware verification

`make test-hil` drives the boards' acknowledged debug consoles, with
`make logs-start` providing serial logs. See
[debug-harness-and-hil.md](features/debug-harness-and-hil.md) for setup,
commands and transcripts. Missing boards cause skips: an all-skipped run is
not a passing hardware gate.

Host tests cannot establish real-panel legibility, MIDI/audio latency,
cache/DMA ownership on silicon, CV settling, SD fault recovery, reboot
persistence or zero-underrun behavior. Device compilation also cannot prove
boot or audible output. Record DWT and soak results using
[performance_monitoring.md](performance_monitoring.md) and
[callback-performance-log.md](callback-performance-log.md).

## Related

- [Project principles](project-principles.md)
- [Roadmap and hardware gates](roadmap.md)
- [UI architecture](ui-architecture.md)
- [Daisy real-time guide](daisy_rt_audio_coding_guide.md)
- [ESP32-P4 guide](esp32p4_coding_guide.md)
