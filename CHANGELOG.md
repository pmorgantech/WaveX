# Changelog

All notable changes to WaveX are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Frontend (ESP32-P4) and backend (Daisy Seed) firmware share a single version
number, defined in the root [`VERSION`](VERSION) file. See `AGENTS.md` for the
versioning and release process.

## [Unreleased]

### Fixed — Startup failure now restarts instead of corrupting itself; build files no longer misdescribe the image

Found by the 2026-08-29 ESP32-P4 review (items E-INIT1, E-BLD1, E-BLD2).

- **A failed init restarts the chip.** `WaveXApplication` is stack-local in
  `app_main`, so returning on failure ran its destructor and freed the
  `StatisticsManager` and `PacketRouter` — while the UART link task, started
  earlier in the same `initialize()`, still reached both through file-scope
  pointers. The next frame off the wire was a use-after-free. A reboot loop is
  a visible failure; a silently corrupted one is not.
- **Removed the inert `-Os -flto …` compile/link options.** They were added
  *after* `project()`, and ESP-IDF configures every component target inside it,
  so they applied to nothing: the image builds `-O2` with no LTO. Anyone tuning
  against those flags was reading fiction. Optimisation level belongs in
  sdkconfig.
- **Pruned `EXCLUDE_COMPONENTS` to the entries that take effect.** Twelve of
  the 32 were being built anyway (IDF pulls back anything still depended on),
  including two that contradicted the rest of the tree — `main/` `REQUIRES
  esp_mm`, and sdkconfig sets `CONFIG_ESP_GDBSTUB_ENABLED=y`.

A clean rebuild after both build changes came out within 48 bytes of the
previous image, which is the expected result if the options really were
applying to nothing and the pruned exclusions really were being built anyway.

### Fixed — Keypad and encoder: the physical controls now decode correctly

Found by the 2026-08-29 ESP32-P4 review (items E-KEY1/2, E-ENC1). All three were
diagnosed from source and the TI datasheet, so they want a bench pass.

- **The TCA8418 keypad no longer gates reads on the INT line.** Nothing
  configures the controller to drive it — the vendored driver's `hw_init()`
  never writes the CFG register — so gating on INT meant either no key was ever
  read, or, if INT did assert, a 100% busy-spin at priority 5 pinned to core 1,
  starving the UI task on that same core. It now polls the event count, which
  works whatever CFG holds, and always yields.
- **`KEY_EVENT_A` bit 7 is masked.** It carries the press/release flag, so
  reading the register raw made a press of key 1 arrive as `0x81` and get
  dropped by the keycode mapping, while its release arrived as `0x01` and was
  posted as a **press**. Every button fired on release, and simultaneous keys
  (the Shift modifier) were unrepresentable. The FIFO is now drained per pass
  and each event decoded on its own, replacing the single-`last_keycode`
  press/release synthesis.
- **Encoder deltas are exchanged atomically.** `pcnt_consume_delta` bracketed a
  plain read/write with `portSET_INTERRUPT_MASK_FROM_ISR()` against "an ISR
  race" — but there is no ISR, and masking interrupts only affects the calling
  core, so it did nothing about the unpinned producer task on the other one.
  Detents were silently dropped under load.
- **The PCNT counter is re-centred rather than cleared every poll.** Counts
  landing between `get_count()` and `clear_count()` were destroyed; that window
  is now hit about once per 8000 counts instead of on every poll during
  movement. Both driver return codes are checked, and a failed clear no longer
  zeroes the baseline — which had re-applied the whole count as fresh delta on
  every later poll.

### Fixed — Comm listener lifetime: a page can no longer be destroyed under a running callback

Found by the 2026-08-29 ESP32-P4 review (items E-LIFE1/2/3). Listener
`{callback, user_data}` pairs are registered by UI pages passing `this` and
invoked from the UART RX task, so both halves of this were live use-after-free
windows on the normal path.

- **One mechanism for all seven listener slots** (`main/comm/listener_slot.h`).
  They were previously four different disciplines in `StatisticsManager` alone —
  one mutex held across the call, one *deliberately released before* it
  ("to avoid deadlocks", which threw away the only thing it was buying), one
  spinlock covering just the write, and one with no locking at all — plus three
  unguarded global pairs in `inter_mcu.cpp`. `ListenerSlot` holds its mutex
  across the invocation, so a page's `set(nullptr, nullptr)` in `onExit` cannot
  return while its handler is still running, and the pair can no longer tear.
  The mutex is recursive so a callback that re-registers itself works rather
  than hanging.
- **The file browser now deregisters on destroy.** It registered two listeners
  with `browser` as user_data and cleared neither, so a browse response or an
  SD-eject notification arriving after the page was popped wrote through freed
  memory — reachable by pressing Back while a listing was still paginating.
- **Per-entry browse logging dropped to `ESP_LOGD`.** At 115200 baud a 20-entry
  page of INFO lines is a few hundred ms; once the listener mutex spans the
  callback, that became a stall for any page deregistering in `onExit`. The
  `StatisticsManager` mutex-trace INFO lines went with them.
- Also fixed while in the same function: the file browser leaked `browser` and
  its entry array when created without a comm interface.

Covered by six new host tests (`listener_slot_test.cpp`) pinning the contract
the concurrency is wrapped around: user_data paired with its own callback,
clearing stops invocation, and re-registering from inside a callback does not
deadlock.

### Fixed — LVGL thread safety: widgets are no longer touched from the UART task

Found by the 2026-08-29 ESP32-P4 review
([`docs/code_review_esp32_20260829.md`](docs/code_review_esp32_20260829.md),
items E-LVGL1/2/3). The LVGL port task renders on the other core, so every one
of these was a live object-tree/heap corruption race — the "display freeze"
class the code's own comments already described.

- **Input dispatch now holds the port lock.** `InputDispatcher::processAll()`
  ran unlocked while `onInput` handlers built and restyled widgets, contradicting
  the contract written down in `ui_navigator.cpp`. Four pages were affected
  (keyboard, sample edit, menu, and the global Shift softkey rebuild). The lock
  is taken around **each event** rather than around the whole queue drain: a
  backlog — a fast encoder spin queued while a page was still building — would
  otherwise hold it across every queued event back to back, stalling the render
  task for as many frames as there are events.
- **Lock order is now LVGL → UART**, recorded at the lock site. Handlers send
  over the link while holding the LVGL lock; `s_uart_mutex` is only ever held
  briefly and with a timeout, and never across `uart_write_bytes`. Nothing may
  take these in the other order — in particular the UART RX task's callbacks
  must stay flag-only.
- **Sample browser status callbacks no longer draw.** Play-bar position,
  status text, softkey rebuilds and load progress arrived on the UART RX task
  and wrote to LVGL directly; they are now staged behind release/acquire atomics
  and applied by the UI task in `processDeferredUpdates_()`.
- **Record page wave chunks no longer draw.** `handleWaveChunk` pushed 512
  chart writes per packet from the UART task; it now stages the chunk and a
  50 ms `lv_timer` renders it, matching the sample edit page.
- **`lv_async_call` is no longer used as an escape hatch from the wrong task.**
  It links an `lv_timer` itself, so calling it off the LVGL context races the
  list it is deferring onto. Removed from the file browser (an ~90-line
  `DEBUG: Direct refresh test` block that duplicated `update_file_browser_ui()`
  and dereferenced a browser that page teardown can free) and from two sample
  browser paths.
- **Busy overlay progress/hide are now task-safe** (`requestProgress`/
  `requestHide`/`service`), and live on the overlay rather than the page that
  started the load, so a completion still dismisses it after the user navigates
  away.

### Added — Voice parameter editing on the keyboard page (digital voice audition, stage 4)

- CUTOFF, RES, ATTACK, DECAY, SUSTAIN and RELEASE are editable from the
  Keyboard page and sent as `MSG_CONTROL_CHANGE`, completing the chain Stage 1
  built: an edit reaches sounding voices *and* the next trigger.
  `inter_mcu_send_control_change()` existed but had never had a caller.
- **The parameters live on the keyboard page rather than a page of their own,
  and that is forced, not a layout preference.** Leaving a page releases every
  held note, so a control on another page could never be swept against a
  sounding one.
- **Two ways to change a value, because the touchscreen alone cannot do it.**
  The LVGL port is single-touch, so a finger holding a pad cannot also drag a
  slider - "hold a note and sweep" is physically impossible with touch alone.
  So: the **physical encoder** (hold a pad, turn with the other hand), and
  **Latch** (tap to sustain, both hands free). `Value -` / `Value +` softkeys
  duplicate the encoder because the encoder is exactly the part that cannot be
  verified from here. Latched pads stay lit so what is sounding is visible.
- Values display in engine units (Hz, ms, percent) using the *same* mapping the
  Daisy applies, so the number on screen is the number the engine used rather
  than a second opinion about it. Initial values mirror `VoiceLiveParams`
  defaults, so opening the page does not change the sound before anything is
  touched.
- Root-note controls moved to the Shift row to make space; encoder direction
  takes the magnitude of `delta` and lets the event type supply the sign, per
  the global contract.

### Added — On-screen keyboard / pad grid (digital voice audition, stage 3)

- A 4x4 grid of touch pads under **Main Menu → Keyboard**, where cell *n* sends
  note `root + n` — which *is* a chromatic keyboard spanning 16 semitones, so
  the "virtual piano" and "pad grid" surfaces are one widget differing only in
  note map and labels. Softkeys shift the root by octave or semitone; pads are
  labelled with note names in scientific pitch notation (MIDI 60 = C4).
- **This is currently the only way to trigger a digital voice without external
  MIDI hardware**, and therefore the only way to hear the per-voice filter,
  envelope and live parameter edits at all. It needed no new protocol work:
  `inter_mcu_send_note_on/off()` already existed and were already in service
  from the MIDI task — nothing in the UI had ever called them.
- Uses `LV_EVENT_PRESSED`/`RELEASED` rather than the `LV_EVENT_CLICKED` every
  other button in this UI uses, because CLICKED fires on release only and would
  make every note zero-length; gate length follows the finger instead.
  `LV_EVENT_PRESS_LOST` is handled too — a finger that slides off a pad emits
  it *instead of* RELEASED, so without it that note would hang.
- Held notes are released on page exit and on any root change, and the note
  number sent at press is remembered per pad rather than recomputed at release,
  so a root change mid-press cannot end a note that was never started. An
  "All Off" panic softkey is there as cheap insurance while this page is the
  only note source.

### Added — Live voice-parameter editing on the digital path (digital voice audition, stage 1)

- `MSG_CONTROL_CHANGE` now reaches the **digital** per-voice filter and
  envelope, not only the Stage A analog VCF/VCA. Previously the per-voice
  filter and ADSR were written once at `Trigger()` time, so on an all-digital
  configuration a cutoff, resonance or envelope knob did nothing at all.
- `VoiceLiveParams` + `VoiceManager::ApplyLiveParams()` push edits onto
  **sounding** voices, driven at block rate from the audio callback behind a
  dirty flag so an unchanged parameter set costs nothing. `OnNoteOn` reads the
  same record, so an edit also carries forward to the next note instead of
  surviving only until one is played.
- **Filter and envelope edits are deliberately asymmetric.** Filter changes
  reach every sounding voice including ones in their release tail — a sweep
  that froze at note-off would sound like the filter jammed. Envelope changes
  skip releasing voices, because `Choke()` forces a short release onto a voice
  immediately before releasing it and rewriting the ADSR would hand back the
  full-length release mid-choke, so an open hat would not cut off.
- Each parameter now has **two** destinations rather than moving: the analog
  board is optional hardware and the digital voices always render, so a knob
  has to reach both. Cutoff needed a mapping the analog path does not — it
  passes the normalized value through as a CV — so the digital side maps
  exponentially over 20 Hz – 20 kHz; a linear map spends most of its travel
  above 10 kHz and crosses the whole musically useful range in the first few
  percent.
- Scoped **engine-global, not per-slot**, deliberately. `param-locks-and-
  modulation.md` scopes base values to an instrument slot, but nothing can
  address a slot differently yet, so a 16-entry table would be 16 copies with
  no way to reach 15 of them. It becomes per-slot with the instrument model.
- 5 host tests. **Not verified on hardware**: nobody has heard a sweep, and
  the block-rate cadence is exactly where zipper noise would appear.

### Changed — Resonant state-variable filter per voice (digital voice audition, stage 1)

- `OnePoleFilter` is replaced by `SvfFilter` (`src/audio/svf_filter.hpp`), a
  topology-preserving-transform state-variable lowpass with both cutoff and
  **resonance**. `VoiceTriggerParams` gains `filter_resonance` (0–1, default 0).
  The old header anticipated this swap; the reason to make it now is that
  `PARAM_FILTER_RESONANCE` had **no digital consumer at all** — a one-pole has
  no resonance state to give it, and a filter that cannot resonate barely
  exercises the voice architecture.
- **Chosen over a CMSIS-DSP biquad deliberately.** `arm_biquad_cascade_df1_f32`
  covers the *static* resonant lowpass, but it is a block kernel with fixed
  coefficients, and direct-form biquads behave badly when coefficients are
  modulated — the state no longer means what it meant under the previous
  coefficients, which is audible as zipper noise and, at high Q, as blow-ups.
  The TPT structure tolerates cutoff and resonance changing between samples,
  which is the whole point of the stage it serves: sweeping the filter *while a
  note sounds*. The block-kernel route stays open as a measured optimization.
- Rolloff goes 6 → 12 dB/octave, so existing material is filtered more steeply
  at the same cutoff. Resonance defaults to 0 (Q = 0.5, no peak) so a trigger
  that never asked for resonance does not get one. Cutoff at or above Nyquist
  remains an **exact** bypass, resonance included — the voice-manager tests
  depend on that contract to mean "no filtering at all".
- Filter state is in DTCM automatically, by containment in the DTCM-placed
  `s_voice_manager`; the measured cost is 872 → 1144 B of the 128 KB region.
  Coefficients are computed on tuning change rather than tabulated partly to
  keep it that way — a shared static table would land in cacheable AXI SRAM,
  not DTCM, however hot it is.
- 14 host tests (`svf_filter_test`) pin the bypass contract, monotone rolloff,
  the resonant peak, clamping of out-of-range resonance, and stability under
  per-sample cutoff and resonance sweeps. **Not measured on hardware**: the
  per-sample cost of a 2-pole against the old 1-pole across 8 voices still
  needs a DWT number, and FPSCR flush-to-zero should be confirmed rather than
  paying for a denormal guard on a guess.

### Added — Region fades and de-click at playback time (roadmap 1.5.6 item 3)

- A region that starts mid-waveform starts on a step from silence to whatever
  the sample happened to be doing at that frame, and a step is a click.
  `fade_in_ms` / `fade_out_ms` now ride on `SampleMetadata` (0x3D) and
  `MSG_SAMPLE_EDIT_SET` (0x3C), and are applied on **both** playback paths —
  the streaming audition and RAM voices — so the editor auditions what a pad
  plays. `SampleMetadata` grows 84 → 88 bytes and still fits `PKT_SIZE_128`.
- **De-click is the default, not an opt-in.** Both fields default to 1 ms. The
  step exists whether or not anyone asked for a fade, so the honest default is
  the one that removes it; 0 turns it off and is a real, reachable value. 1 ms
  is short enough to be inaudible against a drum transient, whose rise time is
  5–20 ms.
- The curve is **tabulated**, not computed. `VoiceManager::Render()` evaluates
  it per sample in the audio callback, where `cosf` is a library call of
  50–150 cycles with no worst-case guarantee — precisely what the real-time
  rules keep out of the callback. 257 entries (~1 KB) plus one interpolation
  is a handful of cycles and, more importantly, the same handful every time.
  Built at static-init time so no `__cxa_guard` lands on the callback's path.
- **Raised cosine, not linear** (`src/audio/fade.hpp`, host-tested): a linear
  ramp has a corner at each end, and a corner in amplitude is a discontinuity
  in the first derivative — audible as a faint thump on exactly the material a
  click was the problem on. Deliberately *not* the equal-power (sin/cos) shape
  the roadmap recommends for crossfades: equal power is right when two signals
  sum and the sum must hold level, but a fade to or from silence has nothing to
  hold level against, and an equal-power fade-in would start at −3 dB rather
  than zero — which is a step, i.e. the thing being fixed.
- The streaming fade is applied **before** resampling, because the fade
  position is a *source* frame index: after resampling the block no longer maps
  one-to-one onto file frames and the ramp would drift against the boundary it
  covers. That needed the SD buffer slots to record the file offset they were
  read from — by the time a block is converted the file handle has moved on.
- Fade lengths are counted at the **file's** rate, not the engine's. A 44.1 kHz
  sample on a 48 kHz engine advances 0.919 source frames per output frame, so
  using the engine rate would make the ramp 9% short in source terms.
- The backend clamps fades to the region, since only it knows what the region
  ended up being after its own clamping; a fade longer than the audio it shapes
  never reaches unity, which reads as "the sample got quieter" rather than as a
  fade. Overlapping fades share the region proportionally instead of
  multiplying into a permanent dip.
- The per-voice fade is separate from the ADSR and multiplied with it: the ADSR
  belongs to the instrument (how this note is played), the fade belongs to the
  sample (where its region was cut). Folding one into the other would make a
  marker move change the envelope.
- Sample edit page: FADE IN and FADE OUT join the paged parameter strip, 1 ms
  per detent up to 20 ms and 5 ms above it — a de-click lives in the first few
  milliseconds and a fade you hear as a fade lives above 50, so a single linear
  step would make one of the two useless. The gauge reads green while the value
  is doing the de-click job and blue once it is long enough to be a fade.
- **The loop seam is deliberately not covered.** Fades are anchored to the
  region start and end, so with looping on and the loop starting at the region
  start the fade-in re-fires each pass — a short dip, better than the click but
  not seam smoothing. That is the crossfade in roadmap 1.5.6 item 2.

### Changed

- Added required Daisy and ESP32-P4 coding-guide references to `AGENTS.md` and project-local platform skills under `skills/`.

### Added — Waveform envelopes, cached and mip-mapped (roadmap 1.5.5)

- **The preview aliased.** It sent every *n*th sample, so a bright sample drew
  a trace that did not resemble it and transients vanished entirely — the one
  sample kept per column is almost never the peak. `MSG_ENVELOPE_REQ` (0x3F) /
  `MSG_ENVELOPE_CHUNK` (0x44) send a min/max pair **per channel** per display
  column instead. The payload follows the display width, not the file length:
  1256 columns of stereo is ~10 KB for a whole file, whatever its duration.
- **Per channel, not summed**, decided now rather than after the format ships.
  An out-of-phase stereo sample sums to near silence and would draw as a flat
  line for audio that is perfectly fine, and a loop seam has to be judged on
  both channels.
- **The scan is a job, not a message handler.** A true envelope reads *every*
  sample in the window — that is the difference from decimation and the reason
  it does not alias — which is ~16 M reads for a three-minute stereo file.
  Doing that inline would stall the Daisy's main loop for ~100 ms, four times
  the audio ring's headroom, i.e. an audible dropout on every zoom.
  `PumpEnvelopeJob` measures ~24 k frames per main-loop pass, after the WAV
  refill, and sends each chunk as it completes. On a full TX queue it rewinds
  and retries next pass rather than blocking or punching a hole.
- **Mip-mapped cache in PSRAM** (`components/envelope_cache`), host-tested with
  an instrumented allocator. Tiers are powers of two derived from the view — the
  largest that still fits inside one display column — so merging tier columns
  into display columns is exact rather than approximate. Entries are contiguous
  windowed runs; `nextRequest()` returns only the part of the view not already
  held, so scrolling sideways fetches the newly exposed edge and nothing else,
  and a zoom step that lands inside a cached tier costs no round trip at all.
- The budget is measured, not assumed: an eighth of **free** PSRAM at page
  open, clamped to 128 KB–2 MB, with LRU eviction. LVGL's draw buffers and the
  display rotation path share that pool, and a cache that starves them would
  trade a fast waveform for a slow UI.
- **`generation` is part of the cache key**, not a field checked afterwards, so
  an entry from generation N can never be read for N+1. The Daisy now bumps it
  when a load rewrites a sample id that was already in use — a content change
  even though nothing renders destructively yet. Marker and gain edits do not
  bump it: they change what plays, not what the sample contains.
- Chunks still arrive on the UART RX task, which now must not touch the cache
  either, since the cache allocates. The callback assembles the run in a buffer
  allocated once at its ceiling; the UI timer hands the completed run over. An
  epoch counter is bumped before each request and re-checked after the copy, so
  a chunk cannot be filed against a request that changed underneath it, and an
  unanswered run times out after 3 s rather than wedging the page — the backend
  drops a scan when the sample under it is reloaded and does not say so.
- `WaveformView` gained `setEnvelope()`, drawn against a **fixed** full-scale
  range. An envelope that rescales itself cannot be read as a level, which is
  most of what it is for. It currently collapses stereo to the widest excursion
  of either channel; two stacked traces is roadmap 1.5.7 and the format no
  longer blocks it.
- The Link tab counts ENVELOPE_CHUNK, so the per-message table stays complete.
- The legacy `MSG_PREVIEW_REQ` / `MSG_WAVE_CHUNK` path stays for the record
  page and is unchanged.

### Added — Sample browser status strip, audition progress and pagination row

- The audition progress bar had no data source: the Daisy never reported
  playback position at all. It now emits position at a fifth of the meter
  rate — a progress bar does not need 20–50 Hz, and sharing the audition's
  existing send budget beats adding a second periodic sender. It reuses
  `MSG_SAMPLE_STATUS` state 1 with `sample_rate` carrying the *region*
  length, so the UI scales without a second message. This is the **read**
  position: it leads the audible one by the ring (~42 ms), which is invisible
  on a bar but makes it useless as a playhead.
- Status strip: directory and position in the listing on the left, card state
  on the right, tracked by a `storage_mounted` flag on the browser widget. It
  defaults to true, since the frontend cannot poll the slot — "unknown" and
  "present" are the same thing until `MSG_STORAGE_STATUS` says otherwise.
- Free space is **not** shown. The design's "SD 12.4 GB free" would have to be
  invented; neither `FileEntryWire` nor `StorageStatusMessage` carries
  capacity. The gap is recorded in `docs/roadmap.md` § 1.5.3 instead.
- Pagination spinner row (design 1b), deliberately not the modal busy overlay:
  pagination must not block the list, and rows already fetched stay usable
  while more load. It is polled from the UI task rather than created at each
  of the nine `pagination_in_progress` sites — several of those run on the
  UART task, and creating LVGL objects off the UI task is what froze the
  sample edit page.

### Added — Audition loop gap, sample-load progress, design 1b sample browser

- **Loop gap** rides on `SamplePlayIndexMessage`, *not* on `SampleMetadata`:
  it is a property of this audition rather than of the sample, so the caller
  decides. The browser passes 300 ms so a short file does not loop seamlessly
  and read as a drone; the editor passes 0 so the loop seam is heard exactly
  as it will play. The Daisy pushes silence into the ring at the rewind point
  rather than skipping the pass, so the gap is genuine silence and not an
  underrun — and it needs no SD read, which makes it free.
- **Load progress**: the Daisy emits `MSG_SAMPLE_STATUS` state `0x11` from
  inside the read loop, rate-limited to whole percent, with `frames_played`
  carrying the percentage. `0x11` is distinct from `0x10` (complete) so an
  older frontend ignores it rather than misreading a percentage as a frame
  count. The sender pumps the TX queue itself — it is 4 deep and nothing
  drains it from that context, so progress would otherwise be dropped.
- **Browser port to design 1b**: list 770×487, detail panel 474×521 with a
  filename headline, format and length rows, audition state and a progress
  bar. The three row-building sites in `wavex_file_browser` had drifted apart
  — different fonts, different selection colours, no duration at all — and now
  share one `fb_style_row` (54 px rows, green selection ring, dim
  directories, right-aligned duration), so the selection restyle cannot
  diverge from the initial build as it had.
- Two things deliberately **not** built: the design's "Data offset 44
  (aligned)" row, because `data_start` is carried by neither `FileEntryWire`
  nor `SampleMetadata` and inventing the one field that correlated exactly
  with the stutter would be worse than omitting it; and a waveform in the
  detail panel, because without an envelope cache a per-selection round trip
  would make scrolling unusable.

### Added — `SampleMetadata`: one authoritative record per sample

- Markers and gain applied to streaming audition only; `VoiceManager` ignored
  both, and the preview generator silently rendered the left channel. Those
  looked like three bugs but were one: there was no record for the playback
  and display paths to agree on, so each carried its own partial view.
- `SampleMetadata` (84 B, `MSG_SAMPLE_META` 0x3D) is owned by the Daisy and
  pushed on every change. `MSG_SAMPLE_META_REQ` (0x3E) asks for a resend, with
  id 0 meaning "every loaded sample" — which is how the frontend repopulates
  after its own restart without the backend tracking who has seen what.
- `MSG_SAMPLE_EDIT_SET` is now explicitly the **command** and the record is
  the **state**. That closes the gap where the backend clamped silently (it
  refuses loops under 256 frames and narrows out-of-order regions) while the
  UI kept displaying the request rather than the result. The edit page adopts
  any newer record, so a refused loop becomes visible instead of believed.
- Three consumers now read it: `OnNoteOn` fills `VoiceTriggerParams` from the
  record (every field it needed already existed; nothing was filling them), so
  a note-triggered voice plays the region the editor auditioned; the streaming
  reader derives its byte offsets through one `ApplyMetaToStreaming`; and
  `OnPreviewReq` takes its channel selection from `channel_mode` instead of a
  hard-coded left, with the 24-bit branch rewritten around a `read24` helper
  that had the same left-only assumption baked into its index arithmetic.
- `Resolve()` — sentinel expansion plus clamping — lives on the struct, so the
  rule is shared rather than reimplemented per consumer.

### Added — Sample edit page: real geometry, four markers, loop, gain

- The reported faults shared one root cause: `kSampleFrames` was hard-coded to
  48000, exactly one second at 48 kHz. That is why Start and End would not move
  past 1 s and why zoom appeared not to open out — it was already showing the
  whole "sample" it believed existed. Geometry now comes from the browse
  listing (rate, duration, channels, bits, size) via `SampleBrowserState`, so
  the page opens fully zoomed out with the region spanning the whole clip.
- New `MSG_SAMPLE_EDIT_SET` (0x3C) carries start, end, loop start, loop end, a
  loop flag and gain. The backend clamps and is the authority; it refuses a
  loop shorter than 256 frames, which would re-seek on every refill pass and
  starve the ring. `0` means "to the end" for `end_frame` and `loop_end`, so a
  frontend that does not know the file length can still send a region.
- Daisy: the refill reader caps at the region (or loop) end and rewinds to the
  loop point instead of the file start. Gain is a **saturating** q15 multiply
  on the converted block, written out rather than `arm_scale_q15` (not in this
  target's linked CMSIS set). Saturating deliberately: a wrapping multiply
  turns a hot sample into full-scale noise at the moment the user raises gain.
- UI: five parameters in four card slots, scrolled by `< Param` / `Param >` so
  the design's 305 px card pitch survives. Encoder steps scale to the *visible*
  span, not the whole sample, or a three-minute file would be unadjustable.
  Zoom anchors on the focused marker. Loop handles sit on the bottom edge so
  they never collide with S/E when the loop is at the region bounds.
- Sample loading shows the shared `ui_busy_overlay`, which is honest because
  the ESP32 is not blocked during a load — the Daisy does the SD work and
  answers over the link.
- Oversized samples are refused with both numbers on screen, checked against
  the allocator's **largest free block** rather than total free: extents are
  contiguous, so a fragmented pool with plenty of total space still cannot
  take a big sample.

### Added — Global Shift modifier for alternate softkey rows

- The sample edit page needs more operations than six softkeys hold.
  `UIPage::getShiftedSoftkeys()` opts a page in, `UINavigator` owns the state,
  and `BUTTON_SHIFT` is intercepted in `InputDispatcher` so no page can
  swallow the modifier by consuming `ButtonPress` for something else.
- **Latched, not held** — hold-and-press is awkward one-handed on a touch
  panel, and holding a key while turning the encoder is worse. **Sticky** — it
  clears after one shifted key fires, and on navigation, because a plain
  toggle gets left on and the next press does the wrong thing. **Inert rather
  than hidden** on pages with no alternate row, because a control that appears
  and disappears as you navigate is harder to learn than one that is always
  present and sometimes dim.
- Sample edit gets the first shifted row: Select / Loop / Gain / Save /
  Save As / Reset. All but Reset are disabled with their reason attached,
  since each needs protocol work that does not exist yet
  (`docs/roadmap.md` § 1.5.1). Its unshifted row is Back / Audition / Zoom − /
  Zoom + / `< Param` / `Param >`, with Audition toggling to Stop like the
  browser and stopping on page exit — it used to play on under a page that no
  longer existed.
- Physical Shift still needs a key: keycode 4 maps to `BUTTON_SHIFT`, but the
  matrix mapping is a three-key stub, so the header chip is the only way in.

### Fixed — WAV duration overflow, blank diagnostics tabs, sample-edit freeze

- **WAV duration was silently wrong.** `(frames * 1000u)` is a 32-bit multiply
  and overflows above 4,294,967 frames — 97.4 s at 44.1 kHz. A 3:00 file
  wrapped to 82 s and longer files wrapped repeatedly, so the reported figure
  looked like a plausible duration rather than like garbage, which is why it
  survived. Moved to a shared `WaveX::Wav::DurationMs` that promotes the
  multiply, with tests straddling the wrap point at 44.1 and 48 kHz. Every
  existing WAV fixture is a few seconds long, which is exactly how this
  reached hardware.
- **Blank diagnostics tabs.** The page refreshed only the tab named by
  `active_tab`, but nothing updated that member when the tab bar was touched —
  `lv_tabview` switches pages itself. `active_tab` stayed at `TAB_SYSTEM` and
  the refresh timer kept updating a tab nobody was looking at. Touch and
  softkey now share one `setActiveTab()` path, which also refreshes
  immediately rather than waiting for the next 500 ms tick.
- **Sample edit page froze on Audition and zoom.** `handleWaveChunk` runs in
  the UART RX task and called `lv_chart`/`lv_label` directly — LVGL from a
  non-UI task with no lock. The callback now only fills the buffer and raises
  a flag; a 50 ms `lv_timer` draws on the UI task. Two things had to change
  with it: the preview buffer is allocated once at a fixed ceiling and never
  resized, because a reallocation while the RX task is mid-copy is a
  use-after-free where a torn value is merely one stale frame; and encoder
  movement is coalesced into a single request after 150 ms instead of one
  preview request per detent.
- **CPU tiles merged.** ESP32 CPU is one tile (busier core as the headline, a
  bar per core, one sparkline) and the freed slot became DAISY CPU with its
  own sparkline — `HeartbeatMessage` already carries it, so no protocol
  change. Its old spot on the Link tab became FRAMES/s as a rate.

### Fixed — Sample Browser scrolling responsiveness (ESP32)

- Scrolling was never waiting on the Daisy — file metadata is served from the
  cached browse response — but each encoder detent paid for ~1 KiB of
  INFO-level logging pushed synchronously through the 115200-baud console
  (~100 ms per detent, much of it under the LVGL lock). `ESP_LOGx` output now
  goes through a non-blocking ring buffer (`main/log_ring`, mirroring the
  Daisy's `comm/log_ring`): writers append and never block, a lowest-priority
  task drains to the console, and on overflow the oldest bytes are dropped
  and counted, with gaps marked inline.
- Scroll-path log messages dropped from INFO to DEBUG (one INFO line per
  selection change remains). This includes the PCNT task's per-poll delta
  log, which fired every 2 ms while a knob turned.
- The PCNT1 detent accumulator previously posted one step and then reset to
  zero, discarding every count beyond the first detent per 32 ms poll window
  — fast turns lost most of their steps. It now consumes whole detents
  (`WAVEX_PCNT1_COUNTS_PER_DETENT` = 4, PEC11R 4x quadrature), carries the
  remainder, and posts the full detent count in one event.
- Moving the file-browser selection within the visible page now restyles the
  highlight in place; the destroy-and-recreate rebuild of all visible list
  buttons only runs when the viewport actually scrolls (or the entry list
  changes).

### Changed — Diagnostics page rebuilt as a five-tab layout

- Replaces three static text columns (roughly 90% empty space, every figure a
  since-boot total) with an `lv_tabview`: System / Audio / Link / Storage /
  MIDI, following `docs/ui-diagnostics-spec.md` and the WaveX Wireframes v2
  card anatomy (305x226 cards, 273x14 gauges).
- **System** shows eight cards from sources the ESP32 already has: CPU per
  core, internal heap, PSRAM, LVGL pool with fragmentation, task count,
  uptime and minimum-free-heap watermark. Gauges carry a per-card warning
  threshold rather than one blanket 85% rule, because on several of these a
  full bar is the healthy state.
- **Link** shows link state with heartbeat age, Daisy CPU (avg/min/max, the
  only backend figure `HeartbeatMessage` already carries), packet totals and
  error counts, plus a scrollable per-message-type table driven straight from
  `wavex_packet_stats_t` — 19 message types with no new plumbing.
- **Audio**, **Storage** and **MIDI** state what they are waiting for instead
  of rendering invented numbers: those figures live on the Daisy and do not
  cross the link until `MSG_DIAG_PUSH` exists.
- Softkeys are now Back / Tab < / Tab > / Freeze / — / Samples. **Freeze**
  holds the last values so a transient can actually be read; during the
  August audition debugging, values routinely changed faster than they could
  be noted.
- The sampling timer no longer formats display text. It samples the CPU
  counters (which need a steady cadence to mean anything) and flags the UI;
  each tab reads its own sources on the UI task when it is the visible one,
  so hidden tabs cost nothing.

### Changed — ESP32 links against Picolibc instead of Newlib

- `CONFIG_LIBC_PICOLIBC=y`. Picolibc has been selectable since ESP-IDF v5.0
  and becomes the default in v6.0, so this is available on the pinned v5.5
  toolchain today and removes one more difference from a future 6.x baseline.
- Measured on this project, not quoted from the vendor benchmark: the app
  image drops from 912,992 to 884,624 bytes, a 27.7 KiB (3.1%) saving. That
  is well short of the ~20% Espressif reports because their figure comes from
  a stdio-dominated microbenchmark, whereas this image is mostly LVGL and
  application code. The flash saving is therefore incidental — the reason to
  do it is the per-task stack and heap-allocation overhead on stdio paths,
  which is a runtime property this build cannot show and which wants
  confirming on hardware against `display_manager`'s DMA-heap telemetry.
- Safe for this project, checked rather than assumed: nothing in WaveX
  touches `struct _reent` or redirects `stdin`/`stdout`/`stderr` per task
  (the two documented `LIBC_PICOLIBC_NEWLIB_COMPATIBILITY` limitations), and
  no prebuilt archive in the dependency tree targets RISC-V — the NemaGFX
  libraries are ARM Cortex-M builds and the zl38063 blob is codec firmware,
  neither of which links on ESP32-P4. Revert with `CONFIG_LIBC_NEWLIB=y`.
- Note the project was on full Newlib, not newlib-nano, which is the
  comparison where Picolibc has the most room to help.

### Changed — ESP32 encoders move to the pulse_cnt driver

- `pcnt_task` now uses `driver/pulse_cnt.h` instead of the legacy
  `driver/pcnt.h`, which ESP-IDF removed in v6.0 along with the ADC, DAC,
  I2S, Timer Group, MCPWM, RMT, temperature-sensor and sigma-delta legacy
  drivers. The replacement API exists from IDF v5.0, so this builds on the
  pinned v5.5 toolchain today and removes the project's only hard blocker to
  a future 6.x upgrade (a full sweep of the removed 6.0 headers and symbols
  found no other use, in WaveX sources or in resolved components).
- Units and channels are opaque handles rather than fixed hardware indices,
  so `WAVEX_ENCODER_PCNT_UNIT` / `WAVEX_PCNT1_UNIT` are now WaveX logical
  indices (0/1) into this module's own table, and the `*_CH_A` / `*_CH_B`
  macros are gone — the driver allocates both quadrature channels from the
  unit handle, so there was no longer anything for them to select.
- Quadrature decoding is preserved exactly: each channel counts both edges of
  one signal and takes direction from the other's level, reproducing the
  legacy pos/neg + lctrl/hctrl matrix. Glitch filtering moves from a raw APB
  cycle count to `pcnt_unit_set_glitch_filter()`'s nanoseconds, so
  `WAVEX_ENCODER_FILTER_NS` / `WAVEX_PCNT1_FILTER_NS` (10 µs, the previous
  800 cycles at an assumed 80 MHz APB) are the new source of truth. **The
  converted filter width is the one behavioural change and wants confirming
  against real encoders.**
- Polling cadence, delta accumulation, counter-clear-on-change and the public
  entry points are unchanged, so `ui_task` and `wavex_application` call sites
  are untouched.

### Fixed — SD bring-up, driver-link leak, and remount retries

- `TrySpeed()` ran `f_mount(nullptr, ...)` and `HAL_SD_DeInit()` on every
  attempt including the first, de-initializing a peripheral that had never
  been brought up. `HAL_SD_DeInit()` invokes `HAL_SD_MspDeInit`, releasing
  clocks and GPIOs that libDaisy configures only from `HAL_SD_MspInit` inside
  `HAL_SD_Init` — which does not run until first disk access. No card would
  mount at any speed. Teardown is now gated on the peripheral actually having
  been brought up, so boot follows the sequence that worked before
  negotiation existed.
- `FatFSInterface::Init()` was called per attempt. It calls
  `FATFS_LinkDriver()`, which claims a slot in a fixed table of `_VOLUMES`
  (2) and is not idempotent, so the third speed always failed with
  "FatFS link failed". The driver is linked exactly once; the link is
  independent of the mount.
- A failed remount is retried every 2 s instead of being abandoned until the
  card is physically reseated — a card can report ready before it is readable.
- Mount attempts now log card type and capacity, and name the FatFS result
  (`FR_NO_FILESYSTEM` calls out exFAT, which this build cannot read).

### Fixed — Resampler phase continuity across chunk boundaries

- `LinearResampleFrames()` restarted at phase 0 on every call and stopped one
  frame short of its input, which is right for a self-contained buffer and
  wrong for successive chunks of one continuous stream: each boundary
  discarded up to a frame and jumped the fractional phase. Measured at one
  lost frame per chunk (4410 frames fed as 30 chunks produced 4770 output
  frames where 4800 is correct), i.e. a discontinuity every ~24 ms — roughly
  42 Hz, audible as warble.
- Added `ResampleStreamInterleaved()`, which carries the phase and one history
  frame per channel so the first output of a chunk interpolates against the
  last input of the previous one. Covered by four new tests pinning chunked
  output against whole-buffer output, total-output accounting, per-channel
  history for stereo and 8-channel, and state reset.

### Added — Audition exits automatically when storage goes away

- Ejecting the card, or exhausting SD read recovery, now tells the frontend
  via `Comm::NotifyStorageLost()`: a `MSG_SAMPLE_STOP_RESP` so the UI leaves
  audition mode, and an empty `MSG_BROWSE_RESP` so the browser clears its
  listing. Previously the Daisy fell silent while the ESP32 kept showing
  "Playing" over a file list it could no longer open. Both reuse existing
  message shapes, so no protocol change and no version bump.
- Sent on ejection even when nothing is playing, since the listing refers to
  files that are no longer reachable.
- ESP32: an empty browse response now clears the list. `loaded_entries` is
  reset only by `refresh_file_list()`, so an *unsolicited* empty response
  re-adopted the stale count and left the old files on screen — and with
  `current_page` non-zero it did not even mark a UI update.

### Changed — Audition telemetry

- Removed the per-pump `Transferred N frames` logging (96% of log volume
  during playback), the per-pass pre-buffer progress line, and the
  per-100-reads `SD I/O Stats` line that duplicated `SD PERF` and still
  printed ticks labelled as milliseconds.
- Added `RING: low_water`, the lowest ring occupancy seen per interval,
  sampled in the audio callback. Zero underruns only proves the ring never
  reached empty; it says nothing about how close it came, and a dip toward
  empty is audible well before zero.
- `WAV open:` now reports `data_start` with its frame and sector remainders.
  This identified the difference between files that stutter and files that do
  not: `data_start % 4 == 2` stutters, `== 0` does not.
- Fixed `SD PERF` KB/s integer truncation, which read about 3% low.

### Added — Sample browser responds to card insertion

- New `MSG_STORAGE_STATUS` (0x39, backend → frontend, unsolicited) reports the
  SD card mounting or being lost. The frontend has no view of the card slot
  and cannot poll for this, so insertion was previously invisible: the browser
  only re-listed when the user left the page and came back.
- The file browser subscribes to it and re-lists its current path when a card
  appears, so swapping cards while the browser is open now works without
  leaving the menu.
- Sent on successful (re)mount and alongside the existing loss notification.

### Fixed — Boot-time UART framing error reported as a fault

- `0x0004` is `HAL_UART_ERROR_FE`. Until the ESP32 boots and drives its UART
  pin, the Daisy's RX line is undriven, so the receiver sees a start bit with
  no valid stop bit. The Daisy is up long before the ESP32 finishes ESP-IDF,
  LVGL and display init, so exactly one framing error at boot is expected. It
  is now reported as such before the first frame arrives, and error bits are
  named rather than printed as a bare hex code. The DMA reset still runs — it
  is the right response, just not an incident.

### Fixed — SD remount after card insertion

- `SD_initialize()` (libDaisy `sd_diskio.c:106`) leaves its `Stat` static
  unchanged when `BSP_SD_Init()` fails, so a failed re-init returned the stale
  "ready" status from boot. FatFS then skipped its `STA_NOINIT` check and read
  from an uninitialized peripheral, making every bus clock report
  `FR_DISK_ERR` while the real failure stayed invisible. The card is now
  brought up explicitly with `HAL_SD_Init()` + `HAL_SD_ConfigWideBusOperation()`,
  checked, and logged with the HAL error and card state on failure.
- Added a 200 ms settle delay after insertion: card-detect closes before the
  card is electrically ready, and the debounce covers switch bounce, not
  power-up.

### Added — UART link instrumentation

- `WAVEX_DAISY_UART_PERF_DEBUG` (default 0) reports per-interval link cost:
  call count, total/avg/max microseconds spent in `UartLinkProcess()`, the
  percentage of the interval that represents, RX/TX bytes and frames, and error
  deltas. `total_us` is the figure that answers whether the link competes with
  the audio ring refill — nothing previously measured it. Accumulators reset on
  read, so each line describes its own interval.
- Added RX/TX byte counters to the link stats.

### Added — docs/backlog.md

- New home for unscheduled work, kept out of `roadmap.md` so phase gates stay
  readable. Each entry records why the item is not urgent, so the reasoning can
  be re-checked rather than re-derived. Seeded with the runtime-tunable logging
  bitmask, the SPI-link question (with the measurements arguing against it for
  now), the non-frame-aligned WAV `data` chunk finding, and the open SD remount
  verification.

### Added — UI design constraints brief

- New `docs/ui-design-constraints.md`: a one-page, paste-ready brief for
  design passes (Claude Design or human), with every claim sourced to code —
  1280×720 landscape (720×1280 panel, software-rotated), LVGL 9.4 at RGB565,
  75 px header + 100 px six-softkey bar leaving a 1280×545 content area,
  Montserrat-only typography at the eight compiled-in sizes, the existing dark
  palette, the 20-line-strip rendering budget that rules out large animated
  regions, the enabled widget inventory, and encoder-first input.
- `ui-architecture.md` re-verified against code and updated: hardware summary
  added, dead `.cursor/rules` reference corrected (untracked local file, not
  shared truth).
- Backlog: recorded a GT911 touch-range mismatch found during the audit —
  `x_max=800, y_max=480` configured against a 720×1280 panel; flagged for
  corner-tap verification rather than a blind fix.

### Added — Off-host UI preview harness (tools/ui_preview)

- Compiles the same vendored LVGL 9.4 the firmware uses for the host and
  renders WaveX screens to BMP at the logical 1280×720 — same fonts, palette
  and chrome geometry as the device, no hardware or serial dump needed.
  Renders a replica of the current diagnostics page plus proposed tabbed
  diagnostics and sample-browser designs, for iterating with design tools.
- These are previews for design work, not captures of the firmware pages
  (those depend on ESP-IDF services; compiling them against the existing test
  mocks is the natural next step).

### Added — On-device screenshots over serial (debug builds)

- `WAVEX_ESP_SCREENSHOT_DEBUG` (follows `WAVEX_DEBUG_LOGGING_ENABLED`, this
  codebase's debug/release switch — the ESP32 compiles `OPTIMIZATION_PERF`
  even day-to-day, so the compiler's notion of a debug build would never
  fire): a listener task watches the console UART for `WAVEX-SCREENSHOT`,
  the UI task captures the active LVGL screen into PSRAM under the LVGL lock
  (`lv_snapshot`, now enabled in sdkconfig), and the listener prints it as
  RLE+base64 RGB565 between markers — so the UI never blocks on the
  seconds-long 115200-baud dump.
- `scripts/esp32_screenshot.py` triggers a capture and decodes to PNG with
  CRC verification. By default it writes the trigger to the tty and harvests
  the dump from `logs/esp32.log`, so it coexists with the running serial
  logger; `--direct` reads the port when no logger holds it. Decoder
  round-trip verified against a synthetic dump.

### Added — WaveX Wireframes v2 rendered in LVGL

- `tools/ui_preview` now implements all seven screens from the Claude Design
  project *WaveX Wireframes v2* using that design's own coordinates, colours
  and sample content: Diagnostics System/Audio/Link/Storage/MIDI, the refined
  sample browser, and a new sample-edit screen. Renders are pixel-true at
  1280×720 with the device's fonts, so they can be compared against the
  wireframe directly and reused as the reference for the firmware port.
- Two design rules are encoded rather than approximated: gauge fills only turn
  orange at a real warning threshold (a full pre-buffer or plenty of free
  space stays green — high is not universally bad), and cards omit the gauge
  entirely when the metric has no budget, so there are no empty tracks.

### Added — Diagnostics page specification

- New `docs/ui-diagnostics-spec.md`: tab-by-tab content for the diagnostics
  screen (System / Audio / Link / Storage / MIDI), each stat marked as already
  available on the ESP32, present on the Daisy but needing a wire message, or
  needing new instrumentation — plus the reason each figure earns its place,
  drawn from the August 2026 audition debugging where none of the numbers that
  actually identified faults were on the page.
- Specifies `MSG_DIAG_SUBSCRIBE`/`MSG_DIAG_PUSH` (94 bytes, fits `PKT_SIZE_128`,
  ~188 B/s at 2 Hz) as the single addition that unblocks most of the content,
  and two presentation rules: per-interval counters that reset on read, and
  sparklines over bare numbers.

### Added — Firmware flash and serial log workflow

- Added `make flash-all` to stop serial loggers, flash Daisy and ESP32 in
  parallel, wait for both operations, and restart logging with the result
  status preserved.
- Added canonical `make start-logs` and `make stop-logs` targets with
  configurable log rotation (`LOG_KEEP`, default 4); the existing
  `logs-start` and `logs-stop` names remain compatible aliases.
- Log rotation now copies and truncates the primary `.log` files in place,
  preserving their inodes for existing `tail -f` sessions.

### Added — Daisy development and audition tooling

- Added software-triggered Daisy DFU entry (`make daisy-flash-auto`), serial
  logging with reconnect support (`make logs-start` / `make logs-stop`), and
  USB serial-device discovery helpers.
- Added CodeGraph configuration for focused repository indexing and installed
  `vim-tiny` in the devcontainer image.

### Fixed — SD read failures now recover instead of killing playback

- A failed streaming read had no recovery path: `refill_sd_buffer()` returned
  `false` on every subsequent pump forever, so audio stopped permanently and
  only stopping and re-triggering the audition by hand brought it back. FatFS
  latches a disk error into the `FIL`, after which every `f_read` fails
  immediately and nothing short of a fresh `f_open` clears it — which is
  exactly what the manual workaround was doing. The engine now performs that
  reopen automatically, resuming from the current byte offset, capped at 5
  recoveries per file so a genuinely dead card ends playback cleanly instead
  of reopening forever.

### Added — SD card hot-swap, and auto-format disabled by default

- `SdSdio::Poll()` watches the card-detect pin (debounced 250 ms) and handles
  hot-swap: unmount on removal, remount with a fresh bus-clock negotiation on
  insertion, so a different card can be swapped in without a reboot. A new
  card renegotiates from the configured start rather than inheriting the
  previous card's rate. Playback stops cleanly on removal instead of failing
  reads against a card that is physically gone.
- **`WAVEX_DAISY_SD_AUTO_FORMAT` added, defaulting to 0, and `main()` no longer
  passes `true`.** FatFS reports `FR_NO_FILESYSTEM` for a card whose boot
  sector could not be *read*, not only for one that has no filesystem — and
  read corruption is a demonstrated failure mode here (SDMMC data CRC errors).
  The previous unconditional `auto_format = true` meant a single corrupted
  boot-sector read could erase a user's card.
- Added per-interval SD throughput and latency reporting under
  `WAVEX_DAISY_SD_DEBUG` (`SD PERF:` — KB/s, read count, avg/min/max latency,
  and the negotiated bus clock). Accumulators reset on read so each report
  describes its own interval, and rates use the measured `dt` rather than the
  nominal 5 s.

### Added — SD bus clock negotiation

- `SdSdio::InitAndMount()` now starts at `WAVEX_DAISY_SD_CARD_SPEED` and steps
  down one bus clock at a time until the card both mounts and reads, so a card
  or harness that cannot hold the configured rate lands on the fastest rate it
  can instead of failing. Each candidate is proven with a real directory read,
  since a mount that succeeds but cannot be read is exactly what a marginal
  clock looks like.
- Added `SdSdio::DowngradeSpeed()`, invoked from the streaming recovery path
  when `HAL_SD_GetError()` reports `SDMMC_ERROR_DATA_CRC_FAIL`. Marginal timing
  only shows up after sustained transfer, so a boot probe cannot catch it;
  reopening at the same clock would just fail again. Restricted to CRC:
  timeouts and absent cards are not fixed by going slower.
- The default start moves from STANDARD (25 MHz) to FAST (50 MHz), which
  negotiation makes safe. Watch for `SD: negotiated DOWN` or `SD: downgrading`
  — either means that board is not holding the configured rate.

### Fixed — Failed SD reads no longer spin the main loop

- After a read failure the pump retried as fast as the loop ran — measured at
  ~40,000 failed reads per second, each returning in ~8 µs — burning the main
  loop and flooding the log, when a poisoned `FIL` cannot succeed until it is
  reopened. Retries now back off 20 ms, well inside the ring's ~42 ms of
  headroom, so a transient error costs no audio if the next attempt succeeds.
- The failure line now reports `HAL_SD_GetError()` and the card state
  alongside the FatFS code, since `FR_DISK_ERR` only means "disk_read said
  no" and says nothing about why the SDMMC layer refused.

### Fixed — Silent SD read failures

- A failing `f_read` in the streaming refill was invisible: the error log sat
  behind `WAVEX_DAISY_SD_DEBUG`, and `s_io_duration` was recorded before the
  error check while `s_io_count++` came after it. A card that stopped
  responding therefore presented as "count frozen, last still changing" with
  no error anywhere — audio stopped while the log looked healthy. Read
  failures are now logged unconditionally (rate-limited to 1/s) with the
  FatFS result code, counted separately from successful reads, and surfaced
  in the periodic stats.
- `WAVEX_DAISY_SD_DEBUG` and `WAVEX_DAISY_STREAM_DEBUG` temporarily default to
  1 to exercise the non-blocking log ring under playback load. Both revert to
  0 once confirmed.

### Added — Loop-point and output-level telemetry

- The WAV loop point (EOF rewind) is now logged unconditionally with its
  period and `f_lseek` duration. It previously sat behind
  `WAVEX_DAISY_SD_DEBUG`, and `f_lseek` falls outside the timer that wraps
  only `f_read`, so a file looping — and any cost of doing so — was invisible
  in `I/O Stats`. It fires once per pass through the file, so it cannot spam.
- The periodic stats now include output peak/RMS, separating a healthy audio
  clock that is emitting silence (a data problem) from one whose output never
  reaches the ear (codec, SAI, or analog).

### Fixed — SDMMC interrupt outranked audio

- `SDMMC1_IRQn` ran at priority 0 (installed by libDaisy `per/sdmmc.cpp:84`),
  above the audio SAI DMA at 5, inverting the architecture.md §7.1.5 rule that
  audio is highest. SD init runs before the priority block in `main()`, so
  nothing had corrected it and every SD transfer's interrupt could preempt the
  audio callback. Now set to 8 — below audio, above the SPI link. Safe to
  demote: SDMMC1 moves data through its own IDMA, so this IRQ only signals
  completion.
- The periodic stats line now carries `now`/`dt`/`blocks`/`cpu`/`logdrop`.
  `blocks` counts audio callbacks, which the SAI DMA drives independently of
  the main loop, so it distinguishes a starved ring from a callback that
  stopped running — the latter reports no underrun at all, since underruns are
  only detected inside the callback.

### Fixed — SD card speed configuration

- `WAVEX_DAISY_SD_CARD_SPEED` documented three mutually contradictory sets of
  clock figures (20/40 MHz in one comment, 12.5/25 MHz in another, neither
  matching libDaisy). Replaced with values derived from the clock tree:
  `SDMMC_CK = sdmmc_ker_ck / (2 x ClockDiv)` with `sdmmc_ker_ck = PLL2R =
  200 MHz`, giving 400 kHz / 12.5 / 25 / 50 / 100 MHz. The speed itself is
  unchanged (STANDARD, 25 MHz).
- Setting 4 (VERY_FAST) was selectable but unreachable: the switch in
  `sd_sdio.cpp` had no case for it and fell through to `default: STANDARD`,
  and the `speed_names[]` log array had only four entries, so that setting
  would also have read out of bounds. Both fixed, with a `static_assert`
  binding the array to the macro's range.
- Documented that raising this setting yields roughly 10%, not 2x: most of an
  8 KiB read is command and card-state polling overhead in libDaisy's
  `SD_read()`, not transfer time.

### Fixed — Logging no longer stalls the audio ring refill

- Daisy logging now goes through a non-blocking ring buffer
  (`src/comm/log_ring.h`) drained one USB packet per main-loop pass, replacing
  `DaisySeed::PrintLine`. libDaisy's `Logger` latches into blocking mode after
  two successful packets (`hid/logger.cpp:73`) and then spins unbounded —
  `while(false == impl_.Transmit(...)) {}`, no timeout — waiting for the USB
  host to drain the CDC endpoint. This happens even with `StartLog(false)`.
  Since the main loop is the only thing refilling the audio ring, every log
  line was an unbounded stall bounded only by how fast the host read the port:
  the root cause behind the audition underruns, and why each reduction in log
  volume improved audio. Overflow now drops the oldest bytes and counts them
  (`WaveX::Log::DroppedBytes()`) instead of ever blocking audio.

### Added — Resampler test coverage

- Extracted the streaming linear resampler from `audio_engine.cpp` into
  header-only, HAL-free `src/audio/linear_resampler.hpp` and added 12 host
  tests. It had no coverage while being the subject of three playback-stall
  fixes, all edge cases in how many frames a pass may consume or produce.
- The per-channel core now reads through a stride, so the per-channel
  de-interleave copy (and the scratch buffer it required) is gone — one fewer
  full pass over each chunk. Mono, stereo and the Stage B 8-channel layout all
  run through the same core, and callers may drive one channel at a time.
- Arithmetic is bit-identical to the CMSIS `arm_linear_interp_q15` it
  replaces, pinned by test. Retires that function's packed 12.20 index, whose
  sign bit capped a usable source at 2047 frames and silently returned the
  first sample for every position past it (latent — callers were bounded below
  that by ring capacity).
- Testing surfaced a pre-existing quirk, preserved deliberately and now
  pinned: the interpolator weights sum to 2^20-1 rather than 2^20, so every
  output is 1 LSB low and unity-ratio resampling is not bit-transparent.
  ~-120 dBFS; worth correcting when the polyphase rewrite rebuilds the
  weights anyway.

### Fixed — Audition underruns from diagnostic logging

- Rate-limited the ring-buffer underrun log to one line per second with an
  episode count. It logged once per underrun episode, which under intermittent
  starvation meant a blocking USB CDC write every few main-loop passes —
  13k+ lines in a single audition — stealing the main-loop time the ring
  refill needs. The logging deepened the starvation it was reporting.
- Inverted the periodic stats throttle: report every interval while playing
  and every 10th when idle. Throttling during playback produced no stats at
  all for any audition shorter than 50 s — no data exactly when the ring is
  under load.
- Hoisted `System::GetTickFreq()` out of the per-pump I/O timing path; it
  reaches `HAL_RCC_GetSysClockFreq()`, which recomputes the PLL tree in
  floating point.

### Fixed — Daisy sample audition

- Prevented non-48 kHz sample prebuffering from spinning when only one frame
  remains, allowing playback to proceed instead of stalling silently.
- Fixed the matching stall in the streaming path: a 1-frame SD-slot tail
  cannot be resampled (the linear resampler needs ≥2 input frames), and the
  skip-without-consume retry then re-requested the same frame forever —
  playback of non-48 kHz files froze into permanent underrun after ~15 s.
  The 1-frame tail is now retired with the slot. Both stalls date to the
  2026-07-03 pair `bb3f2cb` (engine 44.1 → 48 kHz, so 44.1 kHz files began
  resampling at all) + `db0b626` (Finding 6 made resample-failure paths skip
  without consuming); needs a hardware listen test.
- Raised the LONG I/O log threshold to 10 ms and report the periodic
  I/O/stream stats only every 10th interval while a sample is playing — a
  normal resampling pump costs 1.7–5 ms, so the previous 1 ms threshold
  logged every pump and the blocking USB CDC writes starved the ring-buffer
  refill, causing the very underruns being reported.
- Corrected Daisy I/O timing diagnostics to use microseconds and put the
  stream-state telemetry behind `WAVEX_DAISY_STREAM_DEBUG` (off by default)
  rather than deleting it, since resample-path stalls are invisible without
  it.
- Fixed bogus `LONG I/O: 4273494187 us` readings: the measurement subtracted
  two `System::GetUs()` values, but that counter wraps at 2^32/200 =
  21474836 (not a power of two), so any pump spanning the ~21.5 s wrap
  produced garbage. Timing now subtracts raw `GetTick()` values, which wrap
  exactly at 2^32, and converts once via `GetTickFreq()`.

### Fixed — DMA and memory allocation

- Daisy UART4 now uses simultaneous non-blocking DMA: continuous circular RX
  on DMA1 Stream 5 and asynchronous TX on DMA2 Stream 4. A WaveX-owned HAL
  transport bypasses libDaisy v8.1.0's single-operation UART scheduler; only
  one in-flight TX frame occupies D2 DMA RAM while the queue remains in AXI
  SRAM. UART RX position handling is placed in ITCM and all three IRQs remain
  below audio.
- SDRAM failure now disables sample-RAM operations instead of leaving an
  allocator pointed at unavailable memory. The centralized layout gives the
  sample allocator 60 MiB and reserves 4 MiB for offline render scratch.
- The small-sample slab reservation is reduced from an unreachable 4 MiB to
  256 KiB, and statistics now report the whole reserved slab arena rather
  than only lazily formatted pages.
- Replaced the Daisy loaded-sample `std::vector` with a fixed 32-entry registry
  so sample loads do not grow or fragment the bare-metal heap. The registry
  retires its least recently loaded entry when it runs out of slots or when the
  sample arena cannot fit an incoming load, so auditioning is no longer capped
  at 32 loads per boot and no longer strands SDRAM the browser can never
  reclaim (it allocates a fresh `sample_id` per audition). Re-loading an
  existing id now moves that entry to the newest slot, keeping the
  "most recently loaded sample" that note playback and preview select correct.
- Added a copied-at-boot ITCM section, a 30 KiB D2 DMA link-time ceiling, a
  DTCM static-data ceiling that preserves 64 KiB for stacks, and ESP32
  capability-specific internal/PSRAM DMA heap telemetry around display
  initialization.

### Fixed — ESP32 CPU usage diagnostics

- The diagnostics page reported a constant `Total=100.0% Core0=100.0%
  Core1=100.0%`, which was a parsing artifact rather than a measurement.
  FreeRTOS pads task names with spaces to `configMAX_TASK_NAME_LEN-1`, so
  the run-time-stats field reads `"IDLE0          "` and never matched
  `strcmp(name, "IDLE0")`; both idle deltas stayed zero and every sample
  computed `100 - 0`. Trailing padding is now trimmed before comparison.
- Per-core usage divided one core's idle time by the run-time total summed
  across BOTH cores, halving every idle fraction — a fully idle system would
  have read 50%, never 0%. The denominator is now per-core.

### Fixed — Daisy→ESP32 UART duplicate frames

- The TX pump re-sent an already-delivered frame whenever its DMA completion
  landed between the result poll and the busy check: the completed head
  entry was never retired, went out again with the same sequence number
  (the ESP32's steady `RX seq=N dropped (duplicate), expected=N+1`
  warnings, ~2–4% of frames), and `StartTransmit()` silently zeroed the
  un-taken success result. The in-flight gate now runs first, so a taken
  result is always final. Any remaining retransmit (the deliberate
  retry-after-failure policy) now logs `UART TX resend seq=N`.
- A UART receive error (PE/FE/NE/ORE) no longer fails the in-flight
  transmit: the old handler marked the TX failed on any error and cleared
  the busy flag while TX DMA could still be running, allowing the next send
  to `memcpy` into the DMA buffer mid-transfer. TX is now only failed when
  the HAL actually aborted it (gState left `BUSY_TX`). Latent on current
  hardware — the error path has never been observed to fire — but it was a
  real corruption window.

### Docs

- `architecture.md` §4.2: recorded the "Why bare-metal, not an RTOS" decision
  for the Daisy backend — the audio DMA clock is the timebase (sequencer events
  counted in audio frames, phase-locked to the callback), the workload is a
  two-level foreground/background split, hand-off is lock-free (the callback
  never blocks on a mutex), and the real timing risk is callback CPU budget,
  which an RTOS would only worsen. Cross-linked from §5.1.

### Added — Instrument model core (Phase 2.5 stage 1)

- `firmware/daisy/src/audio/instrument.hpp` per `instrument-model.md` §2-3:
  the E-mu-lineage `Zone` / `Instrument` / `InstrumentBank` keymap model and
  `ResolveNoteOn()`, which turns an incoming note into up to
  `kMaxLayerTriggers` `VoiceTriggerParams` ready for `VoiceManager::Trigger`.
  HAL-free — sample bytes are looked up through a caller-supplied
  `SampleResolver` (function pointer + context, no `std::function`), so zone
  key/velocity matching, velocity switch/layer/crossfade, coarse+fine tuning
  fold (`TuneRatio` → `pitch_ratio_mul`), zone gain (→ `gain_mul`), choke
  group, region/loop, and drum-vs-keyboard note handling are all testable
  without SDRAM or the audio HAL. A kit is a drum-mode instrument
  (`instrument-model.md` §8), so the Phase 2 sequencer's per-track sample
  lookup resolves through exactly this path once wired.
- 18 host tests (`instrument_test.cpp`): range matching, velocity
  switch/layer, layer cap, unresolved-sample skip, drum root-forcing,
  tune/gain/region flow-through, both crossfade directions, and bank slot
  routing. Host tests green; `instrument.hpp` ARM-compiles clean.
- Not yet wired: the sample table that populates a `SampleResolver` from
  loaded WAVs (stage 4) and the callback note path calling `ResolveNoteOn`
  (replaces the item-8 stopgap) land next, together enabling audible
  sequencer/kit playback.

### Added — Voice manager extensions for the instrument model (Phase 2.5 stage 2)

- `voice_manager.hpp` per `instrument-model.md` §3/§10: `Voice` gains `slot`
  and `choke_group`; `VoiceTriggerParams` gains `slot`, `choke_group`, and
  the identity-default multipliers `gain_mul` / `pitch_ratio_mul` (so the
  instrument layer can fold in zone gain and coarse/fine/scale tuning without
  re-deriving the base velocity/pitch). `VoiceManager::Choke(group,
  fast_release_s)` mutually-excludes a choke group (open/closed hat) via a
  forced fast release; `StopSlot(slot)` hard-stops only one slot's voices (so
  rebinding one instrument slot doesn't cut the others). `Trigger()` applies
  choke before allocating so a voice can't choke itself.
- `envelope.hpp`: `SetReleaseTime()` (reconfigure release only, for choke).
- 6 new host tests (gain/pitch multipliers, slot+choke storage, choke cuts
  same-group / spares other groups, StopSlot scoping). Host tests green and
  the Daisy ARM firmware links clean.

### Added — Sequencer message dispatch + engine forwarding (Phase 2 wiring)

- `daisy_inter_mcu_message_handlers.cpp` now routes `MSG_SEQ_TRANSPORT`,
  `MSG_SEQ_PATTERN_OP`, `MSG_MIDI_CLOCK_EVENT`, and `MSG_MIDI_CC` (with
  payload-size validation) to new `AudioEngine::OnSeqTransport` /
  `OnSeqPatternOp` / `OnMidiClockEvent` / `OnMidiCc` hooks. `audio_engine.cpp`
  implements them as **real forwarding** to an engine-owned
  `SequencerTransport` (not log-only stubs — the C1 lesson).
- 5 new dispatch host tests (`message_dispatch_test.cpp`, driving the real
  dispatcher against recording mocks) pin each routed type to its subsystem
  call plus truncated-payload rejection, so a handler can't silently regress
  to a stub.
- **Deliberately deferred to the next stage** (documented at the
  `s_seq_transport` declaration): the transport is mutated only from
  main-loop message context and is **not yet driven from the audio
  callback**. Advancing the scheduler from the 1 kHz control tick and turning
  its `TriggerEvent`s into voice triggers needs the double-buffered
  edit-between-steps discipline (`sequencer.md` §4) and a track→sample kit
  mapping (instrument model, Phase 2.5). Until then the transport accumulates
  fully unit-tested state but does not yet produce audio.

### Added — Sequencer transport controller (host-tested glue)

- `firmware/daisy/src/sequencer/sequencer_transport.hpp`: `SequencerTransport`
  binds the two engine cores (`SequencerScheduler` + `TempoFollower`) to the
  Phase 2 wire messages. HAL-free and host-tested; it is the layer the audio
  callback will drive (`Tick()`) and the dispatcher will feed (`Apply*`/
  `OnMidi*`), keeping all of that logic testable before touching
  `audio_engine.cpp`.
  - `ApplyPatternOp` maps every `SeqPatternOpCode` onto the `Pattern` model
    with wire-untrusted bounds checks (out-of-range track/step is a silent
    no-op) and value clamping (velocity ≤127, probability ≤100, swing
    50–75, length 1–64). Param-lock set overwrites in place / evicts oldest.
  - `ApplyTransport` handles play/stop/continue, tempo, and clock source.
    Internal mode starts immediately; MIDI mode **arms** and starts the
    scheduler on MIDI START so sequencer step 0 aligns to the master
    downbeat. `Tick()` in MIDI mode syncs the scheduler tempo to the
    follower's servo-corrected instantaneous BPM each tick.
  - `BuildPlayhead()` produces the coalesced `SeqPlayheadMessage`.
- Small additive core support: `TempoFollower::InstantaneousBpm()` (raw
  estimate × phase-servo trim, factored via a new `CurrentTrim()` helper),
  and `SequencerScheduler::PlayheadStep()`/`PlayheadLoop()` (track-0 grid
  crossings define the global playhead).
- 19 new host tests (`sequencer_transport_test.cpp`): pattern-op edits +
  clamping + bounds, internal transport timing, tempo from wire, MIDI
  arm→START→lock→tempo-track→STOP, playhead, MIDI-CC forwarding.

### Added — Phase 2 sequencer / transport / MIDI-clock protocol messages

- `firmware/shared/spi_protocol/protocol.h`: seven new wire messages in the
  reserved 0x50–0x57 block (`docs/features/feature-expansion-ideas.md`):
  `MSG_SEQ_TRANSPORT` (0x50), `MSG_SEQ_PATTERN_OP` (0x51), `MSG_SEQ_PLAYHEAD`
  (0x53), `MSG_MIDI_CLOCK_EVENT` (0x55), `MSG_MIDI_CC` (0x56),
  `MSG_SEQ_CLOCK_OUT` (0x57), plus `MSG_SEQ_PATTERN_SYNC` (0x52) reserved as
  an enum value (bulk sync deferred; project persistence uses WXCF on SD).
  All structs follow the packed + named-constructor + zero-default
  convention. `SeqPatternOpMessage` is a compact idempotent-op envelope
  (op-code selects which of track/step/arg_* apply — table documented at the
  struct). `MidiClockEventMessage` carries the ESP-domain **delta** between
  events, never an absolute timestamp, baking the clock-domain-safety rule
  (`midi-sync-tempo-follower.md` §2) into the wire contract.
- 7 round-trip host tests (`message_types_test.cpp`), incl. signed
  micro-offset survival; `docs/features/inter-mcu-protocol.md` catalog rows.

### Added — WXCF chunk container (roadmap Phase 2 item 6 / Phase 2.5 shared infra)

- `firmware/shared/wxcf/wxcf.hpp`: `Writer`/`Reader` per
  `docs/features/instrument-model.md` §5 - one versioned little-endian TLV
  container format (`magic "WXCF" + file_type/file_version/total_len`
  header, then `chunk_id/chunk_version/payload_len` chunks) shared by every
  planned SD artifact: instruments (`.wxi`), tunings (`.wxt`), scenes/mixer
  project chunks, and eventually patterns/projects (`.wxp`). HAL-free:
  I/O goes through a context-pointer + function-pointer `IoContext`
  (deliberately not `std::function`, so the header stays includable from
  the Daisy's C++14 firmware build without pulling in heap-allocation-
  capable machinery), so it round-trips against an in-memory buffer on
  host and will wrap FatFs `f_read`/`f_write` on target with a thin
  adapter (not implemented in this stage). Forward-compatibility is
  provided by `Reader::SkipPayload()` (unrecognized `chunk_id`s can be
  skipped without a schema-specific buffer) and `VersionMajor()`/
  `VersionMinor()` helpers for the caller's own major-version-reject
  policy - this class only frames bytes, it doesn't know any file_type's
  chunk schema.
  - Atomic-save (temp file + rename) is explicitly out of scope for this
    class - it has no notion of files or paths, only a byte stream. That
    discipline belongs to whatever wraps this with real FatFs calls.
- 11 new host tests (`wxcf_test.cpp`): header/chunk round-trip, unknown-
  chunk skip forward-compatibility, skip-payload across a multi-iteration
  (>64 B) scratch buffer, bad magic / truncated header / truncated payload
  error paths, version major/minor packing, on-wire little-endian byte
  order, and I/O-callback-failure propagation.

### Added — MIDI clock PLL tempo follower (roadmap Phase 2 item 3)

- `firmware/daisy/src/sequencer/tempo_follower.hpp`: `TempoFollower` per
  `docs/features/midi-sync-tempo-follower.md` §3-4 - period estimator
  (median-of-5 + EMA, ±25% outlier rejection with 3-consecutive-consistent
  hard re-acquire for real tempo jumps), phase servo (proportional rate
  trim, clamped ±0.5%), and the Unlocked/Acquiring/Locked/Freewheel state
  machine. HAL-free, not yet wired to the UART message or the sequencer
  scheduler (protocol work is a separate follow-up stage).
- 22 new host tests (`tempo_follower_test.cpp`) covering the design doc's
  test plan: clean lock across several tempos, jittered-clock phase-error
  bounds, a simulated one-hour 50 ppm master-clock offset with no
  unbounded drift, step and gradual tempo changes, dropout/freewheel/
  resume, single-glitch rejection, and Start/Stop/Continue+SPP transport
  math.
- Fixed a real bug caught by an early version of this class's linked
  binary: `Tick()`'s rate-trim clamp used `std::min`/`std::max` against a
  `static constexpr` member, which odr-uses it and fails to link without
  an out-of-class definition in a header-only class - replaced with a
  manual clamp.

### Added — Sequencer scheduler core (roadmap Phase 2 item 1)

- `firmware/daisy/src/sequencer/pattern.hpp`: HAL-free pattern data model
  (`Pattern`/`Track`/`Step`/`ParamLock`/`TriggerEvent`, 96-internal-PPQN
  step scales including triplets) per `docs/features/sequencer.md` §3.
- `firmware/daisy/src/sequencer/sequencer_scheduler.hpp`: sample-accurate
  step scheduler (`SequencerScheduler`) - swing, per-step micro-timing,
  probability gating via a seeded xorshift64* RNG, retrig with correct
  next-step clipping, sorted `(frame, event)` output. Host-testable, not
  yet wired into the audio callback (engine wiring is separate follow-up
  work once the pattern-edit protocol and double-buffer discipline from
  `sequencer.md` §4 exist).
  - Anti-drift design note: frame timing is computed fresh per candidate
    event (`frame = tick * frames_per_tick`) rather than via an
    accumulated per-block phase delta - an accumulated Q32.32 fixed-point
    version was tried first and failed the 10-minute drift golden test at
    exactly 120 BPM (accumulated fixed-point rounding over 600,000
    control-tick calls flipped an exact-integer target tick onto the
    wrong side of a block boundary). The non-accumulating design is
    documented in the class header.
- 20 new host tests (`sequencer_scheduler_test.cpp`) covering the
  `sequencer.md` §6 test plan: the 10-minute/120 BPM drift golden test
  (beat 1200 at exactly frame 28,800,000), swing/micro-timing/retrig
  arithmetic, seeded-probability determinism, pattern looping, and
  event-sort ordering.
- Fixed a stale `firmware/daisy/CMakeLists.txt` entry (`src/sampler.hpp`)
  left over from the `Sampler` class's removal (2026-07-05, code review
  C2) - the file no longer exists and would fail a from-scratch Daisy
  configure.

### Added — Feature-expansion design suite (docs only, 2026-07-05)

- Nine design docs in `docs/features/` covering the E-mu Emax/Emulator-lineage
  feature set: instrument model (presets/zones/velocity layers + the shared
  WXCF chunk container), MIDI clock sync (PLL tempo follower), melodic
  sequencing/live record, param locks + mod matrix/LFOs, sampling & recording,
  arpeggiator, scenes/macros/param slew, tuning & scales, output routing &
  mixer. `feature-expansion-ideas.md` is the suite index and reserves
  inter-MCU message-ID blocks (0x50–0x5F, 0x60–0x6F, 0x70–0x7F, 0xA0–0xAF).
- `docs/roadmap.md`: new **Phase 2.5 — Sampler Instrument Layer** with its own
  gate; Phase 2/4/5 items now reference the new designs. Kit representation
  decision recorded: a kit is a drum-mode instrument (`KIT_OP` 0x54
  reserved-unused, subsumed by `MSG_INST_OP`).

### Added — Stage A paraphonic analog path, engine side (Phase 1 item 5)

- `ParaphonicEnvelope` (`firmware/daisy/src/audio/paraphonic_envelope.hpp`):
  the shared VCF/VCA envelope law from `analog-voice-board.md` §0 —
  retrigger on every note-on (from the drained note queue), release when
  the last held voice releases (`VoiceManager::HeldVoiceCount()`), at the
  1 kHz control tick. HAL-free, 6 new host tests.
- The control tick now stages CV values each millisecond (cutoff =
  base + envelope×depth through the exponential VCF shaping, resonance,
  envelope→VCA with SSI2164 inversion) via the CV group router;
  `AudioEngine::FlushCv()` performs the blocking MCP4728 fast-write from
  the main loop (§7.1.4), with an absent-hardware backoff (8 consecutive
  I2C failures disable the flush with one log).
- `MSG_CONTROL_CHANGE` has a real consumer again: cutoff base, resonance,
  and the shared-envelope ADSR map onto the paraphonic path
  (`PARAM_MODULATION_MATRIX` temporarily carries env→cutoff depth).
  `Flush()` returns the transaction result through the CV backend concept.
- CV calibration wire contract + Daisy workflow (item 5 stage 4): new
  messages `MSG_CV_CAL_SET/GET/RESP` (`CvCalMessage`, mirrors `CvCal`) and
  `MSG_CV_TEST` (steady CV override for the measurement procedure), with
  round-trip tests and `inter-mcu-protocol.md` rows. The Daisy applies cal
  to the CV backend (read-back via new `GroupCal()`), persists the full
  8-group table to SD (`0:/wavex_cvcal.bin`, magic+version, format
  documented in `cv_cal_store.hpp`), loads it at boot after SD mount, and
  the control tick honors the CV-test override. Dispatcher routes pinned
  by 4 new dispatch tests.
- CV Calibration UI page (item 5 stage 5, Settings → CV Calibration):
  per-control gain/offset/curvature editing applied live on the Daisy
  (every change sends `MSG_CV_CAL_SET`), CV test mode with Cut=0/Cut=1
  corner-measurement softkeys (steady CVs via `MSG_CV_TEST`, auto-disabled
  on page exit), Save-to-SD, and read-back of the stored table on entry
  (`MSG_CV_CAL_GET` → deferred `lv_timer` apply, never touching LVGL from
  the UART task). `UISettingsPage` internals opened up (`protected`) for
  purpose-built settings pages.
- Item 5 is code-complete; outstanding is bench verification only
  (CV-update-within-tick scope check, SSI2164 inversion, analog levels).


### Removed — inert legacy DSP surface (review C2)

- Deleted the Daisy audio engine's never-rendered mono-synth remnants: the
  SVF filter, ADSR, LFO, test oscillator, and the `AudioParameters` bank
  they shared — `Callback()` never processed any of them, so every
  `MSG_CONTROL_CHANGE` parameter and the "test oscillator fallback" for
  note-on had been silently doing nothing. `OnControlChange` remains as a
  routed, documented no-op hook until Phase 2 defines real parameter
  routing.
- Deleted the `Sampler` (`sampler.hpp` + its 18 host tests): inert
  end-to-end — nothing fed it input, nothing rendered its playback, and its
  record/play wire commands were dispatcher stubs. `OnSampleCtrl` remains a
  routed no-op hook (logs "recording not implemented"); recording will be
  rebuilt against the voice/streaming architecture when scheduled.
  `GetInputMeters` (which could only ever report zero) went with it.
- Deleted `OnSampleData`/`SampleLoadState`: the `MSG_SAMPLE_DATA`
  push-sample-over-the-link receiver was unreachable (its `loading` flag was
  never set). The message id stays reserved in `protocol.h`; the dispatcher
  logs and ignores it.

### Fixed — one shared WAV header parser, RIFF padding respected (review M12)

- The three hand-rolled RIFF chunk walks (`OpenWav`, `OnSampleLoad`,
  `ParseWavMetadata`) are replaced by one host-tested parser
  (`firmware/shared/wav/wav_header_parser.hpp`, 10 tests) templated over a
  Reader (FatFS adapter on device, `MemReader` for probes/tests). Real bug
  fixed: the two audio-engine copies skipped odd-sized chunks **without the
  RIFF pad byte**, so any WAV carrying an odd-length LIST/INFO chunk before
  `data` mis-parsed (wrong data offset or unsupported-format rejection).
  The walk is also bounded now (a corrupt size field can no longer loop or
  stall the stream), and `data`-before-`fmt` files are tolerated.

### Changed — shared UART frame scanner; TX failures retry (review §6.2/M6)

- Both links' RX byte-stream scanning (start-byte search, length/CRC
  validation, resync, overflow and no-start-byte drain policy) now lives in
  one shared, host-tested class (`firmware/shared/uart_protocol/
  frame_scanner.hpp`, 9 tests including the H2 wedge regression). The two
  hand-rolled copies had already diverged once with real consequences (H2
  existed only on the Daisy). New behavior for both sides: a start byte
  with an impossible length field resyncs immediately instead of stalling
  the stream until buffer overflow.
- Daisy TX (review M6): a failed `BlockingTransmit` now retries on every
  main-loop pass (bounded by a 1 s per-frame give-up) instead of
  head-blocking the queue for a second and dropping the frame without one
  retransmit attempt. The `s_tx_inflight` stuck-transmission machinery —
  built for an async-TX design that no longer exists — and the 30-line
  validate-our-own-frame block are deleted.

### Fixed — sample-memory stats and handle semantics (review M8)

- `SampleMemMgr` stats now cover both pools: `in_use_bytes`,
  `objects_alive`, and `failed_allocs` previously tracked only the small
  slab pool, so the UI's sample-memory page was blind to the dominant
  consumer — the samples themselves in the large extent pool.
- Removed the per-handle `refcnt`/`retain()`: handles are copied by value,
  so each copy counted independently — broken sharing semantics that
  nothing used. `release()` frees unconditionally and zeroes the handle
  (idempotent); zero-byte allocations are rejected (a `len==0` handle is
  the released sentinel). New host tests cover cross-pool stats,
  failed-alloc counting, and zero-byte rejection.

### Changed — preview pipeline bounded (review M7)

- `OnPreviewReq` no longer heap-allocates from wire-controlled values: the
  preview buffer is a fixed 4096-point static array, and an oversized
  start/end/decim request widens the decimation to fit (full selection
  stays visible, coarser) instead of reserving megabytes of newlib heap —
  which, with exceptions disabled, terminated the firmware on allocation
  failure. `SendPreviewChunks` stages frames in a static buffer instead of
  a fresh `std::vector` per chunk.

### Removed — dead protocol artifacts; browse path hardened (review H6/M10)

- Deleted `spi_protocol.h` — a third, competing wire framing (`pkt_t`, its
  own CRC, a barrier-free "lock-free" ring) with zero users; deleted the
  misleading `WaveXPacket` struct (placed `crc` at offset 4 where the wire
  puts it at the end) in favor of a layout comment; deleted
  `ParseBrowseReq`/`ParseSamplePlayReq` (parsed a browse-request format
  nothing sends — the live `[start_index u8][path][NUL]` format is pinned
  by the dispatch host test); `MAX_PAYLOAD_SIZE` moved out of `protocol.h`
  into its only user (`esp_spi_link.cpp`) with an SPI-revival warning that
  220 B cannot carry browse pages.
- Browse path (review H6): response staging buffers (~11 KB) and ListDir's
  ~14 KB directory page buffer moved off the shared main-loop stack into
  statics; the per-response entry clamp is now derived from the payload
  capacity (31) instead of a hard-coded 50 that would have overflowed the
  staging buffer at 32+ entries.

### Changed (Tooling/Repo hygiene, review §8)

- Untracked five stale committed binaries (`firmware/daisy/tests/lib/*.a`,
  `libwavex_test_lib.a` — obsolete since the vendored-source GoogleTest
  switch) and added `*.a` to `.gitignore`; removed `protocol.o` and an empty
  `node_modules/` from the repo root.
- `scripts/graphify-refresh.sh` works again with graphify 0.8.x (`update`
  no longer takes `--no-viz`), and `.graphifyignore` now excludes vendored
  code (libDaisy, DaisySP, managed components, vendored GoogleTest) so the
  graph is first-party signal (~7k nodes) instead of 64k mostly-vendor
  nodes. Pre-commit format hooks also exclude `firmware/**/_deps/`.

### Changed — hot-path logging gated (review M5/M11)

- Daisy no longer logs unconditionally on hot paths: the per-received-frame
  `PrintLine`, the dispatcher's per-message header + hex-dump ladder, the
  per-note NOTE_ON/OFF logs, per-TX and per-preview-chunk traces, and the
  1 Hz link-status lines are now compile-gated behind
  `WAVEX_MCU_LINK_DEBUG` / `WAVEX_MCU_LINK_PACKET_DEBUG` /
  `WAVEX_DAISY_SD_DEBUG` (all default 0). Error paths and one-shot boot/
  user-action logs remain. `fs_browse.cpp`'s seven tracing `printf`s and
  the 22-step `SAMPLE_LOAD: [n/10]` scaffolding are deleted. ESP32
  per-packet/heartbeat/browse-timing logs demoted from INFO to DEBUG.
- Deleted `main.cpp`'s dead CPU-measurement scaffold (review M11): eight
  state variables plus a 100 ms boot busy-loop baseline whose results
  nothing read (superseded by `CpuLoadMeter`), the unused
  `wav_path`/`last_sync`/`last_tx_pump`/`busy_start_ticks` locals, the
  duplicate 1 Hz stats logger with its bare `printf`, and a boot log line
  claiming a hardcoded STM32H7B3 revision on an H750.

### Added — UART link sequence protection (review C4/M4)

- Both live UART links (`daisy_uart_link.cpp`, `esp_uart_link.cpp`) now run
  every received frame through the shared reboot-aware `SequenceTracker`
  (previously wired only into the compiled-out SPI path): duplicate and
  severe out-of-order frames are dropped (counted in link stats as
  `seq_drops`), and a peer reboot (low, fresh-looking sequence number after
  real progress) resyncs and continues (`seq_resyncs`) instead of being
  double-processed or wedging.

### Fixed

- **MIDI notes now actually reach the voice manager** (code review C1): the
  Daisy message dispatcher's `MSG_NOTE_ON`/`MSG_NOTE_OFF`/
  `MSG_CONTROL_CHANGE`/`MSG_SAMPLE_CTRL` handlers were log-only stubs, so
  the entire item-8 MIDI path (ESP32 DIN/USB in → UART link → SPSC note
  queue → `VoiceManager`) was unreachable from the wire despite being
  implemented and unit-tested. The four handlers now validate payload size
  and dispatch to the audio engine; the dead `audio_adapter.{h,cpp}` (zero
  callers) was deleted. A new dispatch-level host suite
  (`firmware/daisy/tests/unit/comm/message_dispatch_test.cpp`, 12 tests)
  drives the real dispatcher against recording mocks and pins every routed
  message type to its observable subsystem call — the test class that would
  have caught this. Bench verification (in-to-sound latency) still pending.
- Daisy no longer requires a USB serial terminal to boot (code review H8):
  `StartLog(true)` put libDaisy's logger in synchronous mode — an unbounded
  busy-wait on every `PrintLine` until a host accepts the transfer, so a
  standalone unit hung inside `StartLog` before the audio engine started.
  Boot logging is now gated by `WAVEX_DAISY_WAIT_FOR_SERIAL`
  (hardware_config.h, default 0 = boot standalone, drop early logs; set 1
  on the bench to keep the old wait-for-terminal behavior).
- Reclaimed ~254 KB of Daisy internal RAM (code review H1): each
  `SlabPage` bitmap in the sample-memory manager was sized at one *word*
  per possible slot instead of one *bit* (512 B where 16 B suffices — ~190
  KB of BSS across 6 classes × 64 pages of bookkeeping), and
  `audio_engine.cpp` carried a 64 KB `s_conversion_buffer` unreferenced
  since the scratch-pool refactor. New `SampleMemTest` host suite (6 tests)
  pins the slot bookkeeping the bitmap drives (full-page fill of the
  smallest class, slot distinctness, release/reuse, extent coalescing).
- `CreateWaveXPacket` rejects payloads of 2043–2048 bytes (code review H4):
  `GetOptimalSizeCode` saturates to the 2048-byte class, whose real payload
  capacity is 2042 (header + CRC overhead), so those six sizes previously
  memcpy'd past the caller's buffer and underflowed the zero-pad `memset`
  length into a wild multi-GB write. Boundary host tests added (2042 OK,
  2043–2048 rejected).
- `ParseUartPacket` takes the destination capacity in/out (code review H5):
  the copy length previously came entirely from the wire — the same overflow
  class fixed earlier in `ParseWaveXPacket`. A frame whose payload exceeds
  the caller's buffer is now rejected outright (a truncated UART message is
  never valid). Both link call sites updated; host tests added.
- ESP32 `PacketRouter` validates payload length before copying typed
  messages (code review H3): a CRC-valid frame with an empty payload
  previously reached `memcpy(&msg, nullptr, sizeof)` (undefined behavior),
  and truncated payloads filled message tails with stale stack bytes. All
  eight fixed-size message cases now go through one checked helper; new
  host tests drive every type with null and truncated payloads.
- Daisy UART RX can no longer wedge permanently on a frame buffer full of
  start-byte-free garbage (code review H2): the no-start-byte path now
  discards the scanned window (keeping the last frame-overhead-minus-one
  bytes), mirroring the guard the ESP32 side already had. Previously the
  buffer never drained, new bytes were discarded on arrival, and none of
  the existing recovery paths could trigger.
- ESP32 `inter_mcu_send_*` wrappers no longer invert their error result
  (code review C3): `uart_link_send` returns -1 on failure, and the old
  `return result ? ESP_OK : -1` mapped that truthy -1 to **ESP_OK**, so
  every link-send failure (queue full, link down) was reported as success
  and all upstream error handling — including midi_task's "note dropped"
  warnings and the UI pages' send-failure paths — was unreachable. Now
  `result >= 0 → ESP_OK`, else `ESP_FAIL`.
- Pre-commit's five repo-local hooks (ESP32/Daisy build checks and all three
  test suites) never actually ran: their `files:` regexes
  (`^(firmware/esp32/|Makefile)$` etc.) matched only the literal directory
  string, never files under it, so every hook silently skipped on every
  commit. Patterns now match directory contents, and shared-code changes
  (`firmware/shared/`) trigger both build checks and both MCU test suites,
  since shared sources are compiled into all of them.
- Sequence-number generators (shared `CreatePacket` and both UART links) now
  skip the reserved value 0 when the 16-bit counter wraps; previously one
  packet per 65,535 would carry seq 0, which receivers reject. Host tests
  cover the wrap on both the generator and tracker sides.

### Changed (Docs)

- `docs/code_review_20260705.md`: comprehensive code review of the full
  first-party tree (findings C1–C4, H1–H8, M1–M13 + smells inventory,
  prioritized P0–P3 action list, doc-drift appendix).
- `docs/architecture.md` corrected to match transport reality (review C4):
  **UART (UART1↔UART4 @ 2 Mbaud) is the transport of record and carries all
  inter-MCU traffic**; the SPI link is wired but compiled out
  (`WAVEX_SPI_LINK_ENABLED=0`) and its revival requires bench re-validation.
  Also refreshed stale "as-built" claims: voice manager is implemented and
  callback-wired (but unreachable from the wire until review C1 is fixed),
  sampler storage fix vs. inert sampler, partition-table rework done,
  event-dispatch consolidation status. `docs/roadmap.md`: corrected 0.2.1's
  "SPI carries browse/wave" premise, marked Phase 1 item 6 blocked on SPI
  re-enablement, and corrected item 8's "Done" claim (dispatcher hop missing).

### Changed (Tooling)

- Graphify analysis is now scoped to source directories and root-level build
  metadata, and the refresh helper no longer generates a submodule ignore list.

### Added — MIDI note path (Phase 1 item 8)

- Shared serial-MIDI byte-stream parser
  (`firmware/shared/midi/midi_stream_parser.hpp`) — HAL-free/header-only,
  handles running status, real-time interleave (including inside SysEx),
  SysEx skipping, system-common alignment, orphan-data discard, and
  velocity-0 NoteOn → NoteOff normalization. 16 host tests
  (`tests/midi/midi_stream_parser_test.cpp`).
- Daisy: `MSG_NOTE_ON/OFF` now plays loaded samples through the 8-voice
  `VoiceManager`, finally wired into the audio callback. Note events are
  resolved to trigger params in the main loop and handed to the callback
  through an SPSC ring (release/acquire, one-block worst-case latency);
  mapping policy: most-recently-loaded 16-bit sample, root note 60,
  velocity → gain, stored sample rate → pitch compensation on the 48 kHz
  engine. Falls back to the test oscillator when nothing playable is
  loaded. `VoiceManager` gained interleaved-stereo source support
  (averaged to mono pre-pan) and `StopAll()`; `OnSampleLoad` now
  hard-stops voices before releasing/rewriting sample memory. 3 new host
  tests (76 total Daisy-side).
- ESP32: DIN MIDI input task (`main/midi_task.cpp`) — UART2 @ 31250 baud
  (pins in `pin_config.h`), per-byte RX delivery (RX-full threshold 1,
  1-symbol idle timeout) for the < 5 ms latency budget, shared parser,
  notes forwarded as `MSG_NOTE_ON/OFF` over the inter-MCU link. Gated by
  new `WAVEX_ESP_DIN_MIDI_ENABLED` (default on); MIDI init failure is
  non-fatal. CC events are parsed but not yet forwarded (CC→parameter
  mapping is Phase 2).
- ESP32: USB MIDI device (`main/usb_midi_task.cpp`) — the P4's native
  USB-OTG port now enumerates as a class-compliant USB MIDI device
  ("WaveX Sampler") via new `esp_tinyusb` (^2.0) dependency with
  `CONFIG_TINYUSB_MIDI_COUNT=1`. Callback-driven RX (no polling): TinyUSB's
  `tud_midi_rx_cb` notifies a reader task that drains the class driver's
  byte stream through the same shared parser and note forwarding as DIN.
  Gated by the existing `WAVEX_ESP_USB_MIDI_ENABLED` /
  `WAVEX_USB_MIDI_INPUT_ENABLED` flags. This completes Phase 1 item 8
  (code-complete; in-to-sound latency measurement needs bench hardware).

### Fixed — DMA/timing review Findings 4–12 (medium/low batch)

- **F4**: `UartLinkSend` no longer disables all interrupts (audio included)
  while building the frame — the ~60–130 µs CRC-under-IRQ-lock guarded no
  actual concurrency (producer and consumer both main-loop-only; invariant
  now documented in code).
- **F5**: preview wave chunks no longer silently dropped past the 4-deep TX
  queue — new `UartLinkPumpTx()` (TX-only, safe from message-handler
  context) drains a frame and the same chunk retries; a pump budget bounds
  the added main-loop blocking, and a genuine stall aborts loudly instead
  of punching silent mid-stream gaps.
- **F7**: the excessive-CRC-error DMA-listener reset is now measured over a
  real 1-second window instead of per-main-loop-iteration (which its own
  log claimed was "per sec" but made slow-drip corruption unable to ever
  trigger recovery).
- **F8**: the UART TX queue moved out of `DMA_BUFFER_MEM_SECTION` (it's
  CPU-polled `BlockingTransmit`, never DMA) — measured RAM_D2_DMA drop:
  28204 B (86.07%) → 19932 B (60.83%), reclaiming ~8.3 KB of the scarcest
  region for future DMA TX / SPI buffers.
- **F9 (ESP32)**: `uart_task` now drains the driver ring fully on every
  data event *and* in the idle branch — leftover bytes generate no new
  UART_DATA event until more data arrives, so frame tails could previously
  sit unread indefinitely. `uart_wait_tx_done`'s result is checked and its
  timeout sized above a max frame's wire time.
- **F10 (ESP32)**: driver rings resized — RX 2 KB → 8 KB (~10 ms → ~40 ms of
  margin at 2 Mbaud), TX 2 KB → 4 KB (a max 2058 B frame now fits entirely,
  so `uart_write_bytes` returns without blocking on wire drain);
  `s_rx_pending` grown to match.
- **F11 (ESP32)**: the `ui_update_pending` cross-task handoff
  (UART task → UI task) is now a release-store/acquire-load atomic pair —
  a plain bool gave no ordering guarantee on the dual-core P4 that
  `entries[]` writes were visible before the flag.
- **F12**: architecture.md §7.1.2 now documents the load-time SD→SDRAM
  direct-DMA exception instead of silently contradicting the code.

### Fixed (Daisy link) — DMA/timing review Finding 2

- **Frames ≥ ~1990 bytes are no longer deterministically un-sendable.** The
  UART blocking-transmit timeout was a fixed 10 ms, but a max frame (2058
  bytes) at 2 Mbaud needs 10.29 ms of wire time — so near-max frames always
  timed out mid-frame, sprayed a truncated frame at the peer (CRC/sync
  storm), and retried the whole frame every main-loop pass (~10 ms blocked
  each) until the 1-second stuck-TX force-clear dropped the message,
  starving the audio pump throughout. The timeout is now derived from the
  frame's own wire time plus margin (`UartTxTimeoutMs()` in
  `uart_protocol.h`, host-tested — including a regression test documenting
  that the max frame's wire time really did exceed the old fixed timeout).
  Documented tradeoff: a max-size frame now legitimately blocks ~13 ms,
  slightly over §7.1.4's ~10 ms guideline, until TX moves to DMA; typical
  traffic stays well under 10 ms.

### Fixed (Daisy link) — DMA/timing review Finding 3

- **UART interrupts no longer preempt audio.** libDaisy hardcodes UART4_IRQn
  (`HAL_UART_MspInit`) and DMA1_Stream5 / UART4 RX DMA (`dsy_dma_init`) to
  NVIC priority (0,0) — the maximum, above the audio SAI DMA at 5 — letting
  the UART RX callback's multi-KB memmoves preempt the audio callback,
  inverting architecture.md §7.1.5's "audio highest" rule. `main.cpp` now
  re-sets both to priority 7 (below audio 5/6, above SPI 10) immediately
  after `UartLinkStart()`. Compile-verified; the jitter improvement needs
  DWT measurement on hardware.

### Changed (CI)

- GitHub Actions now runs inside `espressif/idf:release-v5.5` — the same
  base image as `.devcontainer/Dockerfile` — instead of git-cloning ESP-IDF
  v5.2 from scratch every run. This fixes the toolchain-version mismatch
  with dev (5.5 vs 5.2), removes ~10 minutes of per-run IDF install, and
  makes the Daisy toolchain identical too (the image's
  `gcc-arm-none-eabi` is 13.2.1, matching dev). The stale build-directory
  caches (which risked restoring stale absolute paths) were dropped along
  with the separate IDF/ARM-GCC install steps. The full new CI recipe —
  fresh recursive clone through both firmware builds and all host tests —
  was dry-run locally in the exact container image before landing.
  Cosmetic: `make esp32`'s banner no longer claims "ESP32-S3 / ESP-IDF
  v5.2" (it's P4 / release-v5.5).

### Changed (submodules)

- **libDaisy submodule re-pinned to the public upstream v8.1.0 tag**
  (`9498417a`), and its nested `Drivers/STM32H7xx_HAL_Driver` to the
  upstream commit v8.1.0 ships with (`404a70d`). The local-only patch
  commits (44.1 kHz SAI support, ADC HAL sources disabled, `stdint.h`
  include fix) are now fully obsolete: the 48 kHz switch removed the only
  consumer of the SAI patch, and a clean-from-scratch build against pure
  upstream confirms the ADC/stdint build fixes are no longer needed (the
  CMSIS-include fix in `firmware/daisy/CMakeLists.txt` covers it). This
  unblocks pushing the repo: fresh clones and CI (`submodules: recursive`
  checkout) can now fetch every pinned submodule commit from its public
  upstream. The old patches remain available locally on the
  `wavex-v8.1.0-with-local-patches` branch in the submodule clone if ever
  needed for reference.

### Changed (Daisy audio) — needs hardware listen test

- **Engine sample rate: 44.1 kHz → 48 kHz** (decision 2026-07-03,
  `docs/dma-timing-review-2026-07-03.md` Finding 1). Restores the
  1-block = 1-ms control-tick invariant (48-sample blocks at 48 kHz), which
  44.1 kHz had silently broken — the "1 kHz" tick was running at 918.75 Hz,
  and a Phase 2 sequencer built on it would have run ~110 BPM at a setting
  of 120. `timebase.hpp` now `static_assert`s the integer-ms invariant so
  this can't silently regress again. 44.1 kHz WAV content is rate-converted
  at playback: streaming/audition through `PumpWavIO`'s existing resampler
  (made trustworthy by the Finding-6 fix in the previous commit, since
  resampling is now the *normal* path for 44.1k files), and RAM-resident
  samples via new playback-rate compensation —
  `VoiceTriggerParams::sample_rate_hz` scales `Voice::increment` by
  native/engine rate (0.91875 for 44.1k on 48k), composing multiplicatively
  with note pitch; 3 new host tests. Resample-on-load stays a later option
  for Phase 4 uniformity. **Verify on hardware**: audition one 44.1 kHz and
  one 48 kHz WAV and confirm correct pitch on both.

### Added

- **DMA/timing code review** (`docs/dma-timing-review-2026-07-03.md`): 12
  findings across the UART link and audio path, three high-severity — the
  control tick actually runs at 918.75 Hz not 1 kHz (engine is at 44.1 kHz
  with 48-sample blocks, violating architecture.md §5.1's 1-block=1-ms
  invariant; **decision recorded: return to 48 kHz**, with 44.1 kHz WAVs
  handled by the existing streaming resampler plus playback-rate
  compensation in `VoiceManager`); Daisy→ESP32 frames ≥ ~1990 bytes
  deterministically exceed the 10 ms `BlockingTransmit` timeout at 2 Mbaud
  and can never transmit (retry storm starves audio ~1 s per attempt-cycle);
  and libDaisy leaves UART4 + its RX DMA stream at NVIC priority 0, above
  audio's 5 (priority inversion vs §7.1.5). Fix sequencing added to the
  front of `roadmap.md`'s Phase 1 next steps; no code changed in this
  commit.

- **Link robustness regression tests** (roadmap Phase 1 item 7): investigation
  found the disabled SPI path's sequence-number validation
  (`is_duplicate_packet`, byte-for-byte duplicated between
  `daisy_spi_link.cpp`/`esp_spi_link.cpp`) would have permanently wedged on
  a real peer reboot — a sender resetting its sequence counter to 1 gets
  classified "out-of-order" against the receiver's still-high expectation
  forever, with no recovery path. Extracted into a single shared
  `firmware/shared/spi_protocol/sequence_tracker.hpp` (`SequenceTracker`)
  that detects that specific reboot signature (a low, fresh-looking
  sequence number arriving far below an already-advanced expectation) and
  resyncs instead of wedging, replacing both duplicated copies. Added
  `firmware/shared/spi_protocol/attn_watchdog.hpp` (`AttnWatchdog`): the
  ESP32-side ATTN-assertion code had **no timeout at all** for "asserted
  ATTN, transaction never completed" — a wedged Daisy left ATTN stuck high
  forever with no recovery; now force-deasserted after 500ms, mirroring the
  UART fix's threshold. Both are HAL-free/host-tested (12 new tests) since
  real GPIO/SPI electrical timing isn't testable without hardware — see the
  roadmap entry for exactly what is and isn't covered.

- Synced versioning: a single root `VERSION` file now drives both the ESP32
  (`PROJECT_VER`) and Daisy (`project(... VERSION ...)`) firmware builds.
- `AGENTS.md` / `CLAUDE.md` project instructions.
- **Output/CV backend seam** (roadmap Phase 1 item 1, architecture.md §5.3):
  `WAVEX_VOICE_OUTPUT_BACKEND`/`WAVEX_CV_BACKEND`/`WAVEX_ANALOG_CV_GROUPS`
  flags (`hardware_config.h`); `StereoMixSink` (real, sums per-voice buffers
  into the SAI1 stereo stream) and `TdmVoiceSink` (compiling stub for the
  Phase 3 PCM1690/SAI2 bring-up) in
  `firmware/daisy/src/audio/output_sink.hpp`; a template-based
  `CvGroupRouter` (`firmware/daisy/src/cv/cv_group_router.hpp`, zero vtable
  overhead) that folds voice-indexed CV targets onto
  `WAVEX_ANALOG_CV_GROUPS` physical groups, backed by `Mcp4728Backend` (real
  I2C, Stage A) or `Mcp48Backend` (compiling stub, Stage B). New CMake cache
  vars in `firmware/daisy/CMakeLists.txt` plus a `make daisy-stageb` target
  and CI step prove both flag-set combinations compile. 10 new host tests
  for the router's voice-to-group folding and the sinks' mixing logic (both
  are HAL-free and run without any Daisy hardware).
  **Not wired into the audio callback yet** — today's engine has exactly one
  playback source (the WAV ring buffer / `Sampler`), not an array of
  per-voice buffers, so there's nothing for the sink/router to consume until
  the voice manager (Phase 1 item 2) exists. The paraphonic envelope law
  (which voice's envelope drives the shared Stage A VCF/VCA) is Phase 1 item
  5's job, not this seam's.
- **Voice manager, RAM-resident half** (roadmap Phase 1 item 2):
  `firmware/daisy/src/audio/voice_manager.hpp` — `VoiceManager`, 8-voice
  array, allocation with oldest-triggered stealing when all 8 are busy,
  per-voice gain (from MIDI velocity) and linear pan, a pitch hook
  (`Voice::increment`, hardcoded to 1.0 - note-to-pitch mapping is item 4's
  job), zero-I/O triggering (`Trigger()` just stores a pointer into
  already-SDRAM-resident sample data - no allocation, no blocking), and
  `Render()` into the stereo buffer shape `output_sink.hpp` (item 1)
  consumes. HAL-free like the CV router, so it's host-testable without any
  Daisy hardware; 12 new tests cover allocation, stealing, release-by-note,
  gain/pan scaling, self-stop at sample end (no looping yet), and rejecting
  null/too-short samples. Constructed in `audio_engine.cpp` (compiles for
  the real ARM target - a first pass at this forgot to actually `#include`
  it into any compiled translation unit, so the ARM build silently never
  checked it; caught before committing) but **not wired into `Callback()`**
  — `OnNoteOn`/`OnNoteOff` still only drive the test oscillator, since
  there's no note-to-sample mapping policy yet (item 8's job).
  **Streamed-voice concurrency (2 concurrent streams + prebuffer admission
  control), the other half of item 2's roadmap text, is not started** — the
  existing WAV-streaming path is a separate, still-singleton subsystem; see
  `docs/roadmap.md` item 2 for why it's scoped as its own follow-up rather
  than bundled into this change.
- **Per-voice digital processing** (roadmap Phase 1 item 4): extends the
  voice manager above with the pieces it was deliberately missing —
  `Voice::increment` is now a real 12-TET pitch ratio (`note` relative to a
  per-trigger `root_note`, octave up/down verified exactly); start/end/loop
  playback region (`Voice::start_frame/end_frame/loop/loop_start/loop_end`
  — a looping voice now keeps playing indefinitely instead of self-stopping
  at the sample's natural end); a one-pole lowpass filter stand-in for the
  analog VCF (`firmware/daisy/src/audio/one_pole_filter.hpp`, picked over
  SVF for simplicity — no per-voice resonance state needed for "a
  stand-in"); and a linear ADSR envelope
  (`firmware/daisy/src/audio/envelope.hpp` — deliberately not DaisySP's
  `Adsr`, which isn't linked into the host test libraries). `Release()` now
  starts the envelope's release phase instead of hard-stopping the voice —
  a voice stays allocated and rendering through its release tail, matching
  real synth behavior; this is a real, deliberate behavior change from the
  item-2 version. Voice-stealing now prefers a voice already releasing over
  an older sustaining one (the follow-up the item-2 code comment predicted).
  `Trigger()`'s signature changed to a `VoiceTriggerParams` struct (named
  fields) since the old 5-positional-argument form doesn't scale to this
  many parameters. **Did not adopt `arm_linear_interp_q15`** despite the
  roadmap citing it — that's a profiling-driven ARM-only optimization
  (needs the DWT cycle counter on real hardware) that would cost this
  class's host-testability; the roadmap text reads as "(exists) for later
  use," not a mandate for this pass. 10 new host tests (22 total).
  Still not wired into `Callback()`, same reasoning as item 2.

### Fixed (Daisy SPI link) — roadmap Phase 1 item 7

- **`daisy_spi_link.cpp`'s `extern daisy::DaisySeed* s_hw;` was declared at
  file/global scope**, before any `namespace WaveX::Comm` block in the file
  opens — the same namespace-scoping bug class as the UART `PacketRouter`
  injection fix (Phase 0.2, `c3c7967`). The real `s_hw` is
  `WaveX::Comm::s_hw`, defined in `daisy_uart_link.cpp`; the mismatched
  extern declared a different, never-defined `::s_hw`. Harmless while
  `WAVEX_SPI_LINK_ENABLED=0` wraps this entire file out of the build (the
  default), which is exactly why nobody had noticed: this dead code hadn't
  actually compile-checked, let alone linked, in some time. Found and fixed
  while verifying the sequence-resync/ATTN-watchdog changes below actually
  compile — temporarily flipped the flag to 1, confirmed the link error, hit
  this bug, fixed it, confirmed a clean build+link on both MCUs, then
  reverted the flag (SPI stays disabled; re-enabling it for real is roadmap
  Phase 1 item 6, blocked on oscilloscope access to verify the raised clock).

### Fixed (Daisy audio) — roadmap Phase 1 item 3

- **`Sampler`'s recording buffer no longer grows via `std::vector::push_back`
  in the audio path.** `Sampler::Init()` now takes a `SampleMemMgr&` and a
  frame-count capacity, and preallocates one fixed-size extent from it up
  front (`audio_engine.cpp` requests 30 seconds at the current sample rate,
  right after `SampleMemMgr::init()` — reordered so the manager exists
  before `Sampler` allocates from it). `FeedInputBlock()` now writes within
  that fixed capacity and silently stops once full, instead of triggering a
  heap reallocation on every sample past the old 1024-sample
  `reserve()` (AGENTS.md constraint #1: no heap allocation in the audio
  path). **Found in the process**: `FeedInputBlock()` was never actually
  called from `Callback()` in production (only from tests) — recording is
  wired up via `OnSampleCtrl`'s `StartRec`/`StopRec`, but `Callback()`
  discards its input buffer (`(void)in;`) and never feeds it to the
  sampler, so recording currently produces silence regardless of this fix.
  Making the storage real-time-safe doesn't make recording functional;
  actually wiring live input into `FeedInputBlock()` is separate, deferred
  work (same reasoning as items 1/2 - no hardware here to verify audio-input
  correctness).

- `firmware/daisy/src/cv_bus.hpp` (the `CvBus` class) — superseded by the CV
  backend seam above. Its `Flush()` had a real bug: hardcoded to always
  flush DAC slot 0 regardless of which voice/group was queued (harmless in
  practice only because nothing ever called `QueueVoice`/`Flush` on it —
  `s_cv.Init()` was the only call site anywhere in the tree). The same
  calibration math and MCP4728 fast-write protocol now live correctly in
  `Mcp4728Backend`.

### Changed (ESP32 UI) — needs hardware verification

- Upgraded LVGL 9.3.0 → 9.5.0 and `esp_lvgl_port` to 2.8.0 (roadmap Phase
  0.1): pinned `lvgl/lvgl: '>=9.4,<10'` in `idf_component.yml` so the
  component manager can't silently resolve back down. Enabled
  `CONFIG_LVGL_PORT_ENABLE_PPA=y` (in both `sdkconfig` and
  `sdkconfig.defaults`) to offload display rotation to the ESP32-P4's PPA
  hardware instead of software. **Important correction to the original
  roadmap risk assessment**: our UI does use display rotation
  (`display_manager.cpp` sets `LV_DISPLAY_ROTATION_90`) — the roadmap had
  assumed otherwise when calling this low-risk, and `LVGL_PORT_ENABLE_PPA`'s
  Kconfig help text is literally "Enable PPA for screen rotation," so this
  is likely the correct fix but also puts us squarely in the "known PPA
  rotation-mode bugs on P4" risk category the roadmap flagged. **Only
  compile/build-verified so far** (full ESP32 build green in the
  devcontainer) — this needs to be flashed to the real HX8394/MIPI-DSI panel
  and checked for tearing/corruption during rotation before being trusted,
  and the FPS/CPU before-after profiling the roadmap called for on the
  waveform-preview and meter pages hasn't been done. Flip
  `CONFIG_LVGL_PORT_ENABLE_PPA` back to unset in both sdkconfig files if it
  misbehaves on hardware.

### Fixed (ESP32 dispatch)

- **UART `PacketRouter` injection was silently broken** (roadmap Phase 0.2
  item 4): `application_context.cpp` had the injection call commented out
  with "function not linking properly." Root cause was a namespace-scoping
  bug — the hand-rolled `extern` declaration was lexically inside
  `namespace WaveX`, so it declared (and looked up) `WaveX::
  uart_link_set_packet_router` instead of the real global-scope function
  defined in `esp_uart_link.cpp`. Fixed by including the real header and
  qualifying the call with `::`. Production UART traffic now routes through
  `ApplicationContext`'s single owned `PacketRouter` instead of a throwaway
  `dummy_router` fallback instance in `esp_uart_link.cpp`.

### Removed (ESP32 dispatch consolidation)

- **One event-dispatch owner on ESP32** (roadmap Phase 0.2 item 4):
  `PacketRouter` now owns fan-out; `inter_mcu` is the thin facade over
  `StatisticsManager` it was meant to be. Deleted: `ListenersManager`
  (`comm/listeners.h/.cpp`, wholesale dead — never instantiated anywhere);
  `inter_mcu_set_meter_listener`/`_set_browse_resp_listener` (dead redundant
  entry points — the live registration path is `CommInterfaceImpl` →
  `StatisticsManager` directly, bypassing these); the `s_sample_status_listener`
  static and its always-unreachable fallback branch in
  `inter_mcu_invoke_sample_status_callback` (`s_statistics` always wins in
  production); two dead duplicate SPI dispatch functions in `esp_spi_link.cpp`
  (`handle_control_message_from_daisy`, `_new_format`, zero callers) and the
  hand-rolled byte-unpacking `inter_mcu_process_daisy_control_message` they
  alone called (duplicated what `PacketRouter::handle_meter_push`/
  `handle_heartbeat` already do with typed structs). Left alone:
  `inter_mcu_set_wave_chunk_listener`/`_set_sample_status_listener` — both are
  genuinely live, used by production UI code, and `ICommInterface` has no
  wave-chunk equivalent to consolidate onto yet.

### Changed

- Upgraded `libDaisy` submodule v8.0.0 → v8.1.0 (roadmap Phase 0.1). Pulls in
  the `volatile`/error-flag fix for the SD DMA wait-loop compiler-optimization
  hazard and `HAL_SD_ErrorCallback` wiring, plus a reworked `WavPlayer` (unused
  by our custom streaming path). Local build-fix patches (44.1kHz SAI support,
  ADC HAL sources disabled, missing `stdint.h` include in the nested HAL
  driver) were rebased onto the new tag with no conflicts. `make daisy` and
  `make test` (79/79) pass; on-hardware SD soak test still needed before
  calling the Phase 0 gate closed.

- ESP32 partition table now uses the full 16 MB flash (roadmap Phase 0.2):
  factory image + two 4 MB OTA app slots with `otadata`, `userdata` grown from
  192 KB to ~3.9 MB, and the vestigial `samples` partition removed (samples
  live on the Daisy's SD card; no code referenced the partition). The app
  offset moved 0x10000 → 0x20000 — reflash via `flash-esp32.sh`/`idf.py
  flash` as usual; NVS offset/size are unchanged so stored settings survive.

### Removed

- Dead legacy SPI-based SD card backend: `sd_spi.cpp`/`sd_spi.h`/
  `diskio_sd_spi.cpp` (roadmap Phase 0.2). `WAVEX_DAISY_SD_CARD_BACKEND`
  defaults to `1` (SDIO) and nothing in the tree set it to `0`, so this path
  was unreachable. `esp_uart_link`/`daisy_uart_link` were left in place —
  despite the roadmap calling them legacy, they're the live transport for
  heartbeat/meter/status/ACK messages alongside SPI; `docs/roadmap.md` has
  been corrected.

### Fixed

- Test-build brittleness (roadmap Phase 0.2): GoogleTest is now a single
  vendored submodule (`firmware/shared/tests/_deps/googletest-src`, pinned
  `release-1.12.1`) referenced by all three test suites via
  `FetchContent_Declare(... SOURCE_DIR ...)`, so `make test` no longer needs
  network access. Removed a malformed duplicate nested submodule entry
  (`.../tests/_deps/firmware/daisy/tests/_deps/googletest-src`) left over from
  a `git submodule add` run in the wrong directory. `make test-daisy`/
  `test-esp32`/`test-shared` now wipe their build dirs before each run instead
  of reusing stale ones.
- The top-level `Makefile` and `firmware/daisy/Makefile` — the build
  entrypoints documented in `AGENTS.md` — were never in git: `.gitignore`'s
  bare `Makefile` pattern (meant for CMake-generated makefiles) was swallowing
  them. Added negation exceptions and tracked both files.
- **`ProtocolHandler::ParseWaveXPacket` buffer overflow** (roadmap Phase 0.2,
  found while writing exhaustive round-trip tests): the function ignored the
  caller-supplied destination capacity and always copied the packet's full
  zero-padded payload region, which is rounded up to the next size class (32/
  64/128/...). Any message struct smaller than its packet's padded region
  (e.g. `ErrorMessage`, `SampleLoadMessage`, `SampleMemStatusMessage`,
  `BrowseRespMessage`'s entries buffer) could be overrun by the extra padding
  bytes — reproducible as `*** stack smashing detected ***` once round-trip
  tests actually parsed those types back. Also fixed the one real firmware
  caller (`firmware/esp32/main/comm/packet_router.cpp`), which read the
  `payload_size` in/out parameter uninitialized. `payload_size` is now
  correctly treated as an in/out capacity: at most that many bytes are
  copied, and the true byte count copied is returned.

### Changed (protocol)

- **Wire-struct hygiene** (roadmap Phase 0.2): every message struct in
  `firmware/shared/spi_protocol/protocol.h` now has a zero-initializing
  default constructor and a named-argument constructor, and no other
  constructors — this makes each type a non-aggregate, so
  `Type x = {a, b, c};` / designated-initializer construction is now a
  **compile error** instead of a style guideline (`docs/features/inter-mcu-
  protocol.md` already warned about field-order bugs from aggregate init;
  now the compiler enforces it). All ~40 call sites across app and test code
  were migrated to `Type x(a, b, c);`. `SampleMemStatusMessage` gained a
  bounds-checked `AddEntry()` for its fixed `entries[]` array.
- Round-trip tests in `firmware/shared/tests/protocol/message_types_test.cpp`
  are now exhaustive: every message type is both created and parsed back
  with field-level assertions (several were previously create-only), and the
  four types with no coverage at all (`DataRequestMessage`,
  `StatusRequestMessage`, `SampleLoadMessage`, `SampleMemStatusMessage`/
  `SampleMemEntryMessage`) now have tests. This is what surfaced the
  `ParseWaveXPacket` overflow above.

## [0.1.0] - 2026-07-02

### Added

- Initial versioned baseline. Dual-MCU sampler/groovebox: ESP32-P4 frontend
  (LVGL touchscreen UI, encoders, MIDI I/O) and Daisy Seed backend (real-time
  audio engine, SD sample streaming, CV outputs), linked over SPI with a
  shared wire protocol.

[Unreleased]: https://github.com/maxamplitude/WaveX/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/maxamplitude/WaveX/releases/tag/v0.1.0
