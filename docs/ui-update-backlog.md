# `ui-update` branch backlog

Findings from the first hardware pass over the ported UI (August 2026), plus
the design work still outstanding. Kept separate from `docs/backlog.md`, which
holds longer-lived items; everything here belongs to the `ui-update` branch and
should either land or move before it merges.

Each item records what was actually verified, so nothing gets "fixed" on a
guess. Three of the bugs below were traced to a specific line before this
document was written; the rest are marked as unconfirmed.

---

## B1. Diagnostics tabs show no data unless reached by softkey — **FIXED**

**Reported:** Audio, Link and Storage show empty cards even while a sample is
streaming from SD and playing.

**Cause:** `UIDiagnosticsPage::applyUiUpdates()` refreshes only the tab named
by the `active_tab` member, but nothing ever updates that member when the tab
bar itself is touched. `lv_tabview` switches the visible page on its own; the
page never registers an `LV_EVENT_VALUE_CHANGED` handler, so `active_tab`
stays at `TAB_SYSTEM` for the whole session. Only the `Tab <` / `Tab >`
softkeys move it, because those set it by hand.

The Link tab is the tell: every figure on it is ESP32-local and needs no
backend at all, so it going blank rules out the telemetry link as the cause.

**Fix:** register a value-changed callback on the tabview and drive
`active_tab` from `lv_tabview_get_tab_active()`, so touch and softkey take the
same path. Refresh immediately on the change rather than waiting for the next
timer tick, or a freshly-opened tab shows stale content for up to a second.

---

## B2. Sample edit page freezes on Audition and on zoom — **FIXED**

**Reported:** pressing Audition plays the sample and then the UI freezes; zoom
in/out also locks the display.

**Cause:** `UISampleEditPage::handleWaveChunk()` is invoked from
`inter_mcu_invoke_wave_chunk_callback()`, which runs in the **UART RX task**,
and it calls `WaveformView::setSamples()` (`lv_chart_*`) and
`refreshStatus()` (`lv_label_set_text`) directly. That is LVGL from a
non-UI task with no lock held. `inter_mcu.h` already documents the rule for
the CV-cal listener two screens over — *"do NOT touch LVGL in the callback
(deferred-update pattern, ui-architecture.md)"* — and this page breaks it.

This is pre-existing, not new, but the rewrite made it fire constantly:
auditioning starts meter and wave traffic, and zoom re-requests a preview.

**Second, compounding cause:** `adjustFocused()` calls `requestWaveform()` on
*every* encoder detent. One turn of the encoder queues a burst of preview
requests, each answered by a train of wave chunks, each chunk poking LVGL from
the wrong task.

**Fix, in order:**
1. Make `handleWaveChunk` store into `preview_buffer_` and set a flag only —
   no LVGL. Apply on an `lv_timer` in the UI task, as the diagnostics page
   already does.
2. Debounce the preview request: coalesce encoder movement and re-request once
   the value settles (~150 ms), not per detent.
3. Guard against a stale reply: tag requests so chunks from a superseded
   window are dropped rather than drawn over the current one.

---

## B3. WAV duration is wrong for files over ~97 seconds — **FIXED**

**Reported:** multi-minute songs list as e.g. 38 s.

**Cause:** `firmware/daisy/src/comm/daisy_filesystem.cpp`, in the WAV metadata
parse:

```cpp
uint32_t frames = data_chunk_size / bytes_per_frame;
wire_entry.duration_ms = (frames * 1000u) / sample_rate;
```

`frames * 1000u` is 32-bit. It overflows once `frames > 4,294,967`, which at
44.1 kHz is **97.4 seconds**. A 3:00 file has 7,938,000 frames; ×1000 wraps to
3,643,032,704, which divided by 44,100 reports 82 s. Longer files wrap
repeatedly, which is why the reported figure has no obvious relationship to
the real one — 38 s is consistent with a file of roughly 3:53.

Everything downstream is fine: `FileEntryWire::duration_ms` is `uint32_t`, and
a 32-bit millisecond count holds 49 days. The loss happens entirely in that
one multiply.

**Fix:** promote the multiply, `(static_cast<uint64_t>(frames) * 1000u) /
sample_rate`. Add a unit test at 44.1 kHz and 48 kHz for a 10-minute file —
the existing WAV header tests all use short fixtures, which is why this
survived.

---

## B4. Combine the CPU tiles (requested) — **DONE**

Merge `CPU CORE 0` and `CPU CORE 1` into a single **ESP32 CPU** tile carrying
a sparkline plus two bars (one per core), and repurpose the freed tile for
**DAISY CPU** with its own sparkline and bar.

Notes for whoever implements it:

- `lv_chart` in `LV_CHART_TYPE_LINE` with a small point count is the cheap way
  to do a sparkline; the diagnostics spec already assumes it and it is enabled
  in the LVGL config.
- History has to be kept per series. The page currently holds
  `cpu_usage_history[10]`, which is unused by the new layout — size it to the
  sparkline's point count and add a matching buffer for the Daisy.
- Daisy CPU is already on `HeartbeatMessage` (`cpu_avg/min/max`), so this
  needs no protocol change. It is currently shown on the Link tab; moving it
  to System means the Link tab needs a replacement fourth card — frames/s from
  `DiagPushMessage::link_rx_frames`/`link_tx_frames` is the obvious candidate,
  but see B6.

**Confirmed working, do not regress:** the header status strip (meters + CPU).

Implemented: `ESP32 CPU` (busier core as the headline, a bar per core, one
sparkline) and `DAISY CPU` (sparkline + bar, from `HeartbeatMessage`). The
Link tab's freed card became `FRAMES/s`, derived from the frontend's own
packet counter as a rate rather than a since-boot total - free, and it needs
neither `MSG_DIAG_PUSH` nor `WAVEX_DAISY_UART_PERF_DEBUG`.

---

## B5. Sample browser not yet ported to the wireframe

The only screen from the design that has not been touched. The page's own
layout is about 40 lines, but the file list rows live in the shared
`wavex_file_browser` component, so the design's 54 px rows, green selection
ring and loading-spinner row reach outside `ui_sample_browser.cpp`.

Design 1b also calls for a detail panel (474×521) with the waveform, a
format/length/data-offset table and an audition progress bar. The `data`
row — "offset 44 (aligned)" — is worth keeping: `data_start % 4` correlated
exactly with the stutter across three files (see `docs/backlog.md`; the
mechanism is still unproven, and that entry warns specifically against
"fixing" it by rounding `data_start` up).

---

## B6. Backend link counters need `WAVEX_DAISY_UART_PERF_DEBUG`

`DiagPushMessage::link_*` is only populated when that flag is on, because
timing every `UartLinkProcess` call is the overhead the flag exists to gate.
The Link tab therefore shows the frontend's view only.

Decide one of: leave as-is and label the gap on the tab; split the flag so
frame/byte counts (cheap) are always on and only the microsecond timing is
gated; or accept the timing cost permanently now that we have a number for it.

---

## B7. Sample edit assumes a 48 kHz, 48000-frame sample

`kSampleFrames = 48000` and `formatFrames()`'s hard-coded 48 kHz are
placeholders: nothing tells the frontend how long the loaded sample actually
is, or at what rate. `MSG_SAMPLE_STATUS` carries `sample_rate` and
`frames_played`, and `FileEntryWire` carries `duration_ms` and `sample_rate` —
either could feed this, and after B3 the duration figure will be trustworthy.

Until then the labels say "at 48k" rather than implying a measured duration.

---

## B8. No busy feedback for long operations (requested)

**Reported:** loading a sample looks like a freeze — nothing indicates work is
in progress.

Worth separating two things, because the fix differs:

1. **The UI is genuinely blocked.** If the load runs on the UI task, no
   spinner will animate, because LVGL never gets to redraw. Check first: an
   overlay added on top of a blocked task is a spinner that never spins, which
   is worse than no feedback at all. The design's browser screen already
   anticipates this with a per-row loading spinner for in-flight pagination.
2. **The UI is free but says nothing.** Then a modal overlay is the right fix.

Suggested shape, once (1) is ruled out or fixed:

- A small shared `ui_busy_overlay` (semi-transparent scrim + `lv_spinner` +
  caption), shown/hidden by any page, so browser load, preview fetch and card
  remount all get the same treatment rather than three bespoke ones.
- Prefer a determinate `lv_bar` wherever a total is known.
  `MSG_SAMPLE_LOAD` carries `sample_size`, and `MSG_SAMPLE_STATUS` reports
  progress, so sample load can show real percentage rather than a spinner.
- Always pair it with a timeout and an error path. A spinner that never
  resolves is indistinguishable from the freeze it was added to explain — and
  the backend can genuinely fail to answer (card pulled mid-load).

Related: the sample browser's own "Loading 21-40..." row from design 1b, which
covers pagination specifically and is cheaper than a modal.

---

## B9. Unverified / needs a look

**All four fixes above are unverified on hardware.** B1 in particular was
diagnosed by reading the code, and B9 notes that Audio/Storage blankness is
attributed to it but not proven to be only it.

- The MIDI tab reads all zeros. Expected — the Daisy has no sequencer or tempo
  follower yet — but it has not been confirmed that the zeros are *arriving*
  rather than the tab failing the freshness check. B1 would mask the
  difference; recheck after B1 lands.
- Whether the diagnostics subscription actually reaches the Daisy has not been
  observed on hardware. `MSG_DIAG_PUSH` now has its own packet-stats counter,
  so the Link tab's message table answers this directly once B1 is fixed: a
  non-zero `DIAG_PUSH` row proves the round trip.
- Audio/Storage emptiness is *attributed* to B1 but not yet proven to be only
  B1. If they stay blank once the tab tracking is fixed, the next suspect is
  the subscribe never being sent or the 1.5 s freshness window being too tight
  for the real push cadence.
