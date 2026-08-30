# UI Information Architecture — Target Menu Structure

**Status**: Target design (2026-08-30). Supersedes the ad-hoc main menu that grew one entry per page.
**Scope**: ESP32-P4 frontend navigation only. No engine behaviour changes.
**Platform guidance**: `docs/esp32p4_coding_guide.md` and the `esp32p4` skill apply to every change here; the two Daisy-side items (§6) additionally fall under `docs/daisy_rt_audio_coding_guide.md`.

---

## 1. Why

The main menu grew by appending an entry per page, so it now mixes levels of
abstraction: `Sample Browser`, `Edit Sample`, `Sample Manager`, `Voice`,
`Keyboard`, `Modulation`, `Settings`, `Diagnostics`. Three of those are views of
*the same sample*, and `Modulation` is a property of a voice rather than a
peer of the voice page — its three entries (LFO 1, LFO 2, Envelopes) are
unimplemented stubs that log and return.

Target:

```
Play         (Pads, Keys)
Sample       (Manage, Browse, Edit, Record)
Voice        (Sample, Env, Amp, Filter, Mod)
Settings     (Display, Storage, MIDI, System, Calibrate)
Diagnostics  (ESP32, Daisy, Audio, Link, Storage, MIDI)
```

**"Play" rather than "Keyboard"**, because the group now holds two different
instruments and one of them is not a keyboard. It is also the only verb among
five nouns, which is fitting: the other four are places where the instrument is
configured, and this is the one where it is played.

## 2. The grouping rule — tabs vs list

One rule, so the structure is predictable rather than per-page taste:

- **Tabs when the children share a subject.** Sample's four views are all views
  of *the current sample*; Voice's five are all parameter groups of *one voice*;
  Diagnostics' six are all facets of *the running system*. Tabs make switching
  cheap and, more importantly, carry the subject across the switch.
- **A menu list when the children are unrelated**, *and* deep enough that
  arriving at one is worth a navigation step.

**Settings is tabbed, against the second rule as originally written.** This
document first put Settings in a menu list because its children share no
subject, and that reasoning still holds — Display and MIDI have nothing to do
with each other. What the rule missed is that sharing a subject is not the only
thing that makes tabs the right shape. Settings' five children are *short*: two
of them are four rows, one is read-only, and CV Calibration is the only one deep
enough to feel like a place. A menu list makes the user pay a push and a pop to
cross between five screens that each fit on one, and it buried CV Calibration
two levels down from the main menu. So the rule is really two conditions, either
of which is enough: **tab children that share a subject, or that are individually
too small to be worth a navigation step.** Settings qualifies on the second.

This is not only cosmetic. The Sample group's shared subject fixes a real gap:
roadmap 1.5.1 item 7 records that the edit page "edits whatever the browser last
loaded, with no way to change it". Under a shared-subject tab group, selecting in
Browse or Manage *is* the way to change it, and the Shift-row `Select` key that
item reserved becomes unnecessary.

**Look and feel is already established** by the diagnostics page and must be
matched exactly rather than reinvented: `lv_tabview` with a 56 px tab bar,
`montserrat_22` bar text, dimmed inactive labels, and the selected tab drawn
filled with white text over a 4 px blue bottom border. Reuse those styles from
one place rather than copying the literals into each new page.

## 3. Play — two surfaces, one behaviour

Play stays top level because it is currently the **only** way to trigger a
digital voice without external MIDI hardware, and therefore the only way to hear
the filter, envelope or any live parameter edit at all
(`features/digital-voice-audition.md`). It is a *performance* surface, not a
configuration screen, so it does not belong under Voice: Voice is where a sound
is designed, Play is where it is used.

Two children, because they are genuinely different instruments:

- **Pads** — the existing 4×4 grid. Cell *n* plays `root + n`, so it is 16
  chromatic semitones today and becomes a kit (pad → sample) when the instrument
  model lands. Good for drums and for one-handed triggering.
- **Keys** — a piano layout: full-height white keys with narrower black keys
  overlaid at the right positions, spanning two to three octaves. Good for
  judging pitch and for playing a sampled instrument melodically, which a
  chromatic grid makes needlessly hard — a grid gives no visual cue which cell
  is a C.

**They must share behaviour, not just style.** Everything except the layout and
the note map is common: note-on/off with press/release (never `CLICKED`, which
fires on release and yields zero-length notes), `PRESS_LOST` treated as a
release so a slid-off finger cannot hang a note, per-key memory of the note
number actually sent so a transpose between press and release cannot end the
wrong note, release-all on exit and on transpose, Latch, the panic key, and the
voice-parameter strip with its encoder and `Value ±` fallbacks.

That is too much to duplicate, and duplicating it is how the two surfaces would
drift into behaving differently. Factor it into a shared play-surface base that
owns note state and parameters; each child supplies only its layout and its
`index → note` mapping. The single-touch constraint (§7) applies to both, so
Latch and the encoder path must exist on both.

## 4. Page disposition

| Today | Becomes | Note |
|---|---|---|
| `Sample Browser` | Sample ▸ Browse | Unchanged content |
| `Edit Sample` | Sample ▸ Edit | Gains a real sample selection from the shared subject |
| `Sample Manager` | Sample ▸ Manage | Becomes the group's default tab |
| `ui_sample_record_page` | Sample ▸ Record | Not currently reachable from the menu at all |
| `Voice` | Voice ▸ tabs | Split its current single view into Sample/Env/Amp/Filter/Mod |
| `Modulation` menu | Voice ▸ Mod | **Deleted.** Its three entries are logging stubs; nothing is lost |
| `ui_sample_memory_page` | Diagnostics ▸ Daisy | It is a memory breakdown, which is what that tab is for |
| `CV Calibration` | Settings ▸ Calibrate | Was already a Settings *list* entry, two levels from the main menu; becomes a tab |
| `Keyboard` | Play ▸ Pads | Stays top level, regrouped (§3) |
| — | Play ▸ Keys | New: piano layout sharing the pads' behaviour |

## 5. Diagnostics — splitting System into ESP32 and Daisy

The current `System` tab mixes both MCUs across eight cards (`ESP32 CPU`,
`DAISY CPU`, heap, PSRAM, LVGL pool, tasks, uptime, min-free-heap), with a
comment admitting the two-core ESP32 figures were folded into one tile because
"it cost a slot the Daisy needed". Splitting removes that pressure.

- **ESP32 tab**: CPU0 and CPU1 as separate tiles, each with its own sparkline
  and bar — the current single tile shows one sparkline of the busier core,
  which hides an imbalance between them. Plus internal heap, PSRAM, LVGL pool,
  task count, min-free-heap, uptime. All ESP32-local and live today.
- **Daisy tab**: engine CPU (average and max), sample RAM breakdown (small pool,
  large pool, largest free block, failed allocs, resident count), heap, uptime,
  and the resident-sample table absorbed from the standalone Sample Memory page.

  **Correction (stage 7, as built).** This list originally also said "SD
  counters". It should not: the Storage tab already carries the SD card, its
  throughput, latency, errors and last FRESULT/HAL result across five cards, and
  duplicating them onto Daisy would give two places to read one number and two
  places for them to disagree. The split is by *which machine owns the figure*
  only where that resolves an ambiguity; SD is unambiguous already because only
  the Daisy has the card. The Daisy tab's six card slots go to engine CPU, the
  two pools, largest-free-block, and the two placeholders.

**What is available and what is not.** `DiagPushMessage` already carries
`engine_cpu_x10`, `engine_cpu_max_x10`, `sample_ram_free`, `sample_ram_largest`,
`sample_failed_allocs` and `sample_count`, and `SampleMemStatusMessage` carries
the full small/large pool breakdown. **Daisy heap and uptime are carried by
neither** — they are the one genuine protocol gap in this redesign. Adding them
is two fields on `DiagPushMessage`, and per the cross-cutting rules that means
`protocol.h` + a round-trip test + an `inter-mcu-protocol.md` row in the same
commit. Until then those two cards must render as explicitly unavailable rather
than as zero: a zero uptime looks like a crash loop.

## 6. Staging

One verified commit each. Status is recorded against the code, not against the
plan — this list has already been overtaken once (stage 5 was written as
"absorb Modulation and delete that menu", but stage 4's commit had deleted the
menu, leaving only the tabbing to do).

1. **This document.** — **Done.**
2. **Shared tab-group scaffolding** — factor the diagnostics tabview styling into
   a reusable helper so the new groups cannot drift from it, with diagnostics
   itself converted to use it (proving it is really shared, not a copy).
   **Done**: `ui_tab_group.{h,cpp}` (`tabGroupCreate`/`tabGroupAddTab`) plus
   `ui_palette.h`.
3. **Play group** — extract the shared play-surface base from the existing
   keyboard page, re-land it as Pads, add Keys, tab them together.
   **Done**: `pages/ui_play_page.cpp`, one page owning its own tabview.
4. **Sample group** — tabs over the existing browse/edit/manage/record pages;
   main menu entry replaces three. **Done**: `UITabHostPage` +
   `createSampleGroup()`. The same commit deleted the Modulation menu.
5. **Voice group** — tabs over the five stages. **Done**:
   `pages/ui_voice_page.cpp` builds its own tabview with the shared chrome,
   tabs `Sample / Env / Amp / Filter / Mod` per §4. *Not verified on the panel.*
6. **Settings group** — fold CV Calibration in; fill the remaining stubs or mark
   them plainly as unimplemented rather than logging and returning. **Done**:
   `createSettingsGroup()` with Display / Storage / MIDI / System / Calibrate.
   Brightness, MIDI receive channel and the System tab are real; every other
   control states on the panel what it does not do rather than logging and
   returning. `Display ▸ Contrast` was deleted rather than marked — MIPI-DSI /
   HX8394 has no contrast control, so the row could never have done anything.
   Nothing persists: the frontend has no NVS code at all, and the pages say so.
   The page was also laid out for a 480x320 screen with a fixed 460x250 list,
   so it clipped past six rows — which had made CV Calibration's eleven
   unusable.
7. **Diagnostics split** — ESP32 (CPU0/CPU1 separate) and Daisy tabs; move the
   sample-memory page's content into the Daisy tab. **Done.** Six tabs: ESP32,
   Daisy, Audio, Link, Storage, MIDI. `ui_sample_memory_page.{h,cpp}` is deleted
   and the Diagnostics softkey that pushed it is gone; its pool figures are
   cards and its loaded-sample text block is a table on the Daisy tab. Three
   things were found in passing and fixed in the same commit, since each was a
   figure the page was reporting wrongly:
   - Sample-memory was never a **main menu** entry, contrary to how this stage
     was scoped. It was only ever reachable from Diagnostics softkey 5, so
     `ui_main_menu.cpp` needed no change at all.
   - The Link tab's `FRAMES/s` card rendered the Daisy's CPU *percentage* — the
     title had been updated when Daisy CPU moved to the System tab but the
     refresh had not. It now shows the packet rate over a 1 s window, which is
     what the card has claimed to show since that move.
   - The sparkline was drawn at y=118 over the sub-text line at y=112, so the
     context line on every sparkline card was invisible. Moved to y=140.

   Tab bodies are now built on first show rather than at page entry (§7's
   vertical-space and timer notes still hold; this is the page-entry cost noted
   in `docs/backlog.md`). Six tabs of eight cards would otherwise have made the
   worst page-entry cost in the UI about 20% worse.
8. **Daisy heap + uptime** — the protocol addition, with its round-trip test and
   doc row, so the two placeholder cards become live. **Not started.**

**Two shapes of tab group, not one.** Stage 2 shares the *chrome*, not the
hosting. Where the children are substantial independent pages (Sample), a
`UITabHostPage` hosts them as `UIPage`s and forwards the page contract to
whichever tab is selected. Where the children are views of one page's own state
(Play, Voice), the page builds its own tabview, because the readouts that state
needs — Play's parameter strip, Voice's name and status line — must survive a
tab switch and so cannot live in a tab body. Both call `tabGroupCreate()`, which
is what stage 2 was for.

## 7. Risks worth stating

- **Vertical space.** A tab bar costs 56 px on top of the header and the 100 px
  softkey row. Pages that were already full (the edit page's waveform, the
  browser's list) get less. Check each converted page against the real panel,
  not the simulator, before calling a stage done.
- **Softkey collision.** Grouped pages still need their own softkeys, and the
  row is six wide. The keyboard page already had to move root-note controls to
  the Shift row for this reason; expect the same pressure on Sample ▸ Edit.
- **Nothing in the UI task may block, and nothing outside it may touch LVGL.**
  Both rules have been broken before and both froze the display. Tab switching
  runs on the UI task, but the per-tab refresh timers must not start work that
  outlives the tab.
- **Muscle memory.** This moves every entry the user knows. Worth doing in one
  release rather than drifting over several.
