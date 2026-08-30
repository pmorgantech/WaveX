# WaveX ESP32-P4 Code Review — 2026-08-29

**Scope**: the ESP32-P4 frontend only — `firmware/esp32/main`, `firmware/esp32/components/ui`, the ESP32 build files (`CMakeLists.txt`, `sdkconfig.defaults`), and `firmware/shared/*` as consumed by the ESP32 side. The Daisy tree is out of scope (see `code_review_20260705.md` for the last full pass).
**Standard**: `skills/esp32p4/SKILL.md` → `docs/esp32p4_coding_guide.md` (§14 checklist), plus AGENTS.md constraints and `docs/ui-architecture.md`.
**Method**: four parallel subsystem reviews (core app/tasks, comm/links, input peripherals, LVGL UI) reading every first-party line, with all dead-code claims grep-verified against the whole repo. Every Critical and Major finding below was then independently re-verified against source (including the `esp_tca8418` and `esp_lvgl_port` managed components and the Waveshare BSP) before inclusion. No device build or hardware test was run for this review; nothing below depends on one, but E-KEY1/E-KEY2 predict hardware behaviour that should be confirmed on the bench.
**Branch**: `feature/sequencer-voice-audition` at `41013e1`.

Findings carry stable IDs (`E-…`) so implementation can be tracked in this file. **Completed items leave this document** — detail goes to `CHANGELOG.md`, matching the roadmap's convention — so what remains here is always the open list. A partially-addressed item keeps its row, marked `[~]`, and says what is left.

**Already remediated** (see `CHANGELOG.md` § Unreleased): E-LVGL1/2/3, the LVGL thread-safety cluster (`21304be`, `222b2b4`); E-LIFE1/2/3, the callback-lifetime cluster (`41f4cdd`); E-KEY1/2, the keypad decode and INT busy-spin (`cdab47d`); E-INIT1 and E-BLD1/2 (`f6b7394`); E-TICK1/E-TOUCH1/E-BRWS1/E-MENU1, the UI correctness batch (`edd9981`); E-TX1, the outbound-frame latency (`6a0912c`); E-METER1 and E-DIAG1, the two periodic-work wastes (`b64ae32`); E-MIDI1, E-KBD1 and the defect half of E-PROTO1 (`1d16237`); E-SYNC1 and E-STAT1 (`4b63c37`); E-ODR1 (`4e535c2`); E-INQ1 and E-STD1 (`0be797a`); E-STOP1, E-PROTO1, E-UIM1 and E-VER1 (`043d0d1`); E-CFG1, E-LOG1, E-TASK1 and E-SDK1. All addressed 2026-08-29. **E-SEQ1 was withdrawn as a false positive** - see below. **E-KEY1/2 and E-ENC1 change hardware behaviour and are the ones most needing a bench pass** — they were diagnosed entirely by reading code and the controller datasheet.

---

## 1. Executive summary

The live UART transport story has improved a lot since the 2026-07-05 review: the send-wrapper error inversion (old C3) is fixed, `SequenceTracker` now gates every CRC-valid frame on the UART RX path, frame scanning/parsing enforces capacity contracts, and there are no app-level DMA/cache or IRAM-ISR hazards anywhere in the ESP32 tree (no first-party ISRs exist; UART uses the non-DMA driver; the SPI link's DMA buffers are correctly allocated but the whole link is compiled out).

The debt is concentrated in three themes:

1. **LVGL thread-safety was systematically violated** — ~~the UART RX task mutating widgets directly in comm callbacks, the whole input dispatch path running outside the port lock, and `lv_async_call` issued from the wrong task~~. **Fixed 2026-08-29**; see `CHANGELOG.md`. Kept in this list because it is the reason themes 2 and 3 matter more than they look: the corruption this caused was the most likely explanation for "random" UI failures, so misbehaviour that survives these fixes is now much more likely to be one of the remaining items than a mystery.
2. **Callback lifetime and cross-core publication were unmanaged** — ~~listener pairs as unsynchronized globals, a file browser that never deregistered, and four different locking disciplines across four slots in one class~~. **Fixed 2026-08-29**; see `CHANGELOG.md`. The publication half is closed too: all three comm-driven pages now use release/acquire atomics, and the `volatile` handoffs the guide bans are gone from this tree.
3. **The physical control surface had real functional bugs** — ~~a keypad that either never saw a key or busy-spun core 1, decoding presses as releases, and an encoder using interrupt masking as cross-core synchronization~~. **Fixed 2026-08-29**; see `CHANGELOG.md`. These were found by reading code and the TI datasheet, not by observing hardware, so they are the highest-value items in this review to confirm on the bench: if the keypad still misbehaves, the remaining suspect is the CFG register the vendored driver never writes.

Two systemic build findings rounded it out — ~~inert `-Os`/LTO options added after `project()`, and an `EXCLUDE_COMPONENTS` list that excluded nothing~~. **Fixed 2026-08-29**, and the fix confirmed both diagnoses: a clean rebuild came out within 48 bytes of the old image, which is what it should be if the options really were applying to nothing and the pruned exclusions really were being built anyway.

**Suggested order**: all Criticals are closed, and Major is down to E-STOP1 (latent — every teardown API is unsafe, but grep confirms none has a caller) plus E-ENC1's remaining watch-point-ISR half. What is left otherwise is the Minor and Smell batches: mostly hardware-truth cleanup (E-CFG1), deletions (E-DEAD1, E-ODR1) and build posture (E-SDK1, E-STD1). E-METER1 is worth pairing with the `lv_refr_now` question in roadmap § Outstanding hardware verification, since both concern the same duplicated refresh path. Then the Minor/Smell batches. E-SYNC1's remaining half (the sample-edit page's `volatile`) is cheap and can ride along with any edit-page work. The SPI findings (§7) do not need fixing now but must gate any re-enable of `WAVEX_SPI_LINK_ENABLED`. **Before any of that, a bench pass on the keypad and encoder** — three fixes now depend on hardware behaviour nobody has watched.

---

## 2. Tracking index

| ID | Sev | Area | Summary |
|---|---|---|---|
| [~] E-ENC1 | Major | input | Encoder SMP race fixed 2026-08-29 (atomics replace interrupt masking); the read-then-clear window is narrowed from every movement poll to ~1 per 8000 counts, not closed — closing it needs the driver's watch-point ISR and bench time |
| [~] E-DEAD1 | Smell | all | Dead-code batch — `parse_browse_response`, the `shared_packet_handler` fossil and the demo page trio deleted 2026-08-29; the caller-less `inter_mcu_*`/`pcnt_*` API surface and `window_manager.cpp` still open |
| [ ] E-ARCH1 | Smell | arch | `components/ui` ⇄ `main` dependency cycle blocks host-testing the UI |
| [ ] E-MISC1 | Smell | all | Smaller items batch (fake diagnostics metric, dummy meter data, stack copies, doc drift) |
| — SPI-1..5 | Gate | comm | SPI-link revival blockers — must be fixed before `WAVEX_SPI_LINK_ENABLED=1` (§7) |

---

## 3. Critical

None open. All six Criticals from this review were fixed on 2026-08-29; see `CHANGELOG.md`.

---

## 4. Major

### E-ENC1 — encoder deltas dropped/replayed under SMP

Two stacked defects in the primary control:

1. `main/pcnt_task.cpp:336-341` — `pcnt_consume_delta` "atomically" fetch-and-clears with `portSET_INTERRUPT_MASK_FROM_ISR()`, which masks interrupts **on the current core only** (guide §1). The producer is `pcnt_task` (unpinned, prio 5) doing an unguarded RMW `reading->delta += delta` (:219); the consumer is `ui_task` on core 1. Concurrent execution loses increments (consumer's 0-write overwrites a concurrent `+=`) or replays them. The comment ("to avoid ISR race") is wrong twice — there is no ISR, and it isn't atomic cross-core.
2. `pcnt_task.cpp:199-223` — hardware counts arriving between `pcnt_unit_get_count` and `pcnt_unit_clear_count` are destroyed; and both calls' `esp_err_t` results are ignored, so a failed clear re-applies the full count as fresh delta every 2 ms poll thereafter.

**Fix**: make `delta` a `std::atomic<int32_t>` (`fetch_add` / `exchange(0)`); stop clearing the hardware counter — track `last_hw` and compute `delta = hw - last_hw` (free-running within ±INT16 at a 2 ms poll never wraps); check the driver returns. Consider folding the 500 Hz poll into the UI task's own loop (sole consumer, 31 Hz) or PCNT watch-point callbacks (E-TASK1).

**Partly fixed 2026-08-29.** Defect 1 is closed: `__atomic_fetch_add`/`__atomic_exchange_n` replace the interrupt masking (builtins rather than `std::atomic` because `pcnt_task.h` is `extern "C"`; same precedent as `file_browser.cpp`). Both `esp_err_t` returns are now checked, and a failed clear no longer zeroes the baseline — which was the bug that re-applied the whole count as fresh delta on every later poll.

**Still open — the read-then-clear window is narrowed, not closed.** The review's suggested fix (free-run and never clear) is not safe as written: the unit is configured `high_limit = INT16_MAX` / `low_limit = INT16_MIN`, and the `pulse_cnt` driver **resets the count to zero on reaching either limit**, so a free-running counter produces one large bogus delta per ±32767 counts rather than never wrapping. Instead the counter is now re-centred only when it passes ±8000, so the lossy window went from *every poll during movement* to roughly one per 8000 counts (~85 revolutions), where losing a fraction of a detent is imperceptible. Closing it properly means the driver's watch-point callbacks — an ISR, needing an IRAM-safety audit and bench time.

---

## 5. Minor

### E-SEQ1 — WITHDRAWN, not a defect

The finding claimed the 65535 -> 1 wrap takes the reboot-resync branch, giving a spurious "peer reboot detected" and a resync count per 64K frames. **That is wrong, and the code is correct as written.**

Accepting seq 65535 sets `expected_seq_ = seq + 1`, which truncates to 0 in `uint16_t`. The next comparison is `expected_min = (expected_seq_ > kReorderTolerance) ? (expected_seq_ - kReorderTolerance) : 1`, and with `expected_seq_ == 0` that falls to the `: 1` branch — so seq 1 is not below `expected_min` and is plainly `Accept`ed. The fallback written for startup happens to cover the wrap too. Confirmed by running the tracker through a full 1..65535 cycle: `Accept`, `ResyncCount() == 0`.

It is also **already regression-tested** — `sequence_tracker_test.cpp` walks to 65535, wraps, and asserts both `ResyncCount()` and `OutOfOrderCount()` are zero, plus a second test for a sender that fails to skip 0 on wrap. The finding was reasoned from the source without checking the existing suite.

The secondary claim — a stale pre-wrap frame arriving *after* the wrap is accepted and drags `expected_seq_` back up — is arithmetically true but needs frame reordering, which a point-to-point UART does not produce; frames arrive in transmission order. It would matter only on a transport that can reorder.

**Do not "fix" this.** Changing `expected_min` to make the wrap look more symmetrical is what would actually break it.

### E-CFG1 — hardware-truth pass: what was fixed, and what is deliberately still open

**Fixed 2026-08-29** — everything decidable from source alone:

- `hardware_config.h`'s inter-MCU dependency guard tested five macro names that do not exist (`WAVEX_ENCODER_PCNT_ENABLED`, `WAVEX_PCNT1_ENABLED`, `WAVEX_4067_MUX_ENABLED`, `WAVEX_TCA8418_BUTTON_MATRIX_ENABLED`, `WAVEX_USB_MIDI_ENABLED`). An undefined identifier in `#if` is 0, so the guard silently covered only the audio engine and the LCD. Now uses the real `WAVEX_ESP_*` names, and still passes.
- `pin_config.h` claimed the assignments were "VERIFIED for ESP32-S3-DevKitC-1" and headed the block "ESP32-S3 Frontend", on a P4 target. Now says P4 and says plainly that the assignments are unverified against this board.
- `WAVEX_VALIDATE_ESP_PIN` capped at 48 (an S3 number) and would have rejected this file's own SPI pins 49-51. Raised to the P4's GPIO54.
- `WAVEX_ESP_SPI_CLK_HZ 4000000` was commented "10 MHz".
- The five `WAVEX_TCA8418_*` macros had no users while the code hardcoded its own values. Rather than adopt the macros' numbers and change behaviour blind, **the macros were set to what the firmware actually runs** (8x10 matrix, priority 5, stack 4096) and the call sites now use them. The I2C address moved out of `ui_task.cpp` into `WAVEX_TCA8418_I2C_ADDR`. Behaviour is unchanged; the header is now the single source it was supposed to be.
- Pin values in comments — `ui_task.cpp` said "GPIO31", `tca8418_keypad.h` said "e.g., 52", and `pin_config.h` says 30 — removed. AGENTS.md forbids exactly this, and both copies were wrong.

**Still open, and needs a schematic rather than a guess:**

- `WAVEX_ESP_PCNT1_A/B` (46/47) collide with `WAVEX_ESP_SPI2_SCLK/MOSI` (46/47). PCNT1 is enabled and claims those pins at boot; no SPI2 driver exists yet, so nothing fails today, and it detonates at TLC5947/MCP3008 bring-up.
- The keypad matrix is configured 8x10 because that is what the code has always passed. `WAVEX_TCA8418_COLUMNS` now records it, but nobody has checked how many columns are wired. Getting this wrong silently stops a column being scanned.
- The board-availability comment (`pin_config.h`) excludes assigned pins 6, 14, 15, 34 and 40.

## 6. Smells / cleanup batch

### E-DEAD1 — dead code (all grep-verified against the whole repo)

- **`main/comm/shared_packet_handler.{h,cpp}`** — orphaned fossil: not in `main/CMakeLists.txt` SRCS, excluded from tests, calls `ProtocolHandler` functions that don't exist (cannot compile), and describes a pre-unified wire format (the "competing message format" AGENTS.md rule 4 forbids). `inter_mcu.cpp:7` includes the header for nothing. Delete all three references.
- `inter_mcu.h:156` declares `inter_mcu_toggle_inversion` — defined nowhere (undefined-reference trap); `inter_mcu_toggle_debug` defined but declared/called nowhere; `inter_mcu_send_test_messages`, `inter_mcu_process_packet_data` (books every byte as type 0xFF), `inter_mcu_set_suspended`, `uart_link_stop` — caller-less.
- `ui_task.cpp`: `MAX_REFRESH_INTERVAL_MS` unused; `updatePeakHoldL/R` byte-identical twins; includes `links/esp_spi_link.h` though the link is compiled out (also included by `ui_diagnostics_page.cpp:19`).
- `pcnt_task.cpp`: `pcnt_get_reading`/`pcnt_get_raw_count`/`pcnt_reset_counter` caller-less; `prev_count`/`count` bookkeeping is immediately zeroed, so `pcnt_get_reading` can only return `{0,0,…}`.
- `file_browser.cpp:699-779` `parse_browse_response` (~80 lines, no callers); `statistics.h:195` `m_sample_status_lock` initialized, never used; `esp_uart_link.cpp:87-94` silent fallback `dummy_router` (prefer abort-on-missing-injection); `config.h:36` `WAVEX_ESP32_CONFIG_INCLUDED` has no readers.
- UI legacy: `common/window_manager.cpp` (no external callers, plus a real `lv_pct` arithmetic bug at :60,:187 for whoever revives it); `SoftkeyBar::focusNext/pressFocused` — the encoder-drives-softkey-focus model in `docs/ui-architecture.md` was never wired; demo trio `ui_param_edit.cpp`/`ui_patch_list.cpp`/`ui_demo.h`; `ui_sample_detail.cpp` shows hard-coded "44.1 kHz / 2:34" and appears unreachable.

### E-ARCH1 — `components/ui` ⇄ `main` cycle

`components/ui/CMakeLists.txt` `REQUIRES … main`; UI sources include `inter_mcu.h`, `ui_task.h`, `comm/i_comm_interface.h` directly, and diag/edit/keyboard pages call `inter_mcu_*` free functions, growing the coupling. This blocks host-testing the UI component and is stronger than `docs/ui-architecture.md` admits (its `UISharedContext` injection is the right fix). Also `i_comm_interface.h:29-40` hand-duplicates the callback typedefs from `inter_mcu.h`; `CommInterfaceImpl::sendSampleLoadRequest` unconditionally returns `ESP_ERR_INVALID_ARG`; `sendSamplePlayRequest` silently drops the loop-gap semantics.

### E-MISC1 — smaller items batch

- Fabricated diagnostics metric: `ui_diagnostics_page.cpp:751-774` invents CPU% from task counts + heap pressure (dead under the current `WAVEX_CPU_USAGE_METHOD 1`, but a diagnostics page must never ship a synthesized path).
- `uart_task` carries ~4.5 KB of per-loop stack copies of data already in static storage (`esp_uart_link.cpp:170-172,263`); transmit-in-place would remove them.
- `inter_mcu.cpp:724` heap-allocates a `std::vector` per browse request, and `<vector>` is included only under `ESP_PLATFORM` while the use is unconditional — the non-ESP branch of the TU can't compile (masked because tests exclude the file). A fixed array suffices.
- `usb_midi_task.cpp:55` hardcodes USB serial "0001" — two units collide in DAW port persistence; derive from the eFuse MAC.
- `midi_task.cpp:108` installs the UART driver with no event queue, so RX-FIFO overflow/framing errors are invisible; an event queue would make byte loss observable.
- Comment/code drift: `wavex_application.cpp:142` ("2 second loop" vs 1 s); `include/ui/ui_sample_edit_page.h:19-23` still calls GAIN/LOOP "inert" though the .cpp wires them *(current branch)*; `docs/ui-architecture.md` shows a `std::vector` softkey API vs the actual `std::array`.
- Recurring UI patterns worth one cleanup pass: `box()`/`label()` helpers and near-identical palettes re-declared privately in four files instead of `ui_theme`; list pages rebuild the whole widget tree per encoder detent; `refreshLinkTab` re-sets 21 table cells (strdup churn) every 500 ms; `SoftkeyBar` heap-allocates a `std::function` per press; `adaptiveRefreshControl`'s `lv_refr_now` duplicates the lvgl_port task's own refresh loop.
- `main.cpp:15-20` bring-up debris (`ESP_LOGE` banner + printf/fflush).

---

## 7. SPI-link revival gate

`link_config.h:17-21` records the 2026-07-05 decision: UART is the transport of record, `WAVEX_SPI_LINK_ENABLED=0`, and all of `esp_spi_link.cpp` compiles out. The quarantined code retains defects that **must block** flipping that flag (add these to the roadmap item when SPI revival is scheduled):

| ID | Blocker |
|---|---|
| SPI-1 | `esp_spi_link.cpp:569-588,611-627` — transaction descriptors and RX buffers are memset/reused while the driver still owns them; on `spi_slave_get_trans_result` timeout the queued transaction is never retrieved. With `queue_size=3` and an idle peer this is guaranteed. Violates guide §7/§12. |
| SPI-2 | The live SPI RX path (:681-686) routes on CRC alone — `handle_large_packet` (the only `SequenceTracker` caller) is itself dead code. Re-enabling SPI reintroduces the old-C4 no-seq-gating defect. |
| SPI-3 | `:273-276` — uninitialized `payload_size` passed as in/out capacity to `ParseWaveXPacket` → up to ~2 KB copied into a 220-byte stack array. Initialize to `sizeof(payload)`. |
| SPI-4 | `:98,107,765` — TX seq is `uint8_t`, wrapping 255→0; 0 is rejected by the peer's tracker → one guaranteed dropped message per 256 plus resync noise. (UART fixed this; SPI wasn't updated.) |
| SPI-5 | `:639` — `trans_result->length` (configured bits) used instead of `trans_len` (actual bits): short/aborted transfers undetectable except by CRC luck. |

Also: `spi_post_trans_cb` runs in the SPI ISR with `intr_flags = 0` (not IRAM) — acceptable, but comment it so a future `ESP_INTR_FLAG_IRAM` doesn't sail past unaudited.

---

## 8. Prior-review (2026-07-05) status, ESP32 items

- **C3 (send-wrapper error inversion): fixed** (`inter_mcu.cpp:112` et al.), one log-only residual in a caller-less function (E-PROTO1).
- **C4 (UART unprotected): fixed on the live path** — `SequenceTracker` instantiated at `esp_uart_link.cpp:81` and gating every CRC-valid frame (:132-148). The SPI half of C4 persists in quarantined code (SPI-2).
- **Seq-wrap-through-zero (old M4): fixed** at both UART generators.
- **packet_router unchecked `memcpy`**: fixed — `CopyMessage` bounds copies; wire-driven counts are validated before the `reinterpret_cast`s in the chunk handlers.
- **Checklist items verified clean this pass**: no first-party ISRs (no IRAM audit needed); UART uses the non-DMA driver (no app-level cache maintenance owed); SPI DMA buffers correctly `heap_caps_aligned_alloc(64, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)`; exceptions/RTTI off with no reliance; no steady-state heap churn in reviewed hot paths; `FrameScanner`/parsers robust against hostile input; `log_ring.cpp` is exemplary (correct spinlock use, drop-oldest with gap markers, honest capacity math).

## 9. Test-coverage note

The host suite (79 ESP32 tests) covers units, not the seams where this review's bugs live: nothing exercises the LVGL locking contract, listener register/deregister lifecycles, `pcnt_consume_delta` concurrency, or the TCA8418 decode. The keypad decode (E-KEY2) and the browser index fix (E-BRWS1) are cheaply host-testable once written against the existing mocks; the locking/lifetime fixes are best guarded by a debug-build `lvgl_port` lock-owner assert on the device.
