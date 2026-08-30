# Test remediation plan

**Written 2026-08-30.** Scope: close the gap between what the August 2026 code
audits found and what the test suite can catch.

## The finding that drives this plan

Between 2026-08-28 and 2026-08-30, code audits produced roughly forty
behavioural fix commits across both firmwares. **Not one of them shipped with a
regression test.** Three test-quality commits landed separately
(`44909df`, `b142743`, `848e089`) and retro-fitted coverage for some of the
defects, but they were a parallel effort, not part of each fix.

The suite is not small — 613 host tests, all green, and the quality pass
removed the tautologies that made the older suite gameable. The problem is not
volume. It is that the suite was **not the thing that found any of these
bugs**, and for most of them it still would not. `docs/backlog.md` says this
outright about the UI layer: the LVGL-threading and listener-lifetime defects
"were found by reading rather than by a failing test."

So this plan is organised by **defect class**, not by uncovered file. A
per-file coverage push would have produced the same suite that already missed
these bugs.

### The sibling-drift evidence

`44b0215` added a minimum-size guard to `ValidateWaveXPacket`, with a comment
naming the exact `size_t` underflow. Two functions in the same file, 120 lines
away — `CalculatePacketCrc` and `ValidatePacketCrc`, both public — kept the
identical bug until this plan's first commit. A case test written against the
fixed function could not have caught that. This is the argument for sweeps and
sanitizers over one-off case tests, in one file.

## Tier 0 — sanitizers (done, and the highest-leverage item)

**Status: implemented.** `WAVEX_TEST_SANITIZE` in all three test
`CMakeLists.txt`, plus `make test-asan`.

This is a bug detector rather than new tests, and it retroactively upgrades all
613 existing tests. Two results measured while building it:

| Experiment | Result |
|---|---|
| Existing `voice_manager_test`, unmodified, against pre-`228316d` code under ASan | `heap-buffer-overflow ... READ of size 2 in VoiceManager::Render` |
| New minimum-frame sweep against pre-fix `protocol.cpp` under UBSan | `applying non-zero offset 18446744073709551615 to null pointer` |

The first is the important one: **the release-tail out-of-bounds read was
already being executed by a passing test.** The read returned a plausible float
and every value assertion still held, so the suite ran straight over it and the
defect survived until a human read the code weeks later. ASan alone closes that
class with no new test bodies.

`-fno-sanitize-recover=all` is deliberate — without it UBSan prints and
continues, and ctest still reports a pass.

Remaining Tier 0 work:

1. **A CI job** running `make test-asan`, alongside the existing per-suite jobs
   in `.github/workflows/ci.yml`. Without this the option exists and nobody
   runs it.
2. **`-Wall -Wextra` on the test targets.** There are no warning flags at all
   today. `-Wconversion` would have caught `1d16237`'s `size_t` → `uint16_t`
   narrowing at compile time, for free.

## Tier 1 — the untrusted-input class (Daisy side done)

**Status: the Daisy dispatch sweep is implemented**, as two `TEST_F`s in
`message_dispatch_test.cpp` (no CMake change needed). 256 msg_types × 301
payload lengths × 4 fill patterns ≈ 308k dispatches in 443 ms, deterministic.

Verified against pre-`b34c814` handlers: ASan reports
`heap-buffer-overflow ... in strlen`, the exact defect. All three ingredients
were necessary — the `0x41` fill (no NUL anywhere), the exact-sized heap
allocation (so byte `[len]` is a redzone), and ASan itself.

Still to do here: the same sweep against `PacketRouter::route_uart_message` /
`route_packet` on the ESP32 side, and against `ParseUartPacket` on the shared
side.


Seven of the audited defects are one shape: a length or pointer from the wire
is trusted, and the code reads outside the buffer it was handed
(`44b0215`, `af359b5`, `b34c814`, `1d16237`, plus the two CRC siblings above).

**A dispatch sweep subsumes the class.** Not a fuzzer — a deterministic
cartesian sweep over (all 256 `msg_type` values) × (payload lengths 0 to
max-struct + 4) × (fill patterns), roughly 72k dispatches in well under a
second, fully reproducible. Two assertions: the sanitizer, and "no handler is
invoked for a payload below its struct's size".

The load-bearing detail is **exact-sized heap allocation**. Every existing
dispatch test uses a fixed stack array, so an overread lands in adjacent stack
and reads a plausible byte. One byte past a right-sized heap allocation is an
ASan redzone. This is why the existing
`BrowsePathWithoutTerminatorIsBoundedToPayload` would have passed pre-fix in
some builds.

Entry points, all of which take a raw `(buffer, size)` pair and are already
driven by existing tests:

- Daisy: `WaveX::Comm::ProcessInterMcuMessage(msg_type, seq, payload, size)` —
  the single `switch` the real RX path calls. Insert as new `TEST_F`s in
  `message_dispatch_test.cpp` to avoid a CMake change.
- ESP32: `PacketRouter::route_uart_message` / `route_packet`.
- Shared: `ParseUartPacket`, `ProtocolHandler::ValidatePacket`.

Case tests are still needed for the four defects a sweep cannot reach:
`746f39e` (an I/O-cost bug, not a memory-safety one — needs a readdir call
counter in the FatFS mock), `1d16237(c)` (in an excluded TU), `d9e97b2`
(semantic path join), and `4b63c37(b)` (concurrency).

### Fake coverage to fix

`PacketRouterTest.ErrorMessageWithUnterminatedTextIsBounded` does not test the
fix it appears to. That TU installs a strong override of
`PacketRouter::handle_error` (`packet_router_test.cpp:61`), so the real handler
never runs and the test passes against pre-fix code. It needs a separate TU
with no override.

## Tier 2 — DSP invariants over case tests

The audio suite is the best-covered part of the codebase and still missed
`228316d` and `f09a423`, because both are **value-invisible**: the wrong read
returns a plausible number. Three properties subsume most of the class:

- **P1 — no read outside `[start_frame, end_frame)` for any trigger
  parameters.** Randomise the trigger parameter space against a sample placed
  flush under a `PROT_NONE` guard page. Verified to SIGSEGV on pre-`228316d`
  code. Subsumes `228316d`, `f09a423`, the loop seam, and the `idx0` clamp.
  The guard page is deterministic where ASan depends on redzone placement, so
  it is worth having in addition to Tier 0.
- **P2 — every rendered sample lies within the min/max of the source values in
  the active region, and `phase` stays inside that region after every
  `Render()`.** Subsumes the `f609adb` snap fallback (254 of 256 samples out of
  range without it) and degenerate-loop freezes.
- **P3 — zeroed memory + `Init()` is byte-identical to constructed +
  `Init()`.** This is the generic form of `03af28e` and `062ea80`, both of
  which were "DTCM state is zeroed, so a member that needs a non-zero default
  is silently wrong." **Caveat: this currently fails even post-fix**, at
  `Voice::src_channels` and other `voices_{}` members that `Trigger()`
  overwrites before use — latent, not live. Making it usable means having
  `Init()` do `voices_ = {}`; that one line converts a per-member vigilance
  rule into a mechanical check.

Uncovered case tests worth writing regardless, with verified discrimination:

| Defect | Pre-fix | Post-fix |
|---|---|---|
| `f09a423` trimmed release tail | peak 0.4577 | 0.0153 |
| `f609adb` fractional phase preservation | 25/30 samples mismatch | exact |
| `d97eaf1` fades count at the sample's rate | 480 frames | 441 frames |

## Tier 3 — concurrency, honestly scoped

Sixteen commits, and **most are genuinely not unit-testable.** Recording why
matters more than forcing coverage:

- `e73b4c7`, `9a76595`: the fix *is* `ScopedIrqBlocker`, which libDaisy defines
  as an empty ctor/dtor under `UNIT_TEST`. Fixed and buggy code are
  byte-identical on the host. Not testable without a fake-preemption harness.
- `ebeeb3c`, `cf8ee4b`, `65798a7`, `062ea80`: in `audio_engine.cpp`, which is
  excluded from the host build.
- `21304be`, `222b2b4`: the property is *provenance* ("no `lv_*` call
  originates off the LVGL task"), not a value. Wrong shape for a unit test; the
  right tools are an on-target `assert(lvgl_port_lock_held())` or an
  architectural lint.

What **is** worth doing, highest ROI first:

1. **Make the FreeRTOS mutex mock real.** `esp32_mocks.cpp:159-170` returns
   `(void*)1` from `xSemaphoreCreateRecursiveMutex` and `pdTRUE` from
   take/give. Backing those three with a `std::recursive_mutex` (~30 lines)
   makes `ListenerSlot` genuinely stress-testable — N invoker threads plus a
   setter thread that clears and frees. Under TSan+ASan that catches both the
   torn `{cb, user_data}` pair and the UAF from `41f4cdd`. The existing
   `listener_slot_test.cpp` concedes in its own header comment that it "does
   not prove the mutual exclusion."
2. **Extract the WAV ring buffer** (`378673b`) from `audio_engine.cpp` into
   `src/audio/sample_ring.hpp`. The ring code is already HAL-free
   (`std::atomic<uint32_t>`, plain index math) and drops straight into the
   daisy CMakeLists' existing header-only-test pattern. Producer/consumer
   stress under TSan. **Caveat: TSan validates the C++ memory model, not
   Cortex-M7 ISR preemption** — it proves the algorithm, not the "aligned
   32-bit store is atomic on M7" assumption underneath it.

### Defects found while surveying — resolved 2026-08-30

Three candidates came out of the survey. **Only one was a live race**, which is
worth recording: reasoning from the `volatile` keyword or from the shape of a
read-modify-write over-predicts, exactly as the lint discussion above argues.

- **`s_rb_low_water` (`audio_engine.cpp:289`) — live, fixed.** The ISR
  compare-and-latches it at 1596-97 while the main loop did a plain
  read-then-reset in `TakeRingLowWater()`. A dip latched in that window was
  discarded, so the losses fell precisely on the rare dips the diagnostic
  exists to catch. Now an `__atomic_exchange_n`, matching what `cf8ee4b` did
  for `s_underrun_detected`. The callback's own RMW needs no atomic: main-loop
  code cannot run partway through an ISR.
- **`g_message_count` (`metrics.h`) — not a defect: dead code.** The
  non-atomic `++` is real, but nothing calls it. `main.cpp`'s include is
  vestigial and the only caller is its own test file. Recorded in
  `backlog.md` with a recommendation to delete the module.
- **`log_ring.cpp` — not currently racing, now enforced.** It genuinely is not
  an SPSC ring (`Write()` advances `s_tail` on overflow, and so does
  `Drain()`), but the audit found no ISR logs: the audio callback has none,
  and on the UART path the ISR half (`append_rx_data_isr`) does not log — the
  logging is in `process_rx_frames()`, driven from the main loop. Rather than
  leave that as an unenforced convention across 253 call sites, `Write()` now
  refuses exception-context calls and counts them (`Log::IsrWrites()`, which
  must stay zero). Dropping a line is the safe failure; corrupting the ring
  indices is not.

### On a `volatile` lint

Surveyed: 28 first-party declarations, **zero of them MMIO** (all register
access goes through vendor HAL). So the legitimate use a lint would need to
allowlist does not occur in first-party code, and the allowlist is one
test-mock file.

But it is **a ratchet, not a detector.** Four of the six `volatile`
declarations in `audio_engine.cpp` are variables that `ebeeb3c`/`cf8ee4b`
already fixed — the fixes changed the access sites and left the declarations
`volatile` — while the two that are still broken there would be missed by any
reasoning from the keyword. It would have prevented only the mechanical
type-change commits and none of `422223e` (a plain `bool`, no `volatile`
anywhere), `41f4cdd`, `21304be`, or `9a76595`.

Recommended shape if adopted: a **countdown, not a freeze**. Convert the 28
hits starting with the three live defects above, with a count-based baseline
that may only be edited downward.

## Tier 4 — coverage gaps not tied to an audit defect

Lower priority precisely because no shipped bug traces to them.

- **`audio_engine.cpp` is 3476 lines with zero host coverage.**
  `audio_engine_test.cpp` exists but holds 3 `DISABLED_` tests and is
  `list(FILTER ... EXCLUDE)`d from the build; `daisy_uart_link_test.cpp` is the
  same (5 tests). So the largest and most bug-dense file in the Daisy firmware
  is untested, and two test files are dead weight that read as coverage. The
  codebase already has the answer — `voice_manager.hpp`, `fade.hpp`,
  `output_sink.hpp`, `svf_filter.hpp` are all HAL-free header-only units with
  real tests. **Continuing that extraction is the single structural fix**, and
  it is what unblocks the `audio_engine.cpp` items in Tiers 2 and 3.
- **ESP32 UI.** Of 27 files under `components/ui/{src,pages}/`, 18 are free of
  the `main` dependency cycle and testable today — including `ui_navigator.cpp`
  (271 lines, navigation stack) and `input_dispatcher.cpp`. The blocker is that
  `tests/mocks/lvgl.h` is a 96-line stub against ~23 LVGL calls in
  `ui_navigator` alone, so this needs an LVGL fake (object model with
  create/clean/valid plus event dispatch) before any of it is reachable. The
  remaining 9 files are blocked on E-ARCH1, which `docs/backlog.md` defers
  until after the hardware pass — that deferral still holds and this plan does
  not touch it.
- `profiling/profiler.cpp` — needs `stm32h7xx.h`, so not host-testable without
  a HAL shim.
- `pattern.hpp` — HAL-free and tested only incidentally through
  `sequencer_scheduler_test.cpp`. Phase 2's edit-between-steps discipline
  (`features/sequencer.md` §4) is the reason to give it its own tests before
  the callback integration lands.

## Working rule going forward

Every behavioural fix ships with a test that **fails against the pre-fix
code**, or with a one-line note in the commit saying why that is not possible
(HAL-bound, excluded TU, provenance property). The check is cheap — restore the
pre-fix hunk, run the new test, confirm red — and it is the only thing that
distinguishes a regression test from a test that merely passes.
