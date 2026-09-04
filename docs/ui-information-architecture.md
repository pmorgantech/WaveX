# UI Navigation Reference

**Status:** As-built navigation reference. Implementation history is in git.

## 1. Navigation structure

- **Sample:** Browse, Edit, Manage, Record.
- **Play:** Pads and Keys.
- **Voice:** Sample, Env, Amp, Filter, Mod. Renamed **Instrument** in the
  Track/Instrument model's stage 1; target tabs Osc (1/2), Filter, Amp,
  Env (1/2/3), LFO (1/2), Mod, with Key Map / Pad Map reached from Osc.
- **Settings:** Display, Storage, MIDI, System, Calibrate.
- **Diagnostics:** ESP32, Daisy, Audio, Link, Storage, MIDI.
- Target additions (`features/track-and-patch-model.md` §6): **Track** (selector
  showing eight Tracks per page, MIDI in, poly, mixer strip, Load/Save
  Instrument), **Instrument Browser**, **Bank** (128 slots), **Mixer**.

The Sample group uses a host page because its tabs are independent pages. Play
and Voice each own their tab view because their shared state must survive tab
changes. Diagnostics builds tab bodies on first show to bound page-entry work.

## 2. Choosing tabs

Use tabs for children that share a subject or are too small to justify a
separate navigation step. Reuse the shared tab-group chrome; do not copy its
styles into pages. A tab switch must not start work that outlives the tab.

## 3. Play surfaces

Pads and Keys share note lifecycle behavior: press/release note events,
`PRESS_LOST` release, release-all on exit or transpose, latch, panic, and the
live-parameter strip. Only their layout and note map differ.

## 4. Page ownership

Each page owns one concern. The Track/Instrument model may change terminology,
but it must preserve this boundary. The selected Track (`ui/current_track.h`,
1-based on screen via `trackDisplayNumber()`) and the current sample are
explicit shared UI state; pages must not infer either from a previous page.
Anything that would replace what an occupied Track holds asks first (model doc
§1.3).

## 5. Diagnostics

Keep one authoritative source for each figure. Backend uptime comes from the
heartbeat; diagnostics telemetry supplies backend heap and sample-RAM figures.
Storage counters remain on the Storage tab rather than being duplicated on the
Daisy tab.

## 6. Lifecycle

Pages create and destroy their own timers, listeners, and LVGL objects. A page
that is not active must not receive updates or retain a listener into destroyed
state.

## 7. Runtime rules

- Nothing outside the UI task touches LVGL.
- The tab bar, header, and softkey row reduce usable page height; verify full
  pages and scrolling on the physical panel.
- Keep navigation and refresh work bounded; the UI task must never block.

## Related

- [UI architecture](ui-architecture.md)
- [UI design constraints](ui-design-constraints.md)
- [Outstanding hardware verification](roadmap.md#outstanding-hardware-verification)
