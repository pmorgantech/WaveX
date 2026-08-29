# WaveX Implementation Roadmap

**Status**: Canonical implementation-order document. Read `architecture.md` first.
**Last updated**: 2026-08-29 (pruned completed items; added Phase 1.5)

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
| ESP-IDF | **floating** `espressif/idf:release-v5.5`, currently `v5.5.4-1169-gbb2188bfb8` (2026-06-18) | **Pin to a tag.** The devcontainer follows a branch, so a rebuild can move the toolchain silently — that is worth more than any fix in the release. v5.5.5 (2026-07-17) adds `CONFIG_SPIRAM_ENC_EXEMPT` / `MALLOC_CAP_SPIRAM_NO_ENC`, hardens the JPEG decoder (GHSA-v6r2-f6p2-88cj), and disables ECDSA Secure Boot V2 on ESP32-P4. **None of those touch us** — no secure boot, no PSRAM encryption, no JPEG decoding. The per-component changelog (SDMMC / UART / MIPI-DSI / PPA, the ones that would matter) could not be retrieved, so treat the delta as unknown: pinning moves ~1 month of `release/v5.5` commits, a bigger jump than the tag name suggests. Re-run the SD soak and a panel check after. Stay on 5.5.x otherwise; Plan the 6.0 migration as a dedicated task. Three tripwires: (1) ESP32-P4 default chip revision becomes v3.0, so binaries won't boot on rev < 3.0 silicon without `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` — check the module's silicon rev first; (2) legacy drivers are removed (audit third-party components, `esp_tca8418` is fetched from git and may lag); (3) every managed component must declare 6.0 compatibility, and the waveshare BSP is the likely laggard. Budget a spike branch. |

### 0.2 Repo cleanup still open

1. **Split `daisy_spi_link.cpp`** along the boundaries in `archive/daisy_spi_link_splitup_plan.md` (transport / packet / message-processing / bridges). Opportunistic as other work touches those files, not a big-bang refactor.
2. **UART is the transport of record.** `WAVEX_SPI_LINK_ENABLED` is hard-coded `0`, so the SPI link is compiled out of every image and UART carries all traffic including browse pages and wave chunks. Consolidating onto SPI-only is real protocol work — extending `protocol.h` for the UART-only message types, rewiring every send call site, then hardware bring-up. Track separately if prioritized. See `architecture.md` §4.4.

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
2. **Draggable handles.** All four handles (S, E on the top edge; LS, LE on the bottom) are drawn and track the zoom window, but are not yet touch-draggable — they move only by encoder. LVGL supplies the drag events; the constraint work (ordering, clamping, minimum separation, and mapping pixels back to frames at the current zoom) is the real content.
3. ~~**`< Param` / `Param >`** replace `Param >` and `Refresh`.~~ Done.
4. ~~**Audition toggles to Stop**, matching the browser.~~ Done, and it stops on page exit — audition used to play on under a page that no longer existed.
5. **Encoder direction is a global contract, not a per-page choice.** Clockwise increases, always. The edit page shipped inverted because `InputEvent::delta` is already signed *and* the event type names the sign, so negating on the Left case flipped it back. Anything reading `delta` must take its magnitude and let the type supply direction. Worth a shared helper so the next page cannot repeat it.
6. **Two different physical controls are conflated.** `EncoderLeft`/`Right` come from the rotary encoder; `EncoderUp`/`Down` come from a pot (`ui_task.cpp`). Pages currently treat them as one input. Decide whether that is intended before building marker editing on top of it.

### 1.5.3 Sample browser

**Mostly done.** 54 px rows with a green selection ring, dim directories, right-aligned durations, and the design 1b geometry (list 770×487, detail panel 474×521 with filename headline, format/length rows, audition state and progress bar). The three row-building sites in `wavex_file_browser` had drifted apart — different fonts, different selection colours, no duration at all — and now share one `fb_style_row`.

Still open:

- **The `data_start` row is missing, deliberately.** `data_start` is carried by neither `FileEntryWire` nor `SampleMetadata`, and inventing a number for the one field that correlated exactly with the stutter would be worse than omitting it. Plumbing it needs a wire change: adding 4 bytes to `FileEntryWire` reduces entries per browse packet, so measure that cost first. See `docs/backlog.md`, which warns specifically against "fixing" the correlation by rounding `data_start` up — the mechanism is still unproven.
- **Waveform in the detail panel.** Needs the envelope cache (1.5.5) to be worth doing; a per-selection round trip would make scrolling the list unusable.
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

- **The waveform still draws one trace.** The wire and the cache carry both channels, but `WaveformView` currently collapses them to the widest excursion of either. That is honest — it cannot hide anything the way summing can — but it is not the two stacked traces 1.5.7 item 1 argues for, and it does not let a loop seam be judged per channel. The layout decision belongs to 1.5.7; the format no longer blocks it.
- **The browser detail panel does not use the cache yet** (1.5.3), which is now the cheap version of that item.
- **The legacy decimated preview (`MSG_PREVIEW_REQ` / `MSG_WAVE_CHUNK`) is still live**, used only by the record page. Retire it when that page is rebuilt with recording (Phase 1 item 2) rather than leaving two waveform paths indefinitely.

---

### 1.5.6 Loop editing and de-clicking (added 2026-08-29)

1. **Loop splice view.** When editing a loop, show the audio *before* the loop end on the left and the audio *after* the loop start on the right, butted together at the centre line — what the loop will actually sound like at the seam. Scrolling either point slides its half, so the two waveforms can be aligned by eye. This is the loop-tuning display from the Emax/Akai lineage and it is the only practical way to place a loop by sight; a single continuous waveform view cannot show a discontinuity that exists between two distant points.

   Cheap to build on top of 1.5.5: it is two envelope windows drawn side by side, both from the cache, with a zero-crossing indicator on each side. Worth adding **snap-to-zero-crossing** at the same time — most loop clicks are just a sign discontinuity, and snapping removes them without any DSP.

2. **Crossfade loop.** Where alignment cannot remove the seam, blend it. Two forms, and they are different features:
   - **Playback-time crossfade** (non-destructive): the engine overlaps *n* ms around the loop point on every pass. Costs a little CPU per loop, changes no file, and can be tuned live while listening — which is what makes it the right one to build first.
   - **Rendered crossfade** (destructive): `xfade_loop` in Phase 4 item 2. Permanent, free at playback, but needs the render-job scheduler.

3. ~~**Fade in / fade out / de-click.**~~ **Done** for the playback-time form, which is the one that fixes the audible problem. `fade_in_ms` / `fade_out_ms` ride on `SampleMetadata` and `MSG_SAMPLE_EDIT_SET`, and are applied on **both** playback paths — the streaming audition (pre-resample, since the fade position is a source frame index) and RAM voices — so the editor auditions what a pad plays. Two things settled while building it:

   - **De-click is the default, not an opt-in.** Both fields default to `kDefaultDeclickMs` (1 ms). A region that starts mid-waveform starts on a step whether or not anyone asked, so the honest default is the one that removes it; 0 turns it off and is a real, reachable value. 1 ms is short enough to be inaudible against a drum transient, whose rise time is 5–20 ms.
   - **The backend clamps the fades to the region**, because only it knows what the region ended up being after its own clamping — and a fade longer than the audio it shapes never reaches unity, which reads as "the sample got quieter" rather than as a fade.

   The edit page carries FADE IN and FADE OUT as two more paged params (1 ms per detent up to 20 ms, then 5 ms — a de-click lives in the first few milliseconds and an audible fade lives above 50, so one linear step would make one of the two useless).

   **The loop seam is deliberately not covered.** Fades are anchored to the *region* start and end, so with looping on and `loop_start == region_start` the fade-in re-fires each pass — a short dip, which is better than the click but is not seam smoothing. Smoothing the seam is item 2's crossfade, and it is the right tool for it.

   Rendered (destructive) fades stay in Phase 4 item 2, and should reuse `fade.hpp`'s shape so a rendered file sounds like what was auditioned.

4. ~~Fade shape matters more than it looks.~~ **Settled for fades; still open for crossfades.** `fade.hpp` uses a raised cosine, `g(t) = (1 - cos(pi·t))/2`, host-tested for exact 0/1 endpoints and flat slope at both ends. A linear ramp has a corner at each end, and a corner in amplitude is a discontinuity in the first derivative — audible as a faint thump on exactly the material a click was the problem on.

   Note this is **not** the equal-power pair recommended above for crossfades, and the distinction matters when item 2 is built: equal power is right when two signals sum and the sum must hold level, but a fade to or from *silence* has nothing to hold level against, and an equal-power fade-in would start at −3 dB rather than at zero — which is a step, i.e. the thing being fixed.

### 1.5.7 Mono vs stereo in the editor (added 2026-08-29)

**Finding, from reading `OnPreviewReq` while writing this up: the waveform you are editing is the LEFT CHANNEL ONLY, silently.** For a stereo file the preview loop takes `samples16[i * src.channels]` and discards the rest. Frame indexing is correct — markers land where they should — but a sample with its content weighted to the right channel draws a misleading trace, and a hard-panned one can draw a near-flat line for audio that is plainly audible. Nothing on screen says so.

That is the first thing to fix here, and it does not need the whole caching design: summing to mono, or drawing both channels, is a change to that one loop plus the wire format.

Decisions this raises, roughly in order:

1. **What the waveform shows.** Three options, and they are not equivalent:
   - **Summed mono** — one trace, cheapest, matches what you hear from a mono monitor. Hides phase problems entirely, and an out-of-phase stereo sample sums to near silence, which draws as a flat line for audio that is fine.
   - **Two stacked traces** — honest, and the only view in which you can align a loop on both channels. Halves the vertical resolution of each.
   - **Overlaid L/R in two colours** — full height for both, and phase differences are visible as divergence. Busy on dense material.

   Two stacked traces is the safe default for an editor, with mono files simply using the full height. Whatever is chosen, **say which on screen** — the current silent left-only behaviour is the failure mode to avoid repeating.

2. **The envelope format must carry both channels** (1.5.5). A min/max pair *per channel* per column doubles the payload to ~10 KB for a whole file at full width, which is still small. Deciding this after the format ships means a second format.

3. **Zero-crossing snap is per-channel and they disagree.** A zero crossing in L is generally not one in R, so snapping (1.5.6) has no single right answer for stereo. Options: snap to L and accept it, snap to the nearest crossing of the summed signal, or snap where both channels are within a threshold of zero. The last is the most useful and the least likely to find a candidate; needs a fallback.

4. **Loop seams must be judged on both channels.** The splice view (1.5.6) has to show both, or a loop tuned to look clean on L can click audibly on R. This is the concrete reason two stacked traces beats summed mono for this page.

5. **Streamed and RAM playback disagree about stereo today.** The streaming audition path preserves stereo through `ConvertFramesToOutput`, while `VoiceManager` averages stereo to mono per voice (a Phase 1 stopgap). The same file therefore sounds different depending on how it is triggered, and markers auditioned in the editor will not match what a pad plays. Reconcile before Phase 2.5 — it is the same unification item already noted in 1.5.1.

6. **The preview reads from sample RAM, not SD.** `OnPreviewReq` indexes a loaded sample, so a file too large to load has no waveform at all — which ties this to the partial-load item (1.5.1 item 8). A stereo file is twice the RAM of the mono equivalent, so this bites sooner than expected.

7. **Mono↔stereo conversion** is already a Phase 4 editing primitive. Non-destructive channel *selection* for playback (play L, R, or sum) is a different, cheaper thing and might belong here — it needs a field on the metadata record, not a render job.

**Gate**: set all four markers on a multi-minute WAV, audition the looped region, save, reboot, reload, and hear the same region. Markers survive a power cycle; no UI freeze during audition, zoom or load.

---

## Phase 2 — Groovebox Core: Sequencer + Pads (see `features/sequencer.md`)

The host-testable core is built: `pattern.hpp`, `sequencer_scheduler.hpp`, `tempo_follower.hpp`, `sequencer_transport.hpp` (all HAL-free, ~60 host tests), and the 0x50–0x57 protocol messages with round-trip and dispatch tests.

> **Active work order (2026-08-29): [`features/digital-voice-audition.md`](features/digital-voice-audition.md).** The consolidated, ordered path to a playable and sequenceable **all-digital** voice — items 1, 3 and 5 below, plus only the slice of Phase 2.5 needed to hear anything. It exists because this work was spread across five documents interleaved with work the goal does not need. Its load-bearing finding, verified against the source: **the only live-editable parameter path in the engine today is the Stage A analog one** — `OnControlChange` routes `PARAM_FILTER_CUTOFF`/`RESONANCE`/`ENVELOPE_*` to `s_para_params`/`s_para_env` and nowhere else, while the digital per-voice filter and envelope are written once at `Trigger()` time (`voice_manager.hpp`: `SetCutoff` appears only there). On an all-digital path a filter or envelope knob therefore does nothing today. That is missing engine plumbing, not a UI gap, and it is that document's first stage.

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
3. `TdmVoiceSink` / `AudioOutputMode::VoiceSAI2` path: per-voice → TDM slot interleave; SAI1 stays stereo input + master mix.
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
| LVGL 9.5 performance claim | The upgrade was justified partly on speed; nothing has measured it. | FPS + UI-task CPU before/after on the waveform-preview and meter pages, per `docs/performance_monitoring.md`. |
| Partition table move | App offset moved 0x10000 → 0x20000. NVS content survives only because `nvs` kept its offset/size. | Flash and boot; confirm settings persist. |
| 48 kHz engine + resample path | 44.1 kHz content now always goes through the resampler — it is the normal path, not the exception. | Audition a 44.1 kHz and a 48 kHz WAV; confirm correct pitch on both. |
| UART full-duplex DMA + IRQ priorities | Async TX and the priority inversion fix are compile-verified only. | Sustained traffic during SD streaming; DWT jitter measurement. |
| SD soak on libDaisy v8.1.0 | The SDMMC/fatfs glue changed. | Mount, 1000× sequential reads, hot-unmount. |
| Stage A paraphonic analog path | Item is code-complete; all bench work outstanding. | CV-update-within-tick scope check, SSI2164 inversion, "silent is truly silent", exponential cutoff feel k≈3, analog levels. The CV Calibration page is the tool for this. |
| MIDI in-to-sound latency | Budget is ~2–3 ms on paper. | Measure DIN and USB in-to-sound on the bench; target < 5 ms. |
| Diagnostics telemetry round trip | `MSG_DIAG_PUSH` is implemented on both ends but never observed on hardware. | Open the diagnostics page; a non-zero `DIAG_PUSH` row in the Link message table proves it. |
| Per-voice SVF cost | The one-pole became a 2-pole state-variable filter in the callback's inner loop, ×8 voices. Host tests prove it is *correct*; nothing proves it is *affordable*. The guide requires a DWT number before a DSP change in the callback is accepted. | DWT cycle counter around `Render()` with 8 voices sounding, before/after. If it is tight, the fix is block processing through a CMSIS-DSP biquad, not reverting resonance — see `svf_filter.hpp`. |
| SVF denormal behaviour | The filter's integrator state decays toward zero on silence and can reach denormal magnitudes, which are slow on some FPUs. We deliberately did **not** pay for a per-sample guard, on the assumption that Cortex-M7 flush-to-zero is enabled. | Confirm `FPSCR.FZ` is set. If it is not, either enable it or add the guard — but measure, since the guard costs two compares per sample per voice. |
| Live voice-parameter path | `MSG_CONTROL_CHANGE` now reaches the digital voices (`s_voice_live_params` → `ApplyLiveParams`), so a knob should be audible on notes already sounding. Host-tested only, and the block-rate cadence is exactly where zipper noise would appear. **Currently unreachable at both ends** — see below — so this cannot be checked yet at all. | **Blocked on `features/digital-voice-audition.md` stages 3–4.** No surface sends `MSG_CONTROL_CHANGE`: `inter_mcu_send_control_change()` has zero callers, and `midi_task.cpp` deliberately does not forward MIDI CC (it carries `PARAM_*` ids, not raw CC numbers). Note-on arrives only from MIDI. Once a grid page and a parameter row exist: hold a note, sweep cutoff and resonance — smooth, not stepped — then release and re-trigger; the new note must start where the sweep left it. |
| Digital voices are only reachable by note-on | Worth stating because it is easy to test the wrong path: the sample browser's Audition uses `MSG_SAMPLE_PLAY_REQ` → the **streaming** ring-buffer path, which bypasses `VoiceManager` entirely. The per-voice SVF, ADSR and live params run only on RAM-resident voices triggered by `MSG_NOTE_ON`, mixed on top of the streaming content in `Callback()`. | To exercise the voice path at all: load a 16-bit WAV from the browser (so it is RAM-resident), *then* trigger a note. Auditioning from the browser proves nothing about the filter. |
| SVF sound, by ear | Rolloff doubled 6 → 12 dB/oct, so an unchanged cutoff value now filters more steeply than any existing material was tuned against. The resonance range (Q 0.5–20) is an unvalidated choice. | Audition a sample across the cutoff range at resonance 0 and 1. The two questions: does the same cutoff still sound right, and does Q 20 self-oscillate or merely ring? |
| DTCM placements | `s_voice_manager`, `s_para_env`, and the per-block DSP/stat state were moved to DTCM for callback-time wins that were never measured — both commits say "compile/link-verified only". Now 1144 B of the 64 KB static budget. | DWT the callback with the placements reverted vs. applied. This is the number the ITCM item in `backlog.md` is also waiting on, so measure once and settle both. |
| `ui-update` branch UI fixes | Tab tracking, edit-page threading, WAV duration, CPU tiles — all diagnosed by reading code. | See `docs/ui-update-backlog.md`. |
| Keypad and encoder after the E-KEY/E-ENC fixes | Three fixes to the physical controls were made from source and the TI datasheet with no hardware in the loop, and they change how input is decoded. The keypad in particular was previously either dead or busy-spinning, so **nobody has seen it work** — there is no "it behaved before" baseline to compare against. | Press each mapped key: one press event on press and one release on release, in that order, and Shift held with another key must register as both. Then confirm the UI task is not starved (diagnostics CPU tiles) — that was the busy-spin symptom. Spin the encoder fast in both directions and check no detents are lost or replayed. If keys still never arrive, the remaining suspect is the CFG register the vendored driver never writes, which needs a raw I2C write the component does not expose. |
| Keypad matrix geometry | `tca8418_keypad.cpp` calls `hw_init(8, 10)` while `hardware_config.h` declares `WAVEX_TCA8418_ROWS/COLUMNS` as 8×8, and the five `WAVEX_TCA8418_*` macros have no users. One of the two is wrong. Deliberately **not** changed blind: picking the wrong one silently stops a working matrix column from being scanned, and hardware truth is not something to guess at. | Check the schematic for how many columns are wired, make `hardware_config.h` say that, and have the code use the macros. Part of E-CFG1. |
| LVGL port-lock hold time after the E-LVGL fixes | Input dispatch now takes the port lock per event, and comm callbacks defer to the UI task. Both are argued to be cheap — the mutex is recursive and uncontended, and `lv_refr_now()` in `adaptiveRefreshControl()` already holds the lock far longer than any handler — but **nothing has been measured**, which is exactly what guide §13 forbids relying on. The suspicion worth testing is that the real cost was never the lock: the UI task's `lv_refr_now()` duplicates the lvgl_port task's own refresh loop, so two schedulers contend for one frame budget (review E-METER1/E-MISC1). | FPS and UI-task CPU per `docs/performance_monitoring.md`, while spinning the encoder fast on a list page (the burst case the per-event lock exists for) and while a sample loads (the deferred-update path). Compare against removing the `lv_refr_now()` call to see which term dominates. |

---

## Cross-Cutting Rules (apply to every phase)

- Every protocol change: update `protocol.h` + round-trip test + `features/inter-mcu-protocol.md` in the same commit.
- Every DMA buffer: alignment + placement per `architecture.md` §7 — reviewer checklist item.
- Every phase gate includes: 1-hour zero-underrun soak, both-MCU-reboot recovery test, `make test` green.
- Docs: new subsystems get a `docs/features/*.md` design doc **before** implementation; superseded docs move to `docs/archive/` (never silently deleted).
- **Nothing in the UI task may block**, and nothing outside it may touch LVGL. Backend callbacks arrive on the UART RX task: store and flag, then draw from an `lv_timer`. Both rules have been broken and both froze the display.
- **Completed work leaves this document.** Detail goes to `CHANGELOG.md`; anything still unproven goes to § Outstanding hardware verification. A roadmap that accumulates finished items stops being read.
