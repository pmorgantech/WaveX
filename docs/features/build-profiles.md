# Debug and Release Build Profiles

**Status**: Proposed. Nothing here is built. Today there is one build
configuration per board, and the only thing resembling a release switch is
`WAVEX_DEBUG_LOGGING_ENABLED` defaulting to `1`
(`firmware/shared/config/logging_config.h:381`), whose only documented override
was measured not to work on the board it matters on — see § 6.

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
if(WAVEX_BUILD_DEBUG)
    add_compile_definitions(WAVEX_BUILD_DEBUG=1)
else()
    add_compile_definitions(WAVEX_BUILD_DEBUG=0)
endif()
```

That is `WAVEX_PROFILING_ENABLED`'s shape character for character, deliberately:
it is the form already proven in this tree, and both boards then spell the flag
the same way.

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
ESP32 tree and not one `Kconfig` file. The define goes in via
`idf_build_set_property`, and **its placement is narrowly constrained — there is
exactly one window that works**:

```cmake
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

option(WAVEX_BUILD_DEBUG "Debug profile: logging, console harness" ON)
if(WAVEX_BUILD_DEBUG)
    set(_wavex_bd 1)
else()
    set(_wavex_bd 0)
endif()
idf_build_set_property(COMPILE_DEFINITIONS "WAVEX_BUILD_DEBUG=${_wavex_bd}" APPEND)

project(wavex-esp32)
```

**After `include()`, before `project()`.** Both bounds were measured, and both
bite:

- Earlier than the `include()` fails outright with `CMake Error: Unknown CMake
  command "idf_build_set_property"` — that command is *defined by*
  `project.cmake`, so it does not exist until the include has run.
- Later than `project()` silently applies to nothing, because ESP-IDF
  configures every component target inside that call. The file already carries a
  warning comment recording exactly this: `-Os -flto -ffunction-sections
  -fdata-sections` once sat after `project()`, reading like a size-optimised LTO
  build while the image was in fact `-O2` with no LTO.

**Verified 2026-08-31** against `compile_commands.json`: `-DWAVEX_BUILD_DEBUG=ON`
puts `=1` on 1767 of 1768 translation units, `OFF` puts `=0` on the same 1767.
The one TU without it is `build/project_elf_src_esp32p4.c`, a generated empty
stub IDF hangs the ELF target from — no code, nothing of ours.

The plain `if()`/`set()` form above is what was tested. A
`$<BOOL:${WAVEX_BUILD_DEBUG}>` generator expression reads more neatly but was
not verified in this property, and `COMPILE_DEFINITIONS` on the IDF build object
is not a normal target property — do not substitute it without re-running the
check.

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

## 6. A documented override that does not do what it says

`firmware/shared/config/README_HARDWARE_CONFIG.md:201-212` instructs the reader
to disable features like this:

```bash
make CFLAGS="-DWAVEX_DEBUG_LOGGING_ENABLED=0"
```

**Measured 2026-08-31**, by configuring both trees with probe defines in
`CFLAGS` and `CXXFLAGS` and reading the generated compile commands:

| Target | Result |
|---|---|
| Daisy | **Reaches the compiler.** GNU Make exports command-line variables into the recipe environment, CMake seeds `CMAKE_C_FLAGS`/`CMAKE_CXX_FLAGS` from there, and both probes appear in `CMakeFiles/wavex-daisy.dir/flags.make`. |
| ESP32 | **Reaches nothing.** 0 of 1768 translation units in `compile_commands.json` carried either probe. ESP-IDF constructs its own flag set and does not consult `CFLAGS`/`CXXFLAGS`. |

So the documented command does not achieve its stated purpose — but the
mechanism is not broken the way it looks. Three distinct problems, and it is
worth separating them because only the third generalises:

1. **On the ESP32 it is inert**, and the ESP32 is where
   `WAVEX_DEBUG_LOGGING_ENABLED` actually gates anything today
   (`WAVEX_ESP_SCREENSHOT_DEBUG` → the whole console listener). For the example
   as written, on the board it matters on, it does nothing at all.
2. **On the Daisy the variable is the wrong one.** `CFLAGS` reaches C
   translation units; `logging_config.h` is C++ and every first-party Daisy TU
   consuming the flag is `.cpp`, so `CXXFLAGS` is what would be required.
3. **It only takes effect on the first configure of a build directory.**
   `firmware/daisy/Makefile`'s `configure` runs CMake only when `CMakeCache.txt`
   is absent, so re-running with a different value against an existing `build/`
   was measured to keep the *original* flag and print no warning. This is the
   same trap as § 4 and it applies to any flag delivered this way, including a
   corrected one.

Replace that README section with the profile targets rather than fixing the
variable name. `-DWAVEX_BUILD_DEBUG=OFF` through `CMAKE_EXTRA_ARGS` works on
both boards, is visible in review, and cannot be silently ignored because each
profile owns its own build directory.

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
5. Replace `README_HARDWARE_CONFIG.md`'s `CFLAGS` section with the profile
   targets (§6).
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
