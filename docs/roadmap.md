# WaveX Implementation Roadmap

**Status**: Canonical implementation-order document. Read `architecture.md` first.
**Last updated**: 2026-08-31 (project-principles audit remediation mapped)

Phases are ordered by dependency, not calendar. Within a phase, items are listed in recommended implementation order. Every phase ends with the test gate that must be green before moving on.

**This document lists work that is still open.** Completed work is recorded in [`CHANGELOG.md`](../CHANGELOG.md) and in git history — it is deliberately *not* kept here, so this file stays readable as a plan rather than becoming an archive. The one exception is [§ Outstanding hardware verification](#outstanding-hardware-verification): those items are code-complete but unproven, which makes them open work, not history.

> **Trust the code, not this file.** Rationale recorded here has been factually wrong more than once, including an item marked "Done" that was not. Before acting on a claim below that something is dead, low-risk, or finished, verify it against the source.

---

## Phase 0 — Foundation Hardening

### 0.1 Toolchain upgrades still open

| Component | Pinned today | Recommendation |
|---|---|---|
| CMSIS-DSP | 1.14.4 (2023-03-10), a submodule of libDaisy | **Leave it.** Verified 2026-08-29: libDaisy `origin/master` pins the *same* commit (`3a04f817`), so we are not behind — upstream has not moved it in three years. Bumping would mean pointing a submodule somewhere its parent project does not, and every later libDaisy bump would silently revert the override unless re-applied. We also use exactly one function from it (`arm_linear_interp_q15`); `arm_scale_q15` turned out not to be in the linked set at all. Revisit only if upstream moves it, or if a Phase 4/5 kernel (polyphase FIR, offline mangling) actually needs 1.17 — and note 1.15+ moved to a standalone pack rather than the CMSIS_5 bundle, which is what would make an override awkward. **There is no libDaisy fork** — earlier guidance in this file said to bump "within our fork", which was wrong. |
| libDaisy | v8.1.0 (2026-02-23), plain submodule of `electro-smith/libDaisy`, zero local commits | On the latest **tag**; `master` is 15 commits ahead with no v8.2.0 released. Upstream is active but slow (~2–3 merged PRs/month, newest commit 2026-08-11). Nothing unreleased affects us today: the I2C4 AF fix is for `I2C_4` and we use `I2C_1` for the MCP4728; the UART-logger change is moot since we replaced the logger with `log_ring`; the TCA9534 driver is a different part from our TCA8418. **One does matter later:** `70c2b244` "two SAI blocks with no master" is the SAI2 TDM-8 path Phase 3 needs. Bump when v8.2.0 lands *and* Phase 3 is being worked — the SD soak test is required either way, since v8.1.0's own SDMMC/fatfs changes are still unverified on hardware. |
| LVGL | 9.5.0 (2026-02-18), managed component, pinned `>=9.4,<10` in `main/idf_component.yml` | **Nothing to upgrade to.** Verified 2026-08-30 against the Espressif component registry: 9.5.0 is the newest published version, and `dependencies.lock` already resolves it. The open work is not a version bump, it is that we took almost nothing 9.5 offers — see [§ 0.3](#03-lvgl-95-take-up-added-2026-08-30). Note the packaged component trims `docs/src/CHANGELOG.rst` to the 9.5.0 section only, so a 9.4-vs-9.5 diff cannot be read from the vendored tree. **No breaking-change exposure**: 9.5 removed the XML UI engine and deprecated `lv_fragment`; grep of `components/ui`, `main` and `tools/ui_preview` finds neither. |
| `espressif/esp_lvgl_port` | 2.8.0~1 | **Bump opportunistically, not on its own.** 2.9.0 exists; its entire changelog is CherryUSB input-device support (we use `esp_tinyusb`, so irrelevant) and a DPI callback rename `on_refresh_done` → `on_frame_buf_complete` that only matters once IDF moves. Fold it into the ESP-IDF pin work above so one panel check covers both. |
| ESP-IDF | **floating** `espressif/idf:release-v5.5`, currently `v5.5.4-1169-gbb2188bfb8` (2026-06-18) | **Pin to a tag.** The devcontainer follows a branch, so a rebuild can move the toolchain silently — that is worth more than any fix in the release. v5.5.5 (2026-07-17) adds `CONFIG_SPIRAM_ENC_EXEMPT` / `MALLOC_CAP_SPIRAM_NO_ENC`, hardens the JPEG decoder (GHSA-v6r2-f6p2-88cj), and disables ECDSA Secure Boot V2 on ESP32-P4. **None of those touch us** — no secure boot, no PSRAM encryption, no JPEG decoding. The per-component changelog (SDMMC / UART / MIPI-DSI / PPA, the ones that would matter) could not be retrieved, so treat the delta as unknown: pinning moves ~1 month of `release/v5.5` commits, a bigger jump than the tag name suggests. Re-run the SD soak and a panel check after. Stay on 5.5.x otherwise; Plan the 6.0 migration as a dedicated task. Three tripwires: (1) ESP32-P4 default chip revision becomes v3.0, so binaries won't boot on rev < 3.0 silicon without `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` — check the module's silicon rev first; (2) legacy drivers are removed (audit third-party components, `esp_tca8418` is fetched from git and may lag); (3) every managed component must declare 6.0 compatibility, and the waveshare BSP is the likely laggard. Budget a spike branch. |

### 0.2 Repo cleanup still open

1. **Split `daisy_spi_link.cpp`** along five boundaries, opportunistically as other work touches those files rather than as a big-bang refactor. The file mixes concerns that have no reason to share a translation unit, and the seams are clean:

   | New unit | Takes |
   |---|---|
   | `daisy_spi_transport` | DMA, hardware SPI, interrupt handlers, GPIO/CS configuration |
   | `daisy_packet_manager` | Packet creation, parsing, validation, queue management, size determination |
   | `daisy_message_processor` | Message routing/dispatch and the individual message processors |
   | `daisy_storage_bridge` | Directory browsing and filesystem integration |
   | `daisy_audio_bridge` | Sample playback integration |

   Extract in that order — transport first, since everything else depends on it and nothing depends on the bridges. `daisy_spi_link.h` stays as the assembled public interface so call sites do not move. (The boundaries were originally worked out in a plan doc that has since been deleted; they are recorded here so the item does not depend on it.)
2. **UART is the transport of record.** `WAVEX_SPI_LINK_ENABLED` is hard-coded `0`, so the SPI link is compiled out of every image and UART carries all traffic including browse pages and wave chunks. Consolidating onto SPI-only is real protocol work — extending `protocol.h` for the UART-only message types, rewiring every send call site, then hardware bring-up. Track separately if prioritized. See `architecture.md` §4.4.
3. **`esp_spi_link.cpp` has six recorded defects gating revival — SPI-1..SPI-6, full list in [`backlog.md`](backlog.md#spi-link-revival-is-gated-on-six-recorded-defects).** All found while the file is compiled out, so none are reachable today; all six must be fixed before item 2 is ever picked up, not blindly patched now. The two most recent (2026-08-30 ESP32 coding-guide review, SPI-6 and its buffer-pool sibling) are the deepest: a real ownership-tracking redesign for `spi_slave_task()`'s buffer pool (guide §7/§10), and an IRAM-safety audit of `spi_post_trans_cb`'s call graph (guide §4) before trusting its `ESP_INTR_FLAG_IRAM` behavior.

### 0.3 LVGL 9.5 take-up (added 2026-08-30)

We have been on LVGL 9.5.0 since `5da8d40` (2026-07-02), but that commit enabled exactly one thing: `CONFIG_LVGL_PORT_ENABLE_PPA`, which is `esp_lvgl_port`'s use of the PPA and whose own Kconfig help text is "Enable PPA for screen rotation." Everything else 9.5 added is still switched off. Two items are worth trying; both are cheap to revert and neither can be judged without the panel.

1. **LVGL's own PPA draw unit** — `CONFIG_LV_USE_PPA`. **Enabled, measured on hardware and turned back off, 2026-08-30. Settled — do not re-open without new information.** The measurements are below because the result was not a simple "no": it made drawing meaningfully faster and still lost overall. This is **not** the same accelerator path as `LVGL_PORT_ENABLE_PPA`: the port flag offloads display rotation, while `LV_USE_PPA` installs a draw unit inside LVGL's renderer that accelerates rectangle fills and (with `_IMG`) image blits. The source is vendored at `managed_components/lvgl__lvgl/src/draw/espressif/ppa/`. 9.5 specifically added `LV_PPA_BURST_LENGTH` (upstream PR 9612) so the burst can be matched to the P4's 128-byte cache lines. **Caution:** the draw unit is young — 9.5's own changelog carries "fix(draw/ppa): fix build and rendering issues" and a cache-alignment fix — and it stacks a second PPA consumer on top of a rotation path that has never been looked at on hardware, which makes a bad frame hard to attribute. Enable it as a measured experiment, and settle the rotation row in § Outstanding hardware verification in the same session.

   What reading the vendored source established before enabling it, and what makes this a genuine coin-flip rather than a free win:

   - **`CONFIG_LV_DRAW_BUF_ALIGN` had to go 4 → 128.** `lv_draw_ppa_private.h:40` hard `#error`s unless it equals `CONFIG_CACHE_L2_CACHE_LINE_SIZE`, which is 128 on the P4. This is not a PPA-only setting — it raises the alignment of every LVGL draw-buffer allocation.
   - **The fill path is narrower than "accelerates fills" suggests.** `ppa_evaluate()` rejects any fill with `radius != 0`, any gradient, and anything not fully opaque. Our chrome is almost entirely rounded (radius 2–6 across cards, buttons, bars and tiles), so what actually reaches the PPA is page and layer *background* fills — real work, but not the card drawing.
   - **`LV_USE_PPA_IMG` deliberately left off.** It only accepts unscaled, unrotated, unrecoloured, opaque RGB565/RGB888 variable-source images, and this firmware draws no images at all — no `lv_image_*` or `lv_canvas_*` call exists outside `tools/ui_preview`. It would be pure code size.
   - **The likeliest way this loses is cache sync, not the PPA itself.** LVGL's default `invalidate_cache_cb` is `NULL`, and `lv_draw_buf_invalidate_cache()` early-returns on NULL — so today cache invalidation costs nothing. `lv_draw_ppa_init()` installs `lv_draw_buf_ppa_init_handlers()`, which overrides the **global** default handler with an `esp_cache_msync` over `draw_buf->data_size` — the *whole* buffer, ignoring the `area` argument it is passed — and `ppa_execute_drawing()` calls it twice per task. On 720x1280 buffers with `CONFIG_BSP_LCD_DPI_BUFFER_NUMS=3` that is easily more work than the fill it replaced, and it now applies to every LVGL cache invalidation, not only PPA-accelerated ones. **If the FPS measurement comes out worse, this is the first thing to look at**, and the cheap fix is a narrowed `invalidate_cache_cb` rather than reverting the flag.
   - Static cost measured at enable time: +5,368 bytes, entirely External RAM `.text`/`.rodata`. DIRAM unchanged, so no static internal-RAM cost. The 128-byte alignment cost is runtime heap only and does not show up in `idf.py size`.

   **Result, Diagnostics page, via `scripts/sysmon_stats.py` (medians; 24-32 samples per run, so ~7-10s each and all IQRs overlapping — the direction is trustworthy, the precision is not):**

   | | PPA off | PPA on | PPA on, narrowed cache cb |
   |---|---|---|---|
   | render median | 10 ms | 9 ms | **7.5 ms** |
   | render p95 | 37 ms | 21 ms | 29 ms |
   | render max | 95 ms | 55 ms | 47 ms |
   | flush | 1 ms | 3 ms | 1.5 ms |
   | refr | 3 ms | 8 ms | 7.5 ms |
   | CPU | 18.5% | 23.0% | 21.0% |
   | FPS | 29 | 26 | 27 |

   Two things were learned, and both outlast the decision:

   - **The predicted cache-callback problem was real and is now fixed.** The middle column is the flag as upstream ships it; `flush` tripling gave it away, since a draw unit cannot make flushing slower. `wavexInvalidateCacheArea()` in `display_manager.cpp` (commit `5322cc6`) narrows the sync to the dirty rows and moved all five fields the right way. That code is retained and compiles out with `LV_USE_PPA`, so a future retry starts from the fixed version. **One edge case in it is still unverified** (found in the 2026-08-30 ESP32 coding-guide review, not exercised since `LV_USE_PPA` has been off since the measurement above): `end` is clamped to `draw_buf->data_size` before the cache-line round-up and clamped again after, so if `data_size` is not itself a multiple of `CONFIG_CACHE_L2_CACHE_LINE_SIZE`, the final `end` can come out unaligned — guide §6 warns specifically against an unaligned `esp_cache_msync()` range. Confirm `data_size` is always line-size-padded before re-enabling `LV_USE_PPA`.
   - **What remains is structural and not fixable from here.** `ppa_evaluate()` applies **no minimum-area threshold** and `lv_draw_ppa_fill.c:39` uses `PPA_TRANS_MODE_BLOCKING`, so a 4x4-pixel fill becomes a blocking hardware round-trip. Large fills win, the many small ones lose, and the sum is +14% CPU and -7% FPS despite -25% render. Adding a size threshold means editing `managed_components/`, which the component manager regenerates — the trap `docs/backlog.md` already records.

   **Retry only if** upstream adds an area threshold or non-blocking ops (`LV_PPA_NONBLOCKING_OPS` currently `#error`s as "experimental and not supported yet"), or if a page appears whose cost is dominated by a few large opaque fills rather than many small ones.

2. **Drop shadow on the card/tab chrome** — **tried 2026-08-30, froze the UI, reverted.** Do not retry without fixing the cause below.

   9.5 added native software blur and drop shadow (`lv_obj_set_style_drop_shadow_color/offset_x/offset_y/opa/quality/radius`), distinct from the older `shadow_*` box-shadow properties and not behind an `lv_conf` flag. Applying them to the diagnostics cards and voice page tiles built cleanly, cost 256 bytes, passed host tests — and hung the display on both pages the moment they were opened: half-rendered, then frozen.

   **Why, exactly.** A drop shadow is not a style flag LVGL rasterises in place. `lv_draw_rect.c:78` calls `lv_draw_layer_create_drop_shadow()`, which allocates a **whole extra A8 layer** sized to the object plus twice the blur radius on each side (`lv_draw.c:545-551`), renders the object into it, blurs it, and composites it back. Our LVGL pool is `CONFIG_LV_MEM_SIZE_KILOBYTES=128` with a 32 KiB expand. The layers our surfaces need:

   | Surface | Size | A8 layer needed |
   |---|---|---|
   | Diagnostics link message panel | 622x464 | 654x496 = **324 KB** |
   | Voice parameter panel | 1256x300 | 1288x332 = **428 KB** |
   | Diagnostics stat card | 300x110 | 332x142 = 47 KB |

   The big two cannot fit and `lv_draw_layer_create()` returns NULL. LVGL does not degrade gracefully: `lv_draw_rect.c:79` follows the call with `LV_ASSERT_NULL(ds_layer)`, we build with `CONFIG_LV_USE_ASSERT_NULL=y`, and the default `LV_ASSERT_HANDLER` is `while(1);`. So the UI task spins forever mid-frame — which is precisely "half renders, then freezes". With `CONFIG_LV_USE_LOG` off there is not even a message to find. (Had asserts been off instead, the NULL would have been passed straight into `lv_draw_fill()` and dereferenced, so neither setting is safe.)

   **Raising `LV_MEM_SIZE` to ~450 KB does not work — it will not link.** Checked 2026-08-30 in the map file. The pool is `work_mem_int`, a static array `lv_mem_core_builtin.c:82` places at `0x4ff40000` with size `0x20000` (128 KiB). That address is in the `sram_high` region, which is **`0x40000` = 256 KiB in total**. The pool is internal SRAM, not PSRAM, so 450 KB exceeds the entire region it lives in — and `idf.py size` shows only 173 KB of DIRAM remaining overall, so the realistic ceiling is roughly 300 KB even if every other internal allocation were left starving. The voice parameter panel alone wants a 428 KB layer, so no achievable static pool covers it.

   Moving the pool to PSRAM is technically possible — `LV_MEM_ADR`, or backing the pool with a `heap_caps_malloc(MALLOC_CAP_SPIRAM)` buffer — and external RAM is 128 MB at 0.64% used, so the *space* is there. It is very likely the wrong trade anyway: the operation is a full-layer blur, per shadowed object, per redraw, and running it against PSRAM rather than internal SRAM is the slowest place to put it. The panel is already reported as sluggish on the graphically heavy pages.

   **What would have to change to retry.** Not the shadow values — the geometry. Either shadow only objects small enough that their A8 layer fits the pool (roughly 350x350 at radius 8, and nothing on a page may exceed it), or raise `LV_MEM_SIZE` by hundreds of KB, which is not free on this board. A per-page audit of every shadowed object's dimensions is a hard prerequisite, because the failure mode is a silent hang rather than a missing shadow. **This is why the feature is worth its own note rather than a quiet revert**: "already compiled in, so the only cost is render time" was true about flash and wrong about RAM, and the RAM cost is the one that bites.

Deliberately **not** taken up: `LV_CHART_TYPE_CURVE` (Bezier charts) needs `LV_USE_VECTOR_GRAPHICS` plus a vector draw unit — ThorVG, NanoVG or VGLite — which is not a sane ask on this target; our three `lv_chart_create` sites (`ui_diagnostics_page.cpp`, `components/waveform_view.cpp`) do not need it. Noted as available if a use appears: `LV_OBJ_FLAG_RADIO_BUTTON` (we hand-roll selection chrome in `ui_tab_group.cpp`), configurable `lv_indev` gesture thresholds and key remapping, `LV_STATE_ALT` with the new theme create/copy/delete and `lv_obj_bind_style_prop` API (we call no `lv_theme_*` today), and the `lv_canvas` skip-invalidation setter.

### 0.4 Project-principles audit remediation (added 2026-08-31)

The firmware-wide review against [`project-principles.md`](project-principles.md)
found a sound real-time foundation but several incomplete ownership and
abstraction boundaries. The work below is deliberately sliced; do not turn it
into a foundation rewrite that violates Principle 8.

| Audit debt | Roadmap owner | Completion evidence |
|---|---|---|
| Note-on still hardcodes MIDI root note 60 instead of using the built instrument resolver | Phase 2 active work order, Stage 2 | `OnNoteOn` uses `Instrument::ResolveNoteOn`; zone, velocity, and tuning tests pass; hardware notes play at the expected pitch |
| Resident sample memory is freed after a requested stop plus a fixed 10 ms delay | 0.4 item 1 | Explicit callback acknowledgement proves no voice can reference the sample before it is freed; active-voice unload/reload stress has zero use-after-free and zero underruns |
| Main-loop meter reads can observe a callback update partway through the struct | 0.4 item 2 | All multi-field callback telemetry crosses through one complete block snapshot; an interleaving regression test covers publication semantics |
| Large coordination units and overlapping ESP32 globals obscure ownership | 0.4 items 3–4 | Each extracted subsystem has one owner/writer and a narrow facade; every slice builds and tests independently |
| The output-sink abstraction exists but the callback still writes the stereo mix directly | Phase 3 item 3 | The selected `OutputSink` is the sole hardware-output boundary and DSP contains no DAC/codec-specific branch |
| Callback DSP and DTCM placement gains are asserted without target measurements | § Outstanding hardware verification (`Per-voice SVF cost`, `SVF denormal behaviour`, `DTCM placements`) | Recorded DWT before/after data and the one-hour zero-underrun soak settle each claim |
| The ESP-IDF devcontainer follows a moving release branch | Phase 0.1 | Image is pinned to an explicit 5.5.x tag and the listed panel, SD, and silicon-revision checks pass |
| Canonical architecture dependency versions and output-sink commentary lag the code | 0.4 item 5 | Documentation states the locked versions and the as-built output boundary accurately |

Work packages, in implementation order:

1. **Replace timed sample retirement with a request/completion handshake.** Give
   stop-all/sample-retire requests a monotonic generation. The callback stops
   matching voices at a block boundary and publishes the completed generation;
   the main loop frees or replaces sample memory only after observing it. Keep
   load/unload non-blocking and state-machine-driven. This closes Principles 1,
   2, 5, 6, and 14.
2. **Publish callback telemetry as immutable snapshots.** Move `BlockMeters`
   and any other multi-field callback state through the existing
   `SnapshotMailbox` pattern, with one publication per complete block and one
   coherent consumer copy. This closes Principles 5, 6, and 10.
3. **Consolidate ESP32 application ownership.** Make `ApplicationContext` (or a
   clearly named child) the sole owner of packet routing, statistics, and
   injected listeners. Remove unused duplicate globals rather than preserving
   two plausible owners. Change one collaboration seam at a time and retain a
   stable facade for callers. This closes Principles 3, 5, 7, 14, and 15.
4. **Extract focused modules when their code is next changed.** Start with the
   cleanest seams in `audio_engine.cpp` (resident-sample lifecycle, streaming,
   telemetry publication, output/CV routing), then separate diagnostics data
   from rendering and browser state from presentation on the ESP32. File length
   is evidence, not the acceptance criterion: the goal is one responsibility,
   one owner, and an independently understandable interface. Each extraction
   is its own buildable/testable change. This closes Principles 3, 7, 8, 14,
   and 15.
5. **Correct documentation drift.** Reconcile the ESP-IDF/LVGL versions in
   `architecture.md` with the lockfiles and rewrite the stale `output_sink.hpp`
   as-built commentary when the Phase 3 boundary lands. Documentation-only
   corrections may land sooner; intentional design changes update code and
   documentation together. This closes Principle 13.

**Gate**: `make all` + `make test` clean from scratch; SD soak test passes.

---

## Phase 1 — Solid Playback Core

Order of implementation:

1. **Streamed-voice concurrency**: 2 concurrent streams with prebuffer admission control. The existing WAV-streaming path (`s_wav`, ring buffer, prebuffer, SD buffer slots in `audio_engine.cpp`) is entirely singleton/global; generalizing it to N admission-controlled streams is a refactor of that subsystem, not an extension of `VoiceManager` (which handles only RAM-resident playback).
2. **Recording, rebuilt**: the old `Sampler` was deleted — nothing fed its input, nothing rendered its playback, and its wire commands were dispatcher stubs. Rebuild it voice/streaming-integrated when scheduled. The preallocation lesson stands: no `reserve()` in the audio path; take a fixed extent from the SDRAM allocator at setup.
3. **Raise SPI link clock**: replace the bring-up `PS_16` prescaler; verify on scope, measure error rate at each step. Target: browse a 500-entry directory in < 500 ms; waveform preview of a 3-min WAV in < 1 s. **Blocked** on the SPI link being compiled out (0.2 item 2).

**Gate**: 8-voice drum kit playable from MIDI with zero underruns for 1 hour; paraphonic analog path (Stage A) calibrated and audible; both output/CV flag configurations compile in CI; host tests cover voice allocation and the sample-load → status → UI flow.

---

## Phase 1.5 — Sample Edit Page & UI Interaction Model (added 2026-08-29)

Non-destructive marker editing. Sits here, not in Phase 4, because the engine already supports everything it needs — `Voice` carries `start_frame`, `end_frame`, `loop`, `loop_start` and `loop_end` (Phase 1) — so this is a UI and protocol gap, not new DSP. Destructive operations (trim, normalize, fades, render jobs) stay in Phase 4.

Current state: the page draws the wireframe layout and START/END/ZOOM move the *preview window* only. Nothing is sent to the backend, so nothing is audible or persistent.

### 1.5.1 Marker model and protocol

1. ~~**Four independent markers**: start, end, loop start, loop end.~~ Done — `MSG_SAMPLE_EDIT_SET` (0x3C) carries all four plus gain and a loop flag. The backend clamps (`start <= loop_start < loop_end <= end`) and refuses loops under 256 frames, which would re-seek every refill pass and starve the ring.
2. ~~**Audition must honour the markers.**~~ Done for both paths. Streaming caps at the region (or loop) end and rewinds to the loop point; `OnNoteOn` now fills `VoiceTriggerParams` from the record, so a note-triggered voice plays the same region the editor auditioned. `VoiceTriggerParams` already had every field — nothing was filling them.
3. ~~**Gain**~~ Done for both paths: saturating q15 on the streaming block, and `gain_mul` (dB converted to linear) at RAM-voice trigger.
4. ~~**No status reply yet.**~~ Done — `MSG_SAMPLE_META` (0x3D) is the authoritative record and is pushed on every change, so the UI shows what the engine applied rather than what it asked for. `MSG_SAMPLE_EDIT_SET` is now explicitly the *command* and the record is the *state*.
5. **Save / Save As**: persist markers and gain. Prefer a WXCF sidecar (`features/instrument-model.md` §5) over a new per-file format — this is the same data a zone carries, and duplicating it invites divergence.

   **Naming needs a decision.** Auto-numbering (`amen.wav` → `amen1.wav` → `amen2.wav`) is the cheap option and needs no text-entry UI, but three things have to be settled first, and each has bitten samplers before:
   - **Collision policy.** Scan for the first free suffix, or track a counter? A counter goes stale the moment a file is deleted or the card is swapped; scanning costs a directory listing per save but is always right. Scanning, given how often cards get swapped here.
   - **Where the number goes.** Before the extension (`amen1.wav`), never after (`amen.wav1`), and the base must be truncated so the result fits `FILE_NAME_MAX` (48) — silently truncating the *number* off the end would overwrite the original.
   - **Whether Save As copies audio or writes a sidecar.** These edits are non-destructive, so a sidecar is far cheaper and instant. But "Save As" implies a new file the user can see in the browser and load independently, which a sidecar is not. Probably: sidecar for markers, and a genuine render-to-new-file when Phase 4's render jobs exist.

   A text-entry surface (on-screen keyboard) is worth having eventually regardless, and is reusable for preset and pattern names — but it should not block Save As.
6. ~~**Loop gap in the browser, gapless in the editor.**~~ Done. `loop_gap_ms` rides on `SamplePlayIndexMessage`, **not** on `SampleMetadata`: it belongs to the audition, not to the sample, so the caller decides — the browser passes 300, the editor 0. The Daisy pushes silence into the ring at the rewind point rather than skipping the pass, so the gap is genuine silence rather than an underrun, and it costs no SD read.

7. **Sample selection from the edit page.** Currently the page edits whatever the browser last loaded, with no way to change it. Either a picker, or make the edit page accept a sample argument and have the browser push it. The Shift row has a `Select` key reserved for it.
8. **Partial load for oversized samples.** The browser now refuses a sample larger than the allocator's largest free block, showing both figures. Loading a truncated head instead would need a length field on `MSG_SAMPLE_LOAD` and a truncating reader on the Daisy. Worth doing — but the refusal-with-numbers is the honest interim, where the old behaviour was a load that failed with no explanation.

### 1.5.2 Interaction model

**Decided (2026-08-29): a Shift modifier reveals an alternate softkey row.** Built and global — `UIPage::getShiftedSoftkeys()`, `UINavigator::toggleShift()`, a SHIFT chip in the header, and `BUTTON_SHIFT` intercepted in `InputDispatcher` so no page can swallow it. Three properties worth keeping:

- **Latched, not held.** Hold-and-press is awkward one-handed on a touch panel, and holding a key while turning the encoder is worse.
- **Sticky**: clears after one shifted key fires, and on navigation. A plain toggle gets left on and the next press does the wrong thing.
- **Inert, not hidden, on pages with no alternate row.** A control that appears and disappears as you navigate is harder to learn than one that is always there and sometimes dim.

Physical Shift still needs a key: `tca8418_keypad.cpp` maps keycode 4 → `BUTTON_SHIFT`, but the matrix mapping is a three-key stub and `WAVEX_ESP_BUTTON_MATRIX_ENABLED` gates it. Until then the header chip is the only way in.

Specific items:

1. **Loop splice view** (see 1.5.6 item 1) is the one interaction on this page that is not a variation of "move a marker" — it needs two synchronised waveform panes and its own scroll behaviour. Worth designing before the drag work below, since dragging inside the splice view has different semantics: there, dragging moves the *audio* under a fixed centre line.
2. ~~**Draggable handles.**~~ **Done** — all four (S, E on the top edge; LS, LE on the bottom) are touch-draggable, mapping the pointer back through the zoom window with the inverse of the function that places them. As predicted, the constraint work was the content rather than the LVGL plumbing, and three parts of it are worth keeping:

   - **Bound the dragged handle, do not clamp afterwards.** Running `clampMarkers()` over the result makes the handle you are *not* touching move: push E past S and the clamp walks S across the screen. Each handle instead gets an explicit `[lower, upper]` from its neighbours, so it stops dead against them.
   - **The 256-frame loop minimum had to come to the frontend.** The backend does not reject a short loop — `audio_engine.cpp`'s `kMinLoopFrames` silently clears `loop_enabled` — so before this you could drag a tight loop, see LOOP ON, and watch it turn itself off a round trip later. Enforced only when the region is long enough to hold one, or a short region would become undraggable.
   - **Edits are coalesced.** LVGL fires `LV_EVENT_PRESSING` at the refresh rate, so sending per event would put ~30 `MSG_SAMPLE_EDIT_SET` per second on the link. A fixed ~12 Hz cadence during the drag plus one on release, and release sends only when something is actually owed so a tap does not duplicate.

   Dragging also moves the encoder focus to that parameter, so Zoom + anchors on the handle just moved rather than whatever was last selected.
3. ~~**`< Param` / `Param >`** replace `Param >` and `Refresh`.~~ Done.
4. ~~**Audition toggles to Stop**, matching the browser.~~ Done, and it stops on page exit — audition used to play on under a page that no longer existed.
5. ~~**Encoder direction is a global contract, not a per-page choice.**~~ **Done**, and the diagnosis in this item was incomplete: the root cause was not that pages read `delta` carelessly, it was that the two *producers* disagreed about what `delta` meant. `ui_task.cpp` posted the rotary encoder's raw signed count and the pot's magnitude, so the same negation was correct for one control and inverting for the other. Fixed at the producer — `delta` is now a magnitude from both — with `InputEvent::steps()` as the single reading path (direction from the event type, size from `delta`) and host tests pinning it. **The edit page's local patch was not the end of it:** the voice page still had the live inversion, so counter-clockwise *increased* the value while editing a parameter. Both now use the helper.
6. **Two different physical controls are conflated.** `EncoderLeft`/`Right` come from the rotary encoder; `EncoderUp`/`Down` come from a pot (`ui_task.cpp`). Pages currently treat them as one input. Decide whether that is intended before building marker editing on top of it.

### 1.5.3 Sample browser

**Mostly done.** 54 px rows with a green selection ring, dim directories, right-aligned durations, and the design 1b geometry (list 770×487, detail panel 474×521 with filename headline, format/length rows, audition state and progress bar). The three row-building sites in `wavex_file_browser` had drifted apart — different fonts, different selection colours, no duration at all — and now share one `fb_style_row`.

Still open:

- **The `data_start` row is missing, deliberately.** `data_start` is carried by neither `FileEntryWire` nor `SampleMetadata`, and inventing a number for the one field that correlated exactly with the stutter would be worse than omitting it. Plumbing it needs a wire change: adding 4 bytes to `FileEntryWire` reduces entries per browse packet, so measure that cost first. See `docs/backlog.md`, which warns specifically against "fixing" the correlation by rounding `data_start` up — the mechanism is still unproven.
- ~~**Waveform in the detail panel.**~~ **Done**, and the scrolling worry turned out to be settled by a constraint rather than by the cache. `MSG_ENVELOPE_REQ` is served from *sample RAM* on the Daisy, so only the **loaded** sample has an envelope at all — a merely-selected file has nothing to fetch. The panel therefore shows a waveform when the highlighted row is the loaded sample and a "Load to preview" hint otherwise, and asks for nothing at all while the cursor moves. One coarse whole-file tier serves it (no zoom here), which the cache then holds cheaply across visits.

  The fetch machinery is now shared: `components/envelope_fetcher.{h,cpp}` owns the request/assemble/commit/timeout state machine that had lived only inside the edit page, and both pages drive it. It is host-tested (13 tests), which that logic never was — including the defect it was shaped by, where a run abandoned without releasing `EnvelopeCache::noteRequest()` stopped *every* waveform in the process from loading until reboot.
- ~~**Per-row loading spinner**~~ Done, and polled from the UI task rather than created at each of the nine `pagination_in_progress` sites — several of those run on the UART task, and creating LVGL objects there is the mistake that froze the edit page.
- ~~**The audition progress bar exists but nothing drives it.**~~ Done. The Daisy emits position at a fifth of the meter rate (a bar does not need 20–50 Hz) reusing `MSG_SAMPLE_STATUS` state 1, with `sample_rate` carrying the region length so the UI can scale without a second message. Note it is the **read** position, ahead of what is audible by the ring (~42 ms) — fine for a bar, not a playhead.
- **Free space is not shown.** The design's "SD 12.4 GB free" would have to be invented: neither `FileEntryWire` nor `StorageStatusMessage` carries capacity. The strip shows mount state instead. Adding free/total bytes to `StorageStatusMessage` is cheap if wanted.

### 1.5.4 Busy feedback

**Built.** The question of whether the UI was genuinely blocked resolved in our favour: the ESP32 only sends `MSG_SAMPLE_LOAD` and waits, while the Daisy does the SD work, so LVGL keeps redrawing and a spinner genuinely spins. `ui_busy_overlay` (scrim + spinner + caption + optional bar) is shared, always timeout-bounded, and only dismissable by touch *after* it has failed — cancelling a live operation would leave the backend loading into a UI that has moved on.

Still open:

- ~~**Real progress.**~~ Done. The Daisy emits `MSG_SAMPLE_STATUS` state `0x11` during the load read loop, rate-limited to whole percent, with `frames_played` carrying the percentage. State `0x11` is distinct from `0x10` (complete) so an older frontend ignores it. The sender pumps the TX queue itself — it is only 4 deep and nothing else drains it from that context, so progress would otherwise be silently dropped.
- **Use it elsewhere.** Preview fetch and card remount should show the same overlay rather than each inventing something.

### 1.5.5 Sample metadata and waveform caching (added 2026-08-29)

Today every waveform redraw is a round trip: the ESP32 sends `MSG_PREVIEW_REQ`, the Daisy re-reads from SD, decimates, and streams `MSG_WAVE_CHUNK` back. Zooming or moving a marker off-window refetches from scratch. That is the reason the page needs a 150 ms request debounce at all.

1. ~~**A per-sample metadata record, synced to the ESP32.**~~ **Done.** `SampleMetadata` (84 B, `MSG_SAMPLE_META` 0x3D) is owned by the Daisy and pushed on every change; `MSG_SAMPLE_META_REQ` (0x3E) asks for a resend, with id 0 meaning "all". Every playback and display path now reads it, so they cannot disagree: streaming audition, RAM voices and the preview generator all consult the same record. `Resolve()` — the sentinel expansion and clamping — lives on the struct and is therefore shared rather than reimplemented per consumer.

   Still to fold in: it should also become the zone's source of truth (`features/instrument-model.md`), and `generation` is defined but nothing bumps it yet (nothing renders destructively).

2. ~~**Send an envelope, not decimated samples.**~~ **Done.** `MSG_ENVELOPE_REQ` (0x3F) / `MSG_ENVELOPE_CHUNK` (0x44) carry a min/max pair **per channel** per display column. Per channel rather than summed was decided here rather than deferred, because 1.5.7 item 2 is right that a second format later is the expensive outcome: an out-of-phase stereo sample sums to near silence and would draw as a flat line for audio that is fine.

   Two things about the backend side are worth knowing before extending it. A true envelope has to read *every* sample in the window — that is exactly what decimation does not do, and why it does not alias — which is ~16 M reads for a three-minute stereo file. Doing that in the message handler would stall the main loop past the ring's ~42 ms of headroom, so it is a budgeted job (`PumpEnvelopeJob`, ~24 k frames per main-loop pass) that sends each chunk as it is measured. And each chunk repeats the whole window plus the generation, so a chunk is self-describing and cannot be filed under the wrong content.

3. ~~**Mip-mapped tiers cached in PSRAM.**~~ **Done** — `components/envelope_cache.{h,cpp}`, host-tested. Two departures from the sketch above, both deliberate:

   - **Tiers are powers of two derived from the view, not a fixed ÷1024/÷256/… ladder.** The tier is the largest power of two that still fits inside one display column, so a tier column never spans more audio than a pixel does and the merge into display columns is exact — the min of a set of minima is the true minimum. A fixed ladder cannot promise that at an arbitrary zoom.
   - **Every tier is windowed, not just the fine ones.** An entry is one contiguous run per (sample, generation, tier), capped at 4096 columns, and `nextRequest()` returns only the part of the view the cache does not already hold — so scrolling sideways asks for the newly exposed edge and nothing else. Coarse tiers happen to fit whole files, but nothing special-cases them.

   The budget is measured, as asked: an eighth of *free* PSRAM at page open, clamped to 128 KB–2 MB, with LRU eviction against it.

4. ~~**Invalidate on edit.**~~ **Done.** `generation` is part of the cache key, not a field checked afterwards, so an entry from generation N can never be read for N+1. The Daisy bumps it when a load rewrites an id that was already in use — that is a content change even though nothing renders destructively yet. Marker and gain edits deliberately do not bump it.

Still open here:

- ~~**The waveform still draws one trace.**~~ **Done** — `WaveformView` now draws two stacked traces for stereo, L above R, each labelled and each with its own grid zero line, with mono using the full height (1.5.7 item 1). The gap was **entirely frontend**: the wire, the cache and the Daisy's `ResolveDisplayChannels` were already returning two channels, and `setEnvelope()` was collapsing them to the widest excursion of either on arrival.
- ~~**The browser detail panel does not use the cache yet**~~ **Done** (1.5.3) — it shares the cache *and* the new `EnvelopeFetcher`, so the two waveform surfaces cannot drift apart in how they ask for or assemble a run.
- **The legacy decimated preview (`MSG_PREVIEW_REQ` / `MSG_WAVE_CHUNK`) is still live**, used only by the record page. Retire it when that page is rebuilt with recording (Phase 1 item 2) rather than leaving two waveform paths indefinitely.

---

### 1.5.6 Loop editing and de-clicking (added 2026-08-29)

1. ~~**Loop splice view.**~~ **Done** — the audio before `loop_end` on the left, the audio from `loop_start` on the right, butted at a centre seam, so the wrap can be aligned by eye. Built as two ordinary `WaveformView`s rather than a mode inside one, which means stacked L/R comes along unchanged and 1.5.7 item 4 (judging a seam per channel) is satisfied for free. Two details that were not obvious:

   - **Both halves must span the same number of frames.** Near the start or end of a file one side has less audio available; letting each show what it has puts the two at different scales, and comparing them by eye is the only thing this view is for. The span shrinks to what *both* can supply instead.
   - **It follows the focused parameter, not a softkey.** Both softkey rows are full, and "when editing a loop, show the seam" is the behaviour this item asks for anyway — focusing LS or LE is the gesture. The region handles hide with it, since their x axis means nothing against a seam.

   **The zero-crossing half of this item was NOT done, and the estimate above is wrong about it.** "Cheap… with a zero-crossing indicator on each side" does not follow: an envelope carries min/max per *column*, so it can say a column contains a crossing but not where. Locating one needs the samples, which live on the Daisy — snap is a protocol addition plus a backend search, not a drawing change. An indicator derived from the envelope alone would also be useless at anything but extreme zoom, where nearly every column spans zero. Tracked as its own item below.

2. **Snap-to-zero-crossing** (split out of item 1, 2026-09-02). Most loop clicks are a sign discontinuity, and snapping both loop points to a crossing removes them with no DSP. It cannot be done in the frontend: an envelope column carries min/max, which proves a crossing exists somewhere in the column but not at which frame, and the samples are on the Daisy. So this needs a request/reply — "nearest zero crossing to frame N, searching in direction D" — with the answer applied to the marker. Cheap on the backend (a linear scan of resident PCM) and the wire cost is two integers.

   1.5.7 item 3 is the open design question and should be settled with it: L and R rarely cross at the same frame, so a stereo file has no single right answer. Snapping to L and accepting it is the cheapest; snapping where both channels are within a threshold of zero is the most useful and the most likely to find no candidate, so it needs a documented fallback.

3. **Crossfade loop.** Where alignment cannot remove the seam, blend it. Two forms, and they are different features:
   - **Playback-time crossfade** (non-destructive): the engine overlaps *n* ms around the loop point on every pass. Costs a little CPU per loop, changes no file, and can be tuned live while listening — which is what makes it the right one to build first.
   - **Rendered crossfade** (destructive): `xfade_loop` in Phase 4 item 2. Permanent, free at playback, but needs the render-job scheduler.

4. ~~**Fade in / fade out / de-click.**~~ **Done** for the playback-time form, which is the one that fixes the audible problem. `fade_in_ms` / `fade_out_ms` ride on `SampleMetadata` and `MSG_SAMPLE_EDIT_SET`, and are applied on **both** playback paths — the streaming audition (pre-resample, since the fade position is a source frame index) and RAM voices — so the editor auditions what a pad plays. Two things settled while building it:

   - **De-click is the default, not an opt-in.** Both fields default to `kDefaultDeclickMs` (1 ms). A region that starts mid-waveform starts on a step whether or not anyone asked, so the honest default is the one that removes it; 0 turns it off and is a real, reachable value. 1 ms is short enough to be inaudible against a drum transient, whose rise time is 5–20 ms.
   - **The backend clamps the fades to the region**, because only it knows what the region ended up being after its own clamping — and a fade longer than the audio it shapes never reaches unity, which reads as "the sample got quieter" rather than as a fade.

   The edit page carries FADE IN and FADE OUT as two more paged params (1 ms per detent up to 20 ms, then 5 ms — a de-click lives in the first few milliseconds and an audible fade lives above 50, so one linear step would make one of the two useless).

   **The loop seam is deliberately not covered.** Fades are anchored to the *region* start and end, so with looping on and `loop_start == region_start` the fade-in re-fires each pass — a short dip, which is better than the click but is not seam smoothing. Smoothing the seam is item 3's crossfade, and it is the right tool for it.

   Rendered (destructive) fades stay in Phase 4 item 2, and should reuse `fade.hpp`'s shape so a rendered file sounds like what was auditioned.

5. ~~Fade shape matters more than it looks.~~ **Settled for fades; still open for crossfades.** `fade.hpp` uses a raised cosine, `g(t) = (1 - cos(pi·t))/2`, host-tested for exact 0/1 endpoints and flat slope at both ends. A linear ramp has a corner at each end, and a corner in amplitude is a discontinuity in the first derivative — audible as a faint thump on exactly the material a click was the problem on.

   Note this is **not** the equal-power pair recommended above for crossfades, and the distinction matters when item 3 is built: equal power is right when two signals sum and the sum must hold level, but a fade to or from *silence* has nothing to hold level against, and an equal-power fade-in would start at −3 dB rather than at zero — which is a step, i.e. the thing being fixed.

### 1.5.7 Mono vs stereo in the editor (added 2026-08-29)

~~**Finding: the waveform you are editing is the LEFT CHANNEL ONLY, silently.**~~ **Superseded — do not act on this as written.** It was true of `OnPreviewReq` when this section was added. That loop no longer hard-codes a channel: it calls `ResolveDisplayChannels()`, which derives the plan from `SampleMetadata::channel_mode` and returns two traces for an `AS_RECORDED` stereo file when the caller opts into stereo. The envelope path (`MSG_ENVELOPE_REQ`) opts in; the legacy preview path does not, by design — see the residue note at the end of this section.

Decisions this raises, roughly in order:

1. ~~**What the waveform shows.**~~ **Settled and built: two stacked traces**, L above R, each labelled on the panel, with mono using the full height. The alternatives and why they lost: **summed mono** is cheapest and matches a mono monitor, but an out-of-phase stereo sample sums to near silence and would draw a flat line for audio that is fine; **overlaid L/R in two colours** keeps full height for both and shows phase as divergence, but is unreadable on dense material. Stacked halves the vertical resolution of each trace, which is the price paid for being the only view in which a loop can be aligned on both channels (item 4).

   Two things learned building it, both worth keeping:

   - **The whole gap was in the frontend.** The wire has carried both channels since 1.5.5 item 2, the cache stores and merges them per channel, and the Daisy resolves the plan correctly. `WaveformView::setEnvelope()` was collapsing them to the widest excursion of either *on arrival* — so the data was right and only the last step threw it away. Reading the roadmap's framing (that the fix "needs the wire format" changed) would have led to re-doing work that was already done.
   - **Grid rows have to be per lane, not per panel.** A single four-row grid across a split panel puts a line at neither trace's zero. Each lane now draws its own rows, so each trace keeps a centre line to read level against.

2. ~~**The envelope format must carry both channels** (1.5.5).~~ **Done, and it paid off exactly as argued.** A min/max pair *per channel* per column doubles the payload to ~10 KB for a whole file at full width, which is still small. Because this was decided before the format shipped, item 1 above turned out to be a display-layer change with no protocol work at all — the outcome this item existed to buy.

3. **Zero-crossing snap is per-channel and they disagree.** A zero crossing in L is generally not one in R, so snapping (1.5.6) has no single right answer for stereo. Options: snap to L and accept it, snap to the nearest crossing of the summed signal, or snap where both channels are within a threshold of zero. The last is the most useful and the least likely to find a candidate; needs a fallback.

4. **Loop seams must be judged on both channels.** The splice view (1.5.6) has to show both, or a loop tuned to look clean on L can click audibly on R. This is the concrete reason two stacked traces beats summed mono for this page.

5. **Streamed and RAM playback disagree about stereo today.** The streaming audition path preserves stereo through `ConvertFramesToOutput`, while `VoiceManager` averages stereo to mono per voice (a Phase 1 stopgap). The same file therefore sounds different depending on how it is triggered, and markers auditioned in the editor will not match what a pad plays. Reconcile before Phase 2.5 — it is the same unification item already noted in 1.5.1.

6. **The preview reads from sample RAM, not SD.** `OnPreviewReq` indexes a loaded sample, so a file too large to load has no waveform at all — which ties this to the partial-load item (1.5.1 item 8). A stereo file is twice the RAM of the mono equivalent, so this bites sooner than expected.

7. **Mono↔stereo conversion** is already a Phase 4 editing primitive. Non-destructive channel *selection* for playback (play L, R, or sum) is a different, cheaper thing and belongs here. **The backend half is already built and unused:** `SampleMetadata::channel_mode` (`SAMPLE_CH_AS_RECORDED` / `LEFT` / `RIGHT` / `MONO_SUM`) is defined, round-trip tested, and honoured by `ResolveDisplayChannels()` for both waveform paths. What is missing is any UI that sets it — nothing in `components/ui` references `channel_mode`, so it is `AS_RECORDED` on every sample in practice. Two things to settle when the control is added: where it lives (an edit-page param, most likely), and that **a single-trace panel currently does not say which channel it is showing** — selecting LEFT makes the backend send one channel and the panel draws it full height, unlabelled, which is the 1.5.7 failure mode reintroduced by the back door. Label the trace from `channel_mode` in the same change.

**Residue: the legacy preview path is single-trace by design, not by oversight.** `OnPreviewReq` calls `ResolveDisplayChannels(src, allow_stereo=false)`, so an `AS_RECORDED` stereo file still draws as channel 0 alone on the sample **record** page, unlabelled. That is the one place the original left-only complaint still holds. It is deliberately not fixed here: the path is retired when the record page is rebuilt with recording (Phase 1 item 2), and widening a format that is scheduled for deletion is work that gets thrown away.

**Gate**: set all four markers on a multi-minute WAV, audition the looped region, save, reboot, reload, and hear the same region. Markers survive a power cycle; no UI freeze during audition, zoom or load.

---

## Phase 2 — Groovebox Core: Sequencer + Pads (see `features/sequencer.md`)

The host-testable core is built: `pattern.hpp`, `sequencer_scheduler.hpp`, `tempo_follower.hpp`, `sequencer_transport.hpp` (all HAL-free, ~60 host tests), and the 0x50–0x57 protocol messages with round-trip and dispatch tests.

> **Active work order: [`features/digital-voice-audition.md`](features/digital-voice-audition.md).** The consolidated, ordered path to a playable and sequenceable **all-digital** voice. It exists because this work was spread across five documents interleaved with work the goal does not need. Stages 1, 3 and 4 (Goal A: live digital voice parameters, the grid page, live parameter editing) are now done — `ui_play_page.cpp` sends `MSG_NOTE_ON`/`OFF` and `MSG_CONTROL_CHANGE`, and `OnControlChange` publishes both the analog `s_para_pending` and digital `s_voice_live_pending` state for the callback. **Stage 2 (root-note correctness) and the remaining Stage 2b UI diagnostics are the gaps in Goal A** — `OnNoteOn` still hardcodes `root_note = kDefaultRootNote`; use the existing `Instrument::ResolveNoteOn` path rather than creating a second mapping — and Goal B (the sequencer path, stages 5–8) has not started. This is the active remediation for Principles 2, 3, and 11.

Open:

1. **The audible half**: drive `SequencerTransport::Tick()` from the audio callback and turn `TriggerEvent`s into voice triggers. Needs the double-buffered edit-between-steps discipline (`sequencer.md` §4), a track→sample kit mapping (instrument model, Phase 2.5), and the intra-block sample-offset trigger path. Requires hardware audition to verify.
2. **MIDI clock out**: 0x57 serialization on the ESP32 to DIN + USB.
3. **UI**: pad grid page (TCA8418 matrix + touch), step editor page, kit editor. LED feedback via TLC5947 — bring up the SPI2 driver here, its first real consumer.
4. **Project persistence on SD** (kits/patterns/songs); atomic save (temp + rename). Use the WXCF chunk container (`features/instrument-model.md` §5; `firmware/shared/wxcf/wxcf.hpp` is built and host-tested), not a per-file format.
5. **P-lock application**: the pattern model already carries `param_locks[≤4]` and transport edits them; applying them to trigger params lands with the callback integration, per `features/param-locks-and-modulation.md` §2.

Kit representation is settled: a kit is a drum-mode instrument (`features/instrument-model.md` §8). Design Phase 2's kit structs so they *are* the drum-mode subset, not a parallel format to migrate later.

**Gate**: program and perform a 4-track drum pattern with swing from the front panel; MIDI-clock-synced to a DAW without audible drift over 10 minutes.

---

## Phase 2.5 — Sampler Instrument Layer (E-mu lineage)

Makes WaveX an *instrument* in the Emax/Emulator sense: multisampled presets across key/velocity ranges, a closed sampling loop, melodic sequencing, routed modulation. Index and rationale: `features/feature-expansion-ideas.md`.

Built already: the `VoiceManager` extensions, the instrument-model core (`audio/instrument.hpp` — zones, velocity layers, crossfade, choke, tuning fold; 18 host tests), and the WXCF container.

1. **Instrument model, remaining pieces** (`features/instrument-model.md`): the sample table that populates a `SampleResolver` from loaded WAVs; the `MSG_INST_OP/STATUS/ZONE_SYNC` protocol (0x60–0x62); deleting the Phase-1 stopgap note→sample policy; the ESP32 UI; the callback wiring shared with Phase 2 item 1; hardware audition.
2. **Mixer v1** (`features/output-routing-and-mixer.md` §1–2): 16-track gain/pan/mute/solo + per-track meters (0x78/0x79). Small, and the performance work below wants it.
3. **Melodic sequencing** (`features/melodic-sequencing.md`): melodic track type, chords/ties, step-record, live record/overdub/erase on the Daisy.
4. **Modulation matrix + LFOs + filter envelope** (`features/param-locks-and-modulation.md` §3–5): 8 slots/instrument, block-rate evaluation, `MSG_MIDI_CC` (0x56). Land the param slew engine (`features/scenes-and-performance.md` §3) here — same control-tick surface.
5. **Sampling/recording v1** (`features/sampling-and-recording.md`): threshold-armed capture with pre-roll, resample/bounce source, audition-before-save, non-destructive auto-trim markers, assign-to-zone. Depends on Phase 1 item 2.
6. **Arpeggiator** (`features/arpeggiator.md`): per-slot, clock-synced, latch; feeds live record.

**Gate**: build a multisampled keyboard instrument (≥ 3 key zones × 2 velocity layers) from freshly recorded samples entirely on-device; play it from MIDI through the Stage A analog path; live-record a chord progression + arp line over a drum pattern with p-locked filter moves; 1-hour zero-underrun soak with all of the above active; `make test` green.

---

## Phase 3 — Analog Voice Board (see `features/analog-voice-board.md`)

The **Stage A → Stage B transition** (`features/analog-voice-board.md` §0). The paraphonic prototype from Phase 1 already validated CV calibration, envelope→CV timing and analog levels, so this is hardware bring-up plus a flag flip, not new engine architecture. Blocked on the Stage B CV DAC part decision (`architecture.md` §3.3).

1. Decide Stage B CV DAC (recommendation: SPI MCP48CMB28 chain) and voice count; freeze PCB spec.
2. PCM1690 bring-up: SAI2 TDM-8 master TX, 8 test tones to verify slot order; I2C register init (reset sequencing, 24-bit TDM format, unmute).
3. `TdmVoiceSink` / `AudioOutputMode::VoiceSAI2` path: make the selected `OutputSink` the sole hardware-output boundary, with per-voice → TDM slot interleave while SAI1 stays stereo input + master mix. The callback and voice DSP must not branch on DAC/codec type. This is the Principle 4 audit remediation; keep the Stage A sink buildable.
4. `Mcp48Backend` behind the CV group router; flip flags to `TDM8` / `MCP48` / 8 groups; DMA/IT flush from the main loop — never blocking I2C/SPI in the callback.
5. Re-run calibration per voice (procedure and UI page reused from Stage A); stored calibration table on SD.
6. Keep Stage A buildable in CI as the fallback/bring-up configuration.

**Gate**: 8 analog voices with per-voice cutoff/res/VCA under sequencer control; calibration survives power cycle; scope-verified CV update ≤ 1 ms after control tick.

---

## Phase 4 — Offline Sample Editing & Mangling (see `features/offline-sample-editing.md`)

Recording ships in Phase 2.5; non-destructive marker editing ships in Phase 1.5. This phase adds the destructive half. Render-job messages use the reserved 0xA0–0xA3 block.

1. Render-job scheduler on the Daisy main loop (chunked SD→SD processing with progress messages; cancellation).
2. Editing primitives: trim/crop, gain/normalize, fades, reverse, mono↔stereo, resample, **crossfade-loop render** (`xfade_loop` — seam-smoothing for zone loops, the Emax tool; `features/instrument-model.md` §11). The *playback-time* crossfade and fades land first in Phase 1.5.6; these are the permanent, rendered forms, and should reuse the same fade shapes so a rendered file sounds like what was auditioned.
3. Waveform editor UI, destructive half: destructive ops via render jobs. The non-destructive marker UI is Phase 1.5, and the cached preview tiers moved there too (1.5.5) — they are needed long before destructive editing, and the cache invalidation hook (a content generation counter) is what render jobs will trip. This phase extends that UI rather than replacing it.
4. Slicing: transient detection (offline), slice-to-pads workflow.
5. Mangling effects (offline renders): bit-crush, drive/saturate, time-stretch, pitch-shift, granular freeze. CMSIS-DSP kernels where applicable — where the 1.17.0 upgrade pays off.

**Gate**: record → trim → normalize → slice → assign to pads → sequence, entirely on-device, with audio playback uninterrupted during renders.

---

## Phase 5 — Performance & Polish

- Song mode / pattern chaining; performance macros (encoder-assignable) + **scenes with morph** (`features/scenes-and-performance.md`).
- Digital send FX (delay, reverb) in the stereo master section.
- **Tuning & scales** (`features/tuning-and-scales.md`): master tune, 12-degree tables, scale-constrained input surfaces. Small and independent; slot it wherever a gap appears after Phase 2.5 item 1.
- Preset/kit browser richness (tagging, favorites), USB sample import (MSC or MTP — decide), settings persistence.
- ESP-IDF 6.0 migration (see 0.1).
- CPU/memory headroom pass with DWT profiling; lock the final block-size and clock decisions.
- **Polyphase sample-rate conversion** to replace `LinearResampleFrames`. Every non-48 kHz file is resampled by scalar `arm_linear_interp_q15` calls, one per output sample per channel — correct but aliasing-prone (linear interpolation is a poor anti-imaging filter) and ~1.7 ms per ~1050-frame chunk. The common case, 44.1 → 48 kHz, is the rational ratio 160/147, so a polyphase FIR with 160 precomputed phases on the CMSIS-DSP `arm_fir_*_q15` kernels replaces per-sample interpolation with a filter bank. Two wins (quality and CPU), and it removes the ≥2-input-frame edge cases behind the 2026-08 audition deadlocks. **Not urgent**: measured at roughly an 8% duty cycle during audition, this is not the bottleneck — the ~2.9 ms 8 KB `f_read` is the larger half. Verify against a measured profile before starting, and keep the linear path for ratios that are not usefully rational.

---

## Outstanding hardware verification

Code-complete but unproven. Each is real work, not history — a build that links is not a feature that works.

| What | Why it matters | How to check |
|---|---|---|
| LVGL 9.5 + `CONFIG_LVGL_PORT_ENABLE_PPA` with rotation | The original note claimed we don't use rotation. **That was wrong** — `display_manager.cpp` sets `LV_DISPLAY_ROTATION_90`, and PPA has known interaction bugs in rotation modes on P4. We are not exempt. | Flash the HX8394/MIPI-DSI panel, watch for tearing/corruption during rotation. Revert `CONFIG_LVGL_PORT_ENABLE_PPA` in both sdkconfig files if it misbehaves. |
| LVGL 9.5 performance claim | **Answered 2026-08-30.** The "~30% faster" rationale never followed from the flag that was enabled, and the draw unit that could have delivered it was measured and rejected — see [§ 0.3](#03-lvgl-95-take-up-added-2026-08-30) item 1. What remains open is narrower: the rotation row above, and the fact that nothing has profiled the pages that actually feel slow. | Superseded. Use `docs/performance_monitoring.md` Part 2 and `scripts/sysmon_stats.py`; the open target is the Play page's 65 ms render p95, not the 9.5 upgrade. |
| Partition table move | App offset moved 0x10000 → 0x20000. NVS content survives only because `nvs` kept its offset/size. | Flash and boot; confirm settings persist. |
| 48 kHz engine + resample path | 44.1 kHz content now always goes through the resampler — it is the normal path, not the exception. | Audition a 44.1 kHz and a 48 kHz WAV; confirm correct pitch on both. |
| UART full-duplex DMA + IRQ priorities | Async TX and the priority inversion fix are compile-verified only. | Sustained traffic during SD streaming; DWT jitter measurement. |
| SD soak on libDaisy v8.1.0 | The SDMMC/fatfs glue changed. | Mount, 1000× sequential reads, hot-unmount. |
| Stage A paraphonic analog path | Item is code-complete; all bench work outstanding. | CV-update-within-tick scope check, SSI2164 inversion, "silent is truly silent", exponential cutoff feel k≈3, analog levels. The CV Calibration page is the tool for this. |
| MIDI in-to-sound latency | Budget is ~2–3 ms on paper. | Measure DIN and USB in-to-sound on the bench; target < 5 ms. |
| Diagnostics telemetry round trip | `MSG_DIAG_PUSH` is implemented on both ends but never observed on hardware. | Open the diagnostics page; a non-zero `DIAG_PUSH` row in the Link message table proves it. |
| Per-voice SVF cost | The one-pole became a 2-pole state-variable filter in the callback's inner loop, ×8 voices. Host tests prove it is *correct*; nothing proves it is *affordable*. The guide requires a DWT number before a DSP change in the callback is accepted. | DWT cycle counter around `Render()` with 8 voices sounding, before/after. If it is tight, the fix is block processing through a CMSIS-DSP biquad, not reverting resonance — see `svf_filter.hpp`. |
| SVF denormal behaviour | The filter's integrator state decays toward zero on silence and can reach denormal magnitudes, which are slow on some FPUs. We deliberately did **not** pay for a per-sample guard, on the assumption that Cortex-M7 flush-to-zero is enabled. | Confirm `FPSCR.FZ` is set. If it is not, either enable it or add the guard — but measure, since the guard costs two compares per sample per voice. |
| Live voice-parameter path | `MSG_CONTROL_CHANGE` now reaches the digital voices (`s_voice_live_pending` → block-boundary mailbox → `ApplyLiveParams`), so a knob should be audible on notes already sounding. Host-tested only, and the block-rate cadence is exactly where zipper noise would appear. `ui_play_page.cpp` and `ui_voice_page.cpp` both send `MSG_CONTROL_CHANGE` now, so the path is reachable end to end — it just hasn't been checked on hardware yet. | Hold a note on the Play page, sweep cutoff and resonance — smooth, not stepped — then release and re-trigger; the new note must start where the sweep left it. |
| Digital voices are only reachable by note-on | Worth stating because it is easy to test the wrong path: the sample browser's Audition uses `MSG_SAMPLE_PLAY_REQ` → the **streaming** ring-buffer path, which bypasses `VoiceManager` entirely. The per-voice SVF, ADSR and live params run only on RAM-resident voices triggered by `MSG_NOTE_ON`, mixed on top of the streaming content in `Callback()`. | To exercise the voice path at all: load a 16-bit WAV from the browser (so it is RAM-resident), *then* trigger a note. Auditioning from the browser proves nothing about the filter. |
| SVF sound, by ear | Rolloff doubled 6 → 12 dB/oct, so an unchanged cutoff value now filters more steeply than any existing material was tuned against. The resonance range (Q 0.5–20) is an unvalidated choice. | Audition a sample across the cutoff range at resonance 0 and 1. The two questions: does the same cutoff still sound right, and does Q 20 self-oscillate or merely ring? |
| DTCM placements | `s_voice_manager`, `s_para_env`, and the per-block DSP/stat state were moved to DTCM for callback-time wins that were never measured — both commits say "compile/link-verified only". Now 1144 B of the 64 KB static budget. | DWT the callback with the placements reverted vs. applied. This is the number the ITCM item in `backlog.md` is also waiting on, so measure once and settle both. |
| Sample Edit waveform after the envelope-cache abort fix | The wedge that made `nextRequest()` refuse forever is fixed and host-tested, but **which first event abandoned the run has not been identified on hardware** — the fix makes every route recoverable rather than proving which one fired. Candidates, all silent on the wire: the Daisy's `OnEnvelopeReq` early-returns (`ENVELOPE: no sample for id=%u`, `total_frames == 0`, `end <= start` at `audio_engine.cpp:2312-2328`), `CancelEnvelopeJob()` on reload/channel change, and a UART send failure. One more is specific to entry: `onEnter` seeds `total_frames_` from `SampleBrowserState::lastLoadFrames()`, a duration-derived *approximation*, and the first request goes out before `applyMeta()` adopts the backend's exact value ~50 ms later — if the approximation overshoots the real length, the Daisy clamps `start` and returns silently. | Open Sample Edit on a loaded sample: the waveform must draw. Then confirm on the Diagnostics page that the `ENVELOPE_CHUNK` row is non-zero (`ui_diagnostics_page.cpp:1035` — it counts the real RX path, so 0 means nothing arrived). To identify the original trigger, watch the Daisy serial for `ENVELOPE: no sample`, and watch the status line for "Waveform request timed out - retrying": if a retry succeeds where the first attempt did not, the entry-time `total_frames_` approximation is the cause and the fix is to defer the first request until meta arrives. |
| Settings ▸ Display brightness | `bsp_display_brightness_set()` writes 2 bytes to an I2C backlight driver at address 0x45, and the register it writes depends on `CONFIG_BSP_LCD_TYPE_*`. Nothing has confirmed the configured panel type matches the fitted panel, so the write may land on the wrong register and do nothing — or something. `display_manager` also now drives it to 100% at start-up so the UI's model matches the hardware; that write is unverified too. | Open Settings ▸ Display and sweep Brightness end to end. The panel must dim visibly and monotonically, with no flicker or I2C error logged, and the UI must stay responsive during the sweep (the write happens on the UI task). |
| Settings tab group fits the panel | The tab bar costs 56 px on top of the 75 px header and 100 px softkey row, leaving 489 px of tab body (§7 of `ui-information-architecture.md` asks for this to be checked on the panel, not the simulator). Calibrate has eleven rows at 50 px pitch, so it must scroll rather than clip, and the encoder must drag the selection into view. | Enter each Settings tab. No row may be cut off at the bottom edge; on Calibrate, hold the encoder past row nine and confirm the list scrolls with the selection. |
| Settings ▸ MIDI receive channel | The filter is applied in `midi_forward_event()` and is host-untestable — it needs a real DIN or USB source transmitting on a known channel. | Send notes on channel 1 and channel 2. With the setting on Omni both sound; set it to 1 and only channel 1 sounds. Then hold a note, change the filter, release: the note must stop (Note Off is deliberately not filtered). |
| The August 2026 UI fix batch | Diagnostics tab tracking (only the softkey-reached tab refreshed), sample-edit page threading (`handleWaveChunk` ran on the UART RX task), WAV duration wrong past ~97 s (32-bit overflow in the ms conversion), the combined CPU tiles, and the inverted encoder direction on the edit page — **all five diagnosed by reading code, none seen working**. B1 in particular attributes Audio/Storage blankness to tab tracking without proving that is the only cause. | Open Diagnostics and switch tabs by touch, not softkey: every tab must fill. Load a multi-minute WAV and check the listed duration. Audition and zoom on the sample-edit page without a freeze. Turn the edit-page encoder clockwise and confirm the value rises. |
| Daisy SysClk raised 400 → 480 MHz | `main()` now calls `hw.Init(true)` (`System::Config::Boost()`), the STM32H750's rated maximum; it had silently been on libDaisy's 400 MHz default since the project started. Compile-verified only. The clock tree analysis says the risky couplings are absent — SDRAM/FMC is on PLL2 and the audio SAI on PLL3, so neither sample memory nor the 48 kHz rate moves, and both `Defaults()` and `Boost()` already enable D/I-cache so DMA coherency rules are unchanged — but **an analysis is not a soak**. A 20% core clock increase changes every timing margin in the callback and raises power draw and die temperature, and marginal SDRAM or SD behaviour would show up as intermittent corruption rather than a clean failure. | Boot and confirm the reported clock is 480 MHz. Then the full audio soak: 8 voices with SD streaming for one hour, zero underruns, and a hot-card mount/unmount pass. Re-run the DWT callback measurement — this is the *first* thing to measure before the `-O0` → `-O2` decision in `backlog.md`, because the extra headroom may change that answer. Watch the Daisy serial for `LONG I/O` lines and SDMMC CRC downgrades, which are the shapes marginal SDRAM/SD timing would take. Compare against a 400 MHz image (revert the `true`) if anything looks worse. |
| Loop splice view | The seam pair is geometry and timing: whether the two halves genuinely line up at the centre (they are rendered from two independent cache windows at a span chosen to match), whether the mode switch on focusing LS/LE reads as helpful or as the panel changing under you, and whether both windows fill promptly given one fetcher serves them over two commits. None of that is assertable from source. | With a loop set and Loop On, focus LS or LE: the panel must switch to the seam view, both halves must fill within a second or so, and the label plus orange seam must make clear which side is which. Move LS and LE and confirm each half tracks its own marker. Zoom in and out and confirm the two halves stay at the *same* scale — the whole view is worthless if they drift apart. Set a loop right at the start or end of the file, where one side has less audio available, and confirm both halves still match. Then focus a non-loop param and confirm the continuous view and the four handles come back. |
| Draggable marker handles | Touch geometry cannot be judged from source. The pointer-to-frame mapping is the inverse of the placement function and agrees with it by construction, but whether a 30–34 px tab with a 12 px extended hit area is actually grabbable with a finger — and whether the handle lands where you expect at high zoom — is a bench question. The coalescing cadence (~12 Hz) is also a guess at what feels live without flooding the link. | On Sample Edit with a sample loaded, drag each of the four handles. Each must follow the finger, stop dead against its neighbour rather than pushing it, and update the param card as it moves. With Loop On, try to drag LS and LE together: they must refuse to close past ~256 frames, and LOOP must stay on afterwards rather than flipping off a moment later. Then drag continuously for several seconds and watch the Daisy serial — edits should arrive at roughly 12 Hz, not 30. Finally zoom in hard and confirm a handle still lands under the finger. |
| Browser detail waveform, and the shared `EnvelopeFetcher` | Two things at once. The **new panel** is unproven end to end: it draws only when the highlighted row is the loaded sample, and the selection-matching, the "Load to preview" hint and the steady-state guard (which stops it re-rendering 442 columns at the UI task's ~30 Hz) are all reasoned rather than observed. The **refactor underneath it** moved the edit page's envelope request/assemble/commit path into a shared component; it is host-tested and semantics-preserving by inspection, but the edit page's own waveform was already on this list as unproven, so its bench check now covers rewired code. Do that one first — a failure there is a regression, whereas the browser panel is new. | On the browser, select the loaded sample: a waveform draws and the hint disappears. Move to any other row: hint returns, no waveform, and **no envelope traffic** (watch the Daisy serial). Load a different sample and confirm the panel follows. Then re-run the existing Sample Edit row below — the waveform must still draw, zoom must still refill progressively, and leaving mid-run must not stop later waveforms from loading, which is the shared failure mode both pages now depend on one implementation for. |
| Stacked L/R waveform traces | **Correctness is now covered off-device** — `firmware/esp32/tests/widget/waveform_view_test.cpp` renders the widget with real LVGL and asserts a hard-panned channel lands in the lane its label names, verified by mutation to fail on a swap. What is left for the bench is genuinely visual and cannot be asserted: whether a half-height trace is still legible on the panel, whether the divider reads as a boundary rather than a grid line, and whether first-frame render cost regressed now that the widget stores and draws two lanes. | Open Sample Edit on a **stereo** WAV whose channels plainly differ, then a **mono** one: stereo must split and label L/R, mono must fill the height with no divider. Judge legibility at half height — that is the open question, not the mapping. Then check first-frame render cost on entry (`scripts/sysmon_stats.py`, cf. `docs/backlog.md` § *Page-entry render cost*), since this doubles the stored columns and can double the span draw tasks. |
| Encoder direction contract (`InputEvent::steps()`) | The rotary encoder's `delta` changed sign convention at the producer (`ui_task.cpp`), so *every* page's encoder handling is downstream of it, not just the two that were edited. Host tests pin the helper, but nothing has confirmed on hardware which way the physical knob turns — and the whole class of defect here is a direction that is wrong only on real hardware. The voice page's inversion in particular was live and unnoticed, which is evidence nobody has recently checked a value while turning the knob. | On the voice page, enter edit mode on a parameter and turn clockwise: the value must rise, and counter-clockwise must lower it. Repeat on the sample edit page. Then turn fast in both directions and confirm the value tracks the distance turned rather than moving one step per event. Also confirm the pot (EncoderUp/Down) still agrees with the rotary encoder about which way is up. |
| Keypad and encoder after the E-KEY/E-ENC fixes | Three fixes to the physical controls were made from source and the TI datasheet with no hardware in the loop, and they change how input is decoded. The keypad in particular was previously either dead or busy-spinning, so **nobody has seen it work** — there is no "it behaved before" baseline to compare against. | Press each mapped key: one press event on press and one release on release, in that order, and Shift held with another key must register as both. Then confirm the UI task is not starved (diagnostics CPU tiles) — that was the busy-spin symptom. Spin the encoder fast in both directions and check no detents are lost or replayed. If keys still never arrive, the remaining suspect is the CFG register the vendored driver never writes, which needs a raw I2C write the component does not expose. |
| LVGL port-lock hold time after the E-LVGL fixes | Input dispatch now takes the port lock per event, and comm callbacks defer to the UI task. Both are argued to be cheap — the mutex is recursive and uncontended, and `lv_refr_now()` in `adaptiveRefreshControl()` already holds the lock far longer than any handler — but **nothing has been measured**, which is exactly what guide §13 forbids relying on. The suspicion worth testing is that the real cost was never the lock: the UI task's `lv_refr_now()` duplicates the lvgl_port task's own refresh loop, so two schedulers contend for one frame budget (review E-METER1/E-MISC1). | FPS and UI-task CPU per `docs/performance_monitoring.md`, while spinning the encoder fast on a list page (the burst case the per-event lock exists for) and while a sample loads (the deferred-update path). Compare against removing the `lv_refr_now()` call to see which term dominates. |

---

## Cross-Cutting Rules (apply to every phase)

- Every significant design, implementation, and review applies the decision filter in `project-principles.md`. Review findings cite the relevant principle and a concrete failure mode; principle-driven cleanup remains incremental and phase-aligned.
- Every protocol change: update `protocol.h` + round-trip test + `features/inter-mcu-protocol.md` in the same commit.
- Every DMA buffer: alignment + placement per `architecture.md` §7 — reviewer checklist item.
- Every phase gate includes: 1-hour zero-underrun soak, both-MCU-reboot recovery test, `make test` green.
- Docs: new subsystems get a `docs/features/*.md` design doc **before** implementation. **Superseded docs are deleted, and git history is the archive.** The old rule moved them to `docs/archive/` — but that directory is gitignored, so "archiving" a doc removed it from the repo anyway while leaving cross-references pointing at a file no clone contains. Deleting is the same outcome, stated honestly. Before deleting, move anything still *open* into `roadmap.md`, `backlog.md` or the relevant `features/*.md`; the commit message says what was salvaged and where it went. A doc that only records finished work leaves without ceremony — `CHANGELOG.md` and the log already have it.
- **Nothing in the UI task may block**, and nothing outside it may touch LVGL. Backend callbacks arrive on the UART RX task: store and flag, then draw from an `lv_timer`. Both rules have been broken and both froze the display.
- **Completed work leaves this document.** Detail goes to `CHANGELOG.md`; anything still unproven goes to § Outstanding hardware verification. A roadmap that accumulates finished items stops being read.
