# WaveX ESP32-P4 Code Review — 2026-08-29

**Scope**: the ESP32-P4 frontend only — `firmware/esp32/main`, `firmware/esp32/components/ui`, the ESP32 build files (`CMakeLists.txt`, `sdkconfig.defaults`), and `firmware/shared/*` as consumed by the ESP32 side. The Daisy tree is out of scope (see `code_review_20260705.md` for the last full pass).
**Standard**: `skills/esp32p4/SKILL.md` → `docs/esp32p4_coding_guide.md` (§14 checklist), plus AGENTS.md constraints and `docs/ui-architecture.md`.
**Method**: four parallel subsystem reviews (core app/tasks, comm/links, input peripherals, LVGL UI) reading every first-party line, with all dead-code claims grep-verified against the whole repo. Every Critical and Major finding below was then independently re-verified against source (including the `esp_tca8418` and `esp_lvgl_port` managed components and the Waveshare BSP) before inclusion. No device build or hardware test was run for this review; nothing below depends on one, but E-KEY1/E-KEY2 predict hardware behaviour that should be confirmed on the bench.
**Branch**: `feature/sequencer-voice-audition` at `41013e1`.

Findings carry stable IDs (`E-…`) so implementation can be tracked in this file. **Completed items leave this document** — detail goes to `CHANGELOG.md`, matching the roadmap's convention — so what remains here is always the open list. A partially-addressed item keeps its row, marked `[~]`, and says what is left.

**Already remediated** (see `CHANGELOG.md` § Unreleased): E-LVGL1/2/3, the LVGL thread-safety cluster (`21304be`, `222b2b4`); E-LIFE1/2/3, the callback-lifetime cluster (`41f4cdd`); E-KEY1/2, the keypad decode and INT busy-spin (`cdab47d`); E-INIT1; E-BLD1/2, the two misleading build blocks. All fixed 2026-08-29. **E-KEY1/2 and E-ENC1 change hardware behaviour and are the ones most needing a bench pass** — they were diagnosed entirely by reading code and the controller datasheet.

---

## 1. Executive summary

The live UART transport story has improved a lot since the 2026-07-05 review: the send-wrapper error inversion (old C3) is fixed, `SequenceTracker` now gates every CRC-valid frame on the UART RX path, frame scanning/parsing enforces capacity contracts, and there are no app-level DMA/cache or IRAM-ISR hazards anywhere in the ESP32 tree (no first-party ISRs exist; UART uses the non-DMA driver; the SPI link's DMA buffers are correctly allocated but the whole link is compiled out).

The debt is concentrated in three themes:

1. **LVGL thread-safety was systematically violated** — ~~the UART RX task mutating widgets directly in comm callbacks, the whole input dispatch path running outside the port lock, and `lv_async_call` issued from the wrong task~~. **Fixed 2026-08-29**; see `CHANGELOG.md`. Kept in this list because it is the reason themes 2 and 3 matter more than they look: the corruption this caused was the most likely explanation for "random" UI failures, so misbehaviour that survives these fixes is now much more likely to be one of the remaining items than a mystery.
2. **Callback lifetime was unmanaged** — ~~listener pairs as unsynchronized globals, a file browser that never deregistered, and four different locking disciplines across four slots in one class~~. **Fixed 2026-08-29**; see `CHANGELOG.md`. What remains of this theme is the *publication* half: comm-driven pages still solve the producer/consumer handoff three different ways — correct `__atomic` release/acquire (file browser), atomics (sample browser, converted), and `volatile` (the sample-edit page, a guide-§9 regression). That is E-SYNC1.
3. **The physical control surface had real functional bugs** — ~~a keypad that either never saw a key or busy-spun core 1, decoding presses as releases, and an encoder using interrupt masking as cross-core synchronization~~. **Fixed 2026-08-29**; see `CHANGELOG.md`. These were found by reading code and the TI datasheet, not by observing hardware, so they are the highest-value items in this review to confirm on the bench: if the keypad still misbehaves, the remaining suspect is the CFG register the vendored driver never writes.

Two systemic build findings rounded it out — ~~inert `-Os`/LTO options added after `project()`, and an `EXCLUDE_COMPONENTS` list that excluded nothing~~. **Fixed 2026-08-29**, and the fix confirmed both diagnoses: a clean rebuild came out within 48 bytes of the old image, which is what it should be if the options really were applying to nothing and the pruned exclusions really were being built anyway.

**Suggested order**: all Criticals are now closed. Next is the UI correctness batch (E-TICK1, E-TOUCH1, E-BRWS1, E-MENU1) — mostly small, independent, and E-TICK1/E-TOUCH1 in particular are deletions of code that duplicates what the BSP already does. Then E-TX1 and E-METER1, then the Minor/Smell batches. E-SYNC1's remaining half (the sample-edit page's `volatile`) is cheap and can ride along with any edit-page work. The SPI findings (§7) do not need fixing now but must gate any re-enable of `WAVEX_SPI_LINK_ENABLED`. **Before any of that, a bench pass on the keypad and encoder** — three fixes now depend on hardware behaviour nobody has watched.

---

## 2. Tracking index

| ID | Sev | Area | Summary |
|---|---|---|---|
| [~] E-ENC1 | Major | input | Encoder SMP race fixed 2026-08-29 (atomics replace interrupt masking); the read-then-clear window is narrowed from every movement poll to ~1 per 8000 counts, not closed — closing it needs the driver's watch-point ISR and bench time |
| [~] E-SYNC1 | Major | UI/core | `volatile`/plain-`bool` cross-task handoffs — sample browser converted to atomics 2026-08-29; edit page (`volatile`) and `ui_task.h` meter state still open |
| [ ] E-TICK1 | Major | UI | LVGL time runs at 2×: duplicate 5 ms tick timer |
| [ ] E-TOUCH1 | Major | UI | Second GT911 instance created on the BSP-owned touch controller, wrong geometry |
| [ ] E-BRWS1 | Major | UI | Browser tap selects the wrong entry once the list has scrolled |
| [ ] E-TX1 | Major | comm | Outbound UART frames wait for the 10 ms poll tick; one frame drained per tick |
| [ ] E-MENU1 | Major | UI | Menu fallback handler destroys the list mid-event-dispatch |
| [ ] E-DIAG1 | Major | UI | `malloc` + `vTaskGetRunTimeStats` every 500 ms in the shared esp_timer task |
| [ ] E-METER1 | Major | core | Orphaned meter pipeline forces continuous full-display refreshes |
| [ ] E-STOP1 | Major | all | Every `stop()`/teardown API is unsafe (vTaskDelete over held locks / blocked queues) |
| [ ] E-MIDI1 | Minor | shared | MIDI parser: stale `pending_system_data_` can swallow the next message's data bytes |
| [ ] E-PROTO1 | Minor | comm | Router logs non-NUL-guaranteed wire string; silent length truncation in sample-data send |
| [ ] E-STAT1 | Minor | comm | Packet statistics misclassify all 0x30-block traffic; dead type-name table is shifted |
| [ ] E-SEQ1 | Minor | shared | `SequenceTracker` non-modular compare misbehaves at 64K wrap |
| [ ] E-CFG1 | Minor | config | Hardware-truth pass: pin_config contradictions, unused TCA8418 macros, broken guard |
| [ ] E-INQ1 | Minor | UI | Input queue drops are silent and uncounted |
| [~] E-UIM1 | Minor | UI | Browser leak on failed create fixed 2026-08-29 (same function as the E-LIFE2 deregistration); `loading_row` ABA and `uint8_t` page-start truncation still open |
| [ ] E-KBD1 | Minor | UI | Keyboard page "All Off" leaves latched pads lit (current branch) |
| [~] E-LOG1 | Minor | all | Hot-path log storms on the UART task — per-entry browse INFO and the statistics mutex-trace lines removed 2026-08-29 (they became a UI stall once the listener mutex spanned the callback); hex dumps and remaining per-packet INFO still open |
| [ ] E-SDK1 | Minor | build | Watchdog/assert posture: INT WDT 5 s, task WDT off, assertions compiled out |
| [ ] E-STD1 | Minor | build | C++ standard not pinned anywhere (guide §8 requires it) |
| [ ] E-VER1 | Minor | core | Duplicate version truth (`version.h` vs root `VERSION`); `__DATE__`/`__TIME__` |
| [ ] E-DEAD1 | Smell | all | Dead-code batch (grep-verified) incl. `shared_packet_handler` fossil |
| [ ] E-ODR1 | Smell | UI | Duplicate-symbol landmine (`ui_main.cpp`/`ui_api.cpp`); stale `waveform_view.h` copy |
| [ ] E-ARCH1 | Smell | arch | `components/ui` ⇄ `main` dependency cycle blocks host-testing the UI |
| [ ] E-TASK1 | Smell | docs | No ESP32 task table; ad-hoc inline priorities/stacks; polling where events belong |
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

### E-SYNC1 — `volatile`/plain-`bool` cross-core handoffs (guide §9 ban)

The repo already contains the correct pattern — `file_browser.cpp:169-186` uses `__atomic_*` release/acquire, citing "dma-timing-review Finding 11" — but newer code regressed:

- `include/ui/ui_sample_edit_page.h:83-101` *(current branch)* — `volatile uint32_t run_epoch_; volatile bool run_ready_; …`: `handleEnvelopeChunk` (UART task) fills `run_columns_` then sets `run_ready_`; `serviceUi` (LVGL task) consumes. `volatile` provides no inter-core ordering on the P4 — the consumer can see the flag before the column data.
- `include/ui/ui_sample_browser.h:181-186` — deferred-update flags and buffers are plain `bool`s; additionally `pending_metadata_entry_` points into `file_browser_->entries[]`, which the UART task rewrites during pagination while the UI task formats it (torn text).
- `main/ui_task.h:73-94` — `volatile` meter floats + `meter_callback_data_valid`, and plain `content_changed`, written from the uart/esp_timer tasks and read on core 1.

**Fix**: one shared helper implementing the file-browser pattern (`std::atomic` flag with release store/acquire load around a snapshot buffer); use it in all three places; copy metadata out of `entries[]` at publication time.

### E-TICK1 — LVGL time runs at 2×

`src/display_manager.cpp:77,254-268` starts a 5 ms `esp_timer` calling `lv_tick_inc(5)` — but `bsp_display_start_with_config` → `lvgl_port_init` already runs its own 5 ms tick (`esp_lvgl_port.c:293`, default `timer_period_ms = 5` from `ESP_LVGL_PORT_INIT_CONFIG()`, confirmed in `esp_lvgl_port.h:72`). Every animation, `lv_timer` period, long-press threshold, and the BusyOverlay timeout runs at double speed; the edit page's 50 ms service timer effectively runs at 25 ms. `lv_init()` at :115 likewise duplicates lvgl_port's own init. **Fix**: delete `startLvglTick`/`stopLvglTick` and the explicit `lv_init()`.

### E-TOUCH1 — second GT911 driver instance on a BSP-owned controller

`src/display_manager.cpp:76,148-252`: the BSP already created and registered a GT911 indev inside `bsp_display_start_with_config` (`esp32_p4_nano.c:807,816` — verified). `initTouchController` then hard-resets the chip via the RST GPIO while the live indev polls it, creates a second `esp_lcd_touch` handle on the same address with wrong geometry (`x_max=800, y_max=480` on a 720×1280 panel), and registers it nowhere (grep: no `lvgl_port_add_touch`/indev use of `touch_handle_` in first-party code). Dead weight that can glitch working touch during init. **Fix**: delete `initTouchController`.

### E-BRWS1 — tap selects the wrong entry once the list has scrolled

`components/ui/components/file_browser.cpp:604-613`: the click handler derives the entry index by scanning the list's children from 0, but rows are created only for the visible window (starting at `first_visible_index`, cf. the scroll logic at :391-408) and the real entry index is already stored on each button (`lv_obj_set_user_data(btn, (void*)(uintptr_t)i)`, e.g. :1152) — and ignored. Scrolled lists resolve taps to the wrong directory entry (entering the wrong folder / auditioning the wrong sample); the pagination spinner row shifts indices further. **Fix**: `entry_index = (uintptr_t)lv_obj_get_user_data(btn);`.

### E-TX1 — outbound frames wait for the 10 ms event-poll tick

`main/links/esp_uart_link.cpp:205,263-264`: `uart_link_send()` only enqueues; nothing wakes `uart_task` (it sits in `xQueueReceive(events, 10 ms)`), and at most **one** TX frame is serviced per loop pass. A queued note-on waits up to ~10 ms; a full 8-deep queue drains at ~80 ms. For the sequencer/audition path where note timing is the product, that is audible nondeterministic jitter (guide §10). **Fix**: wake the task on send (post a user event to the same queue, or task notification + wait-on-both) and drain the whole TX queue per wake.

### E-MENU1 — menu fallback activates synchronously during event dispatch

`src/ui_menu_page.cpp:142-155`: the list-level `LV_EVENT_CLICKED` fallback calls `activateSelection()` directly; a menu action pushes a page → `lv_obj_clean(content_)` deletes the list *while its event is being dispatched*. The per-item handler directly above (:104-108) defers via `lv_async_call` with a comment explaining exactly this hazard. The fallback's match condition (checks `LV_OBJ_FLAG_EVENT_BUBBLE`, selecting item *i* regardless of the row hit) is also wrong. **Fix**: delete the fallback or make it defer like the item handler.

### E-DIAG1 — heavy work in the shared esp_timer task

`src/ui_diagnostics_page.cpp:530-532,587-594`: `diagnosticsUpdateCallback` (500 ms esp_timer) does `malloc(2048)` + `vTaskGetRunTimeStats()` (suspends the scheduler to walk/format every task) in the esp_timer task — the same task delivering LVGL ticks and the 33 ms meter timer (guide §11). **Fix**: flag from the timer, collect in the page's LVGL timer or a low-prio worker, reuse a static buffer.

### E-METER1 — orphaned meter pipeline forces continuous refreshes

`main/ui_task.cpp:122-134,196-258,678-683`: `createMeterDisplay` has zero callers (grep-verified), so `lvglMeterApplyCallback` — the only code clearing `meter_update_pending` — never runs; meanwhile `meterUpdateCallback` fires every 33 ms unconditionally, and while the Daisy streams meter packets it marks content changed every tick, driving `adaptiveRefreshControl()` to `lv_refr_now` at the ~16 ms cap even when nothing on screen changed (and once per second with no meter data at all). Related: the startup call injects fake meter values (`meterDataCallback(0.5f, 0.4f, 0.8f, 0.7f, …)`, :98-100), and the listener registered at :95 is never unregistered (see E-STOP1). **Fix**: create the timer only when a meter view exists; gate `markContentChanged()` on the widgets existing; delete the dummy data.

### E-STOP1 — every teardown API is unsafe (latent; all currently caller-less, grep-verified)

A single pattern repeated across the tree: `vTaskDelete(handle)` on a task that may hold a lock or be blocked on an object that is then freed.

- `ui_task.cpp:159-185,711-721` — kills the UI task possibly inside `LV_LOCK()` (permanently wedging the LVGL port task); deletes the instance while the meter listener still points at it.
- `links/esp_uart_link.cpp:456-478` — `xTaskNotifyGive` does not wake a task in `xQueueReceive`; then deletes the queue/mutex/driver the task may be blocked on.
- `tca8418_keypad.cpp:154-157` — can kill the task mid-I2C transaction, leaking the shared touch/keypad bus mutex → touch dies permanently.
- `usb_midi_task.cpp:145-148` — TinyUSB still live; `tud_midi_rx_cb` can notify the deleted handle. `midi_task.cpp:152-159`, `pcnt_task.cpp:273-280` — same shape.

**Fix (one pattern everywhere)**: signal the task, let it release resources and self-delete, join via notification, then free. These are public APIs; the first caller gets a heisencrash.

---

## 5. Minor

### E-MIDI1 — parser: stale `pending_system_data_` swallows the next message

`firmware/shared/midi/midi_stream_parser.hpp:56-79`: the status-byte branch never resets `pending_system_data_`. A System Common message whose data bytes were lost (cable glitch) followed by `90 3C 64` has its data bytes consumed as leftover system data — the note never sounds. One line: zero it on any status byte (and in the `F0` branch). The rest of the parser is solid (running status, realtime transparency, SysEx skip all verified correct).

### E-PROTO1 — router/format hardening

- `main/comm/packet_router.cpp:387`: `%s` on `msg.msg` without forcing NUL termination — a corrupt 48-byte `ErrorMessage` reads past the struct. Set the last byte to `'\0'` first.
- `inter_mcu.cpp:794-806`: `size_t length` silently truncates through the `uint16_t` parameter — `65536+n` sends `n` bytes and returns `ESP_OK`. Range-check before the cast (same latent cast at :734).
- `inter_mcu.cpp:371-376`: residual old-C3 instance — `if (result)` treats `-1` as success (log-only, function has no callers).
- ~10 sites return `-1` with a comment claiming `ESP_ERR_INVALID_STATE` (e.g. `inter_mcu.cpp:87,145,158,174,188,280,399,712`) — return the named constant so callers can distinguish not-initialized from send-failed.

### E-STAT1 — statistics misclassification

`main/comm/statistics.cpp:100-103`: every 0x30-block message (BROWSE_RESP, SAMPLE_STATUS, STORAGE_STATUS, SAMPLE_META, CV_*) counts as `unknown_packets`, so the most frequent live traffic reads "unknown" in diagnostics; the comment misstates `MSG_DATA_REQUEST` as 0x0C (enum: 0x0B). `get_packet_type_name` (:324-361) is both dead (no callers) and shifted one type off across the board — delete or fix.

### E-SEQ1 — `SequenceTracker` 64K-wrap edge

`firmware/shared/spi_protocol/sequence_tracker.hpp:57-71` compares absolutely, so 65535→1 takes the reboot-resync branch (spurious resync per 64K frames) and a stale pre-wrap frame post-wrap is accepted and drags `expected_seq_` backwards. Self-recovering; fix with serial-number arithmetic or document.

### E-CFG1 — hardware-truth pass (pin/flag truth violations)

All in the two files that are supposed to be the single source of truth, or contradicting them:

- `pin_config.h`: header still says "ESP32-S3 … VERIFIED for ESP32-S3-DevKitC-1" (:12,:21); `WAVEX_ESP_PCNT1_A/B` 46/47 (:63-64) collide with `WAVEX_ESP_SPI2_SCLK/MOSI` 46/47 (:94-96) — PCNT1 claims them at boot, detonating at TLC5947/MCP3008 bring-up; the board-availability comment (:28) excludes assigned pins 6/14/15/34/40; `WAVEX_VALIDATE_ESP_PIN` caps at 48 (:199) while pins 49–52 are assigned (and the macro has no users); `WAVEX_ESP_SPI_CLK_HZ 4000000` commented "10 MHz" (:180).
- `hardware_config.h:683-688`: the dependency guard tests macro names that don't exist (`WAVEX_ENCODER_PCNT_ENABLED`, `WAVEX_TCA8418_BUTTON_MATRIX_ENABLED`, …) — they expand to 0, so the guard protects almost nothing.
- Keypad ignores its config: `tca8418_keypad.cpp:118` `hw_init(8, 10)` vs `WAVEX_TCA8418_ROWS/COLUMNS` 8×8; :142 hardcodes prio 5/stack 4096 vs the unused `WAVEX_TCA8418_TASK_PRIORITY`/`_STACK_SIZE` macros; `ui_task.cpp:105` hardcodes I2C address `0x34` (no macro exists).
- Pin values in comments — the thing AGENTS.md forbids — and both wrong: `ui_task.cpp:102-104` says "GPIO31", `tca8418_keypad.h:9` says "e.g., 52"; `pin_config.h:88` says 30. Delete the comments.
- `uart_debug_config.h:16`: `#define WAVEX_UART_DEBUG_LEVEL 2  // Enable INFO level` — 2 is WARN; INFO logging is actually compiled out (good for the hot path, but the comment lies).

### E-INQ1 — silent input drops

`src/input_dispatcher.cpp:26-29`: `post()` drops on a full queue with no counter; all callers ignore the result. Guide §13/§14 want dropped-event accounting — add a counter and surface it on the diagnostics page.

### E-UIM1 — file-browser small bugs

Leak on failed create (`file_browser.cpp:281-284` — `browser`/`entries`/container leak when `comm_interface` is missing); `loading_row` ABA (`:1265` cleans the list without nulling the pointer; `fb_show_loading_row` later deletes whatever object reuses the address); `uint8_t next_start_index` (`:1178`) wraps past 255 entries — unreachable at today's 50-entry cap, silent when raised.

### E-KBD1 — "All Off" leaves latched pads lit *(current branch)*

`pages/ui_keyboard_page.cpp:206`: the softkey calls `releaseAll()` which clears `pad_down_` but never `refreshLabels()` — pads stay green with no sound. The Latch-off path (:189-191) gets it right.

### E-LOG1 — hot-path log storms on the UART task

`file_browser.cpp:723-729` hex-dumps 64 bytes as ~64 `ESP_LOGI` lines plus one INFO per parsed entry per browse page; `statistics.cpp:425-433` logs "=== About to acquire mutex ===" per browse response; `statistics.cpp:478-492` four INFO lines per sample-status; `packet_router.cpp` per-packet INFO. Seconds of UART-task stall per directory listing; route through the existing log gates at DEBUG.

### E-SDK1 — watchdog/assert posture

`sdkconfig.defaults`: `CONFIG_ESP_INT_WDT_TIMEOUT_MS=5000` (default 300 — hides exactly the critical-section bugs §14 checks for), `CONFIG_ESP_TASK_WDT_INIT=n` (a hung polling task — three exist — freezes the instrument silently), `CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_DISABLE=y`, `PANIC_PRINT_HALT`. Fine on the bench; record it as a deliberate dev-only posture and revisit before hardware sign-off.

### E-STD1 — C++ standard not pinned

No `-std`/`CXX_STANDARD` anywhere in project CMake; the build rides IDF 5.5's default `gnu++2b`. Guide §8 explicitly requires recording the standard; AGENTS.md even points contributors at a declaration that doesn't exist.

### E-VER1 — duplicate version truth

`main/version.h` hardcodes 0.1.0 alongside the root `VERSION` file (already flowing in via `PROJECT_VER`); `WAVEX_BACKEND_VERSION_*` has zero users; unprefixed `STRINGIFY`/`TOSTRING` macros in a widely-included header; `__DATE__`/`__TIME__` breaks reproducible builds. Generate from `PROJECT_VER`, delete the rest.

---

## 6. Smells / cleanup batch

### E-DEAD1 — dead code (all grep-verified against the whole repo)

- **`main/comm/shared_packet_handler.{h,cpp}`** — orphaned fossil: not in `main/CMakeLists.txt` SRCS, excluded from tests, calls `ProtocolHandler` functions that don't exist (cannot compile), and describes a pre-unified wire format (the "competing message format" AGENTS.md rule 4 forbids). `inter_mcu.cpp:7` includes the header for nothing. Delete all three references.
- `inter_mcu.h:156` declares `inter_mcu_toggle_inversion` — defined nowhere (undefined-reference trap); `inter_mcu_toggle_debug` defined but declared/called nowhere; `inter_mcu_send_test_messages`, `inter_mcu_process_packet_data` (books every byte as type 0xFF), `inter_mcu_set_suspended`, `uart_link_stop` — caller-less.
- `ui_task.cpp`: `MAX_REFRESH_INTERVAL_MS` unused; `updatePeakHoldL/R` byte-identical twins; includes `links/esp_spi_link.h` though the link is compiled out (also included by `ui_diagnostics_page.cpp:19`).
- `pcnt_task.cpp`: `pcnt_get_reading`/`pcnt_get_raw_count`/`pcnt_reset_counter` caller-less; `prev_count`/`count` bookkeeping is immediately zeroed, so `pcnt_get_reading` can only return `{0,0,…}`.
- `file_browser.cpp:699-779` `parse_browse_response` (~80 lines, no callers); `statistics.h:195` `m_sample_status_lock` initialized, never used; `esp_uart_link.cpp:87-94` silent fallback `dummy_router` (prefer abort-on-missing-injection); `config.h:36` `WAVEX_ESP32_CONFIG_INCLUDED` has no readers.
- UI legacy: `common/window_manager.cpp` (no external callers, plus a real `lv_pct` arithmetic bug at :60,:187 for whoever revives it); `SoftkeyBar::focusNext/pressFocused` — the encoder-drives-softkey-focus model in `docs/ui-architecture.md` was never wired; demo trio `ui_param_edit.cpp`/`ui_patch_list.cpp`/`ui_demo.h`; `ui_sample_detail.cpp` shows hard-coded "44.1 kHz / 2:34" and appears unreachable.

### E-ODR1 — link-time landmines

`src/ui_main.cpp` and `src/ui_api.cpp` both define `wavex_ui::ui_init_demo`/`ui_set_active_context` and both are compiled — it links only because `ui_main.o` is never pulled from the archive; the first unique-symbol reference turns into a multiple-definition error. `include/ui/waveform_view.h` is a stale divergent copy of `components/waveform_view.h` (256 vs 512 points, different layout) with both dirs on the include path — one wrong `#include` from an ODR violation. Delete both.

### E-ARCH1 — `components/ui` ⇄ `main` cycle

`components/ui/CMakeLists.txt` `REQUIRES … main`; UI sources include `inter_mcu.h`, `ui_task.h`, `comm/i_comm_interface.h` directly, and diag/edit/keyboard pages call `inter_mcu_*` free functions, growing the coupling. This blocks host-testing the UI component and is stronger than `docs/ui-architecture.md` admits (its `UISharedContext` injection is the right fix). Also `i_comm_interface.h:29-40` hand-duplicates the callback typedefs from `inter_mcu.h`; `CommInterfaceImpl::sendSampleLoadRequest` unconditionally returns `ESP_ERR_INVALID_ARG`; `sendSamplePlayRequest` silently drops the loop-gap semantics.

### E-TASK1 — task architecture is undocumented and partly polling-based

Eleven tasks/contexts exist (inventory below); priorities and stacks are inline magic numbers with guess-comments; the UI task (prio 2) sits below the LVGL port task (prio 4) it contends with; no doc records the table (`docs/architecture.md` covers only the Daisy). Three tasks poll where events belong (guide §10): `pcnt_task` at 500 Hz for a 31 Hz consumer, the keypad task polls a GPIO level with the INT line wired, `ui_task` polls at 32 ms. No stack high-water measurements back the "increased" sizes (§13). **Fix**: adopt the table below into `docs/architecture.md`, move numbers to `hardware_config.h`, and convert the pollers opportunistically.

| Task | Prio | Stack | Core | Style |
|---|---|---|---|---|
| `main` (app_main) | 1 | 32768 | 0 | 1 s delay loop, logs heap/60 s |
| `uart_link` | 6 | 16384 | — | 10 ms event poll + TX drain (E-TX1) |
| `pcnt_task` | 5 | 4096 | — | 2 ms poll (E-ENC1, E-TASK1) |
| `din_midi` | 5 | 4096 | — | blocks on UART read ✓ |
| `usb_midi` | 5 | 4096 | — | blocks on task notification ✓ |
| `tca8418_task` | 5 | 4096 | 1 | 10 ms GPIO poll (E-KEY1) |
| `ui_task` | 2 | 16384 | 1 | 32 ms poll |
| LVGL port task | 4 | 7168 | — | esp_lvgl_port default |
| `log_drain` | 1 | 3072 | — | 20 ms drain ✓ |
| `scrshot` (debug) | 3 | 4096 | — | 200 ms UART read |
| TinyUSB device task | esp_tinyusb default | — | — | event queue ✓ |

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
