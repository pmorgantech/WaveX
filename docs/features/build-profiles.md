# Debug and Release Build Profiles

**Status**: Proposed. Nothing here is built. Today there is one build
configuration per board, and the only thing resembling a release switch is
`WAVEX_DEBUG_LOGGING_ENABLED` defaulting to `1`
(`firmware/shared/config/logging_config.h:381`) with no supported way to set it
to `0` — see § *A documented override that does not work*.

**Why it exists separately**: this began as one section of
[`debug-harness-and-hil.md`](debug-harness-and-hil.md), which needs a real
compile-out guarantee. It is its own document because it is worth doing on its
own terms — it closes a release-hygiene gap that exists right now, in shipped
images, whether or not the harness is ever built.

---

## 1. The flag hierarchy

`WAVEX_DEBUG_LOGGING_ENABLED` is already the de-facto master switch, and it
already gates something that is not logging: serial screenshots
(`WAVEX_ESP_SCREENSHOT_DEBUG`, `logging_config.h:394-396`). The name has
outlived its meaning. Introduce one honest master and default the existing names
from it, so **no current `#if` site changes meaning**:

```
WAVEX_BUILD_DEBUG                 master; 1 in the debug profile, 0 in release
├── WAVEX_DEBUG_LOGGING_ENABLED   defaults to WAVEX_BUILD_DEBUG  (existing name, existing sites)
├── WAVEX_DEBUG_HARNESS_ENABLED   defaults to WAVEX_BUILD_DEBUG  (the console harness)
└── WAVEX_ESP_SCREENSHOT_DEBUG    defaults to WAVEX_DEBUG_LOGGING_ENABLED  (unchanged)
```

Each stays individually overridable, so a release image can keep the harness for
a bring-up session without turning logging back on — which is the configuration
someone will want on the bench the first time a release-only bug appears.

These live in `logging_config.h` beside the flags they generalise. That file
already has a section for exactly this ("FEATURE FLAGS that live here because
they follow the debug/release notion of the firmware, not because they gate log
lines", `:373-376`).

## 2. The Daisy gap, closed

`WAVEX-LOG` handling on the Daisy has **no compile guard at all** — the token
matcher (`daisy/src/main.cpp:77-128`) and the apply/reply block (`:512-526`)
ship in every image built today. The ESP32's equivalent has been guarded since
it was written. Wrap both in `WAVEX_DEBUG_HARNESS_ENABLED`.

**`WAVEX-ENTER-DFU` stays outside the guard, deliberately.** It is the only
reflash path that does not require someone physically holding BOOT and tapping
RESET, so gating the no-touch recovery mechanism behind the debug profile is
exactly backwards — the release image is the one most likely to need recovering.
The tradeoff is that anything writing that token to the CDC port reboots the
instrument into the bootloader; for a synth on a bench that is acceptable, and
the convenience is worth more than the exposure.

## 3. What release actually saves — stated honestly

Real removals: the console listener task and its buffers, and on the ESP32 the
whole of `ui_screenshot.cpp`, which is already `#if`-wrapped as a single unit
(lines 3–321) and takes the screenshot path with it.

**The runtime log-level table is not removed**, and claiming otherwise would be
the kind of unmeasured assertion this repo has been bitten by more than once.
`WaveX::Log::g_module_levels` is 12 bytes plus one byte-load per surviving call
site. With the command channel compiled out nothing can ever change it — but no
compiler will prove that, so the loads stay.

**The release win on logging comes from lowering `WAVEX_LOG_CEILING_*`, which
deletes call sites outright** (`logging_config.h:80-119`) — not from the master
flag. A release profile should therefore set the ceiling to `WARN`, and that is
the number worth measuring.

No size or CPU figure appears in this document because none has been measured.
Both belong in the implementing commit, per `AGENTS.md` § Audio/DSP performance
discipline.

## 4. Profiles: Daisy

**The mechanism already exists and is already used for this exact shape of
problem.** `firmware/daisy/Makefile` takes `BUILD_DIR` and `CMAKE_EXTRA_ARGS`,
and `CMakeLists.txt:141-164` carries two established idioms for turning a CMake
variable into a compile definition. `WAVEX_BUILD_DEBUG` should copy the
`WAVEX_PROFILING_ENABLED` one (`:141-146`) rather than the Stage A/B one,
because unlike those it must always emit a define instead of leaving a header
default in force:

```cmake
option(WAVEX_BUILD_DEBUG "Debug profile: logging, console harness" ON)
add_compile_definitions(WAVEX_BUILD_DEBUG=$<BOOL:${WAVEX_BUILD_DEBUG}>)
```

The release target is then a sibling of the existing `daisy-stageb`
(`Makefile:227-233`):

```make
daisy-release:
	cd firmware/daisy && make BUILD_DIR=build-release \
		CMAKE_EXTRA_ARGS="-DWAVEX_BUILD_DEBUG=OFF"
```

**`BUILD_DIR` must differ per profile, and that is a correctness requirement
rather than tidiness.** `firmware/daisy/Makefile`'s `configure` target runs
CMake only `if [ ! -d "$(BUILD_DIR)" ] || [ ! -f "$(BUILD_DIR)/CMakeCache.txt" ]`
— so re-running with different `CMAKE_EXTRA_ARGS` against an existing build
directory **silently ignores the new flags and rebuilds the old configuration.**
It does not warn. That is why `daisy-stageb` already carries its own
`BUILD_DIR`, and the release profile must too.

## 5. Profiles: ESP32

Nothing comparable exists: there is not one `add_compile_definitions` in the
ESP32 tree and not one `Kconfig` file. The define has to be installed **before**
`project()` in `firmware/esp32/CMakeLists.txt`, because ESP-IDF configures every
component target inside that call:

```cmake
option(WAVEX_BUILD_DEBUG "Debug profile: logging, console harness" ON)
idf_build_set_property(COMPILE_DEFINITIONS
                       "WAVEX_BUILD_DEBUG=$<BOOL:${WAVEX_BUILD_DEBUG}>" APPEND)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wavex-esp32)
```

Ordering is not a detail. That file already carries a warning comment recording
this exact mistake: `-Os -flto -ffunction-sections -fdata-sections` once sat
after `project()`, reading like a size-optimised LTO build while the image was
in fact `-O2` with no LTO. Anything added after `project()` applies to nothing.

Release builds into its own directory for the same reason as the Daisy:

```sh
idf.py -B build-release -DWAVEX_BUILD_DEBUG=OFF build
```

**A `Kconfig.projbuild` option was the alternative and is rejected.** It would
make this the project's first Kconfig file and move the switch into `sdkconfig`
— a generated, checked-in, 3000-line file that already disagrees with
`sdkconfig.defaults` — in order to express one boolean.

Note the ESP32 is *already* built like a release image day to day:
`CONFIG_COMPILER_OPTIMIZATION_PERF` with `ASSERTIONS_DISABLE`
(`sdkconfig.defaults:10-11`). This is precisely why the compiler's own notion of
a debug build cannot carry this distinction, as `logging_config.h:378-380`
already says.

## 6. A documented override that does not work

`firmware/shared/config/README_HARDWARE_CONFIG.md:201-212` instructs the reader
to disable features like this:

```bash
make CFLAGS="-DWAVEX_DEBUG_LOGGING_ENABLED=0"
```

**That cannot take effect on either target.** GNU Make does forward a
command-line override down through the recursive sub-makes, but the innermost
make on the Daisy is CMake-generated and never consults `$(CFLAGS)` — flags are
baked into `flags.make` at configure time — and on the ESP32 the build is
`idf.py`, which does not read `CFLAGS` at all.

This is a mechanical conclusion from reading the build files, not an experiment;
confirm it with one build before editing. It matters because it is currently the
*only* documented way to turn debug logging off, so that section should be
replaced by the profile targets above rather than simply deleted.

## 7. CI

CI already builds two Daisy flag configurations — `make daisy` and `make
daisy-stageb` (`.github/workflows/ci.yml`) — so release-profile builds extend an
established pattern rather than introducing one. Add `daisy-release` and the
ESP32 release build.

Then add the step that actually matters, which is **not** "release compiles":

```sh
strings firmware/daisy/build-release/wavex-daisy.elf | grep -q 'WAVEX-DBG' && exit 1
```

That tests the property the profiles exist to guarantee — nothing of the debug
surface in the release firmware — and it fails the day someone adds a guarded
feature's call site outside its guard. "It compiled" would not catch that. The
command token is the right artifact to grep for because it is a string literal
that cannot survive without the code referencing it.

## 8. Order of work

1. `WAVEX_BUILD_DEBUG` in `logging_config.h`, with the existing flags defaulted
   from it. No behaviour change: every current site keeps its current value.
2. The Daisy `WAVEX-LOG` guard (§2).
3. Daisy and ESP32 profile plumbing and Make targets (§4, §5).
4. CI release builds plus the `strings` gate (§7).
5. Replace `README_HARDWARE_CONFIG.md`'s `CFLAGS` section (§6), after confirming
   with a build.
6. Measure and record the release-profile size delta, with and without lowered
   log ceilings (§3).

## 9. Decisions still open

1. **Does the release profile lower `WAVEX_LOG_CEILING_*`, and to what?** §3
   argues the real saving lives here rather than in the master flag. `WARN` is
   the proposed default; confirm against a measured size delta before choosing,
   since the answer also decides whether ERROR/WARN diagnostics survive in a
   field image.
2. **Does anything else belong under `WAVEX_BUILD_DEBUG`?**
   `WAVEX_DAISY_UART_PERF_DEBUG` (`logging_config.h:408-410`) currently defaults
   to `1` independently, and its own comment says the measurement rides the hot
   path whether or not the report prints. That makes it a candidate — but it is
   a behaviour change to the audio-adjacent link path, so it wants its own
   measurement rather than being swept in.
