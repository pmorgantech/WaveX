# Tuning & Scales — Design

**Status**: Target design (unimplemented). Phase 5 in `roadmap.md` (small, self-contained; can land any time after the instrument model).
**Dependencies**: `instrument-model.md` (the `pitch_ratio_mul` trigger hook is the entire engine surface this needs), `melodic-sequencing.md` (scale-constrained note entry), `arpeggiator.md` (arp conforms to scale).

---

## 1. Scope (deliberately v1-small)

- **Master tune**: global ±100 cents (the "A=442 tonight" knob).
- **Tuning table**: octave-repeating 12-entry cent-offset table (0 = 12-TET). Covers just intonation, Pythagorean, meantone, Arabic maqam quarter-tone subsets — the musically common cases — without a 128-note Scala engine. Full-keyboard 128-entry tables (true Scala import) are a documented later extension of the same message/file format (chunk version bump), not a v1 requirement.
- **Scale mask + root**: 12-bit mask naming the in-scale pitch classes, used by *input surfaces* (step editor pitch encoder, pad-to-note layouts, arp octave fills) — the engine itself never rejects a note (a wire `MSG_NOTE_ON` for an out-of-scale note always plays; masks are ergonomics, not policy).

## 2. Engine math (trigger-time only, zero per-sample cost)

At `ResolveNoteOn` (`instrument-model.md` §3), fold tuning into the existing multiplier:

```
cents(note) = master_tune_cents + table[note % 12]
pitch_ratio_mul *= 2^(cents(note) / 1200)
```

Composes with zone coarse/fine, p-lock pitch offsets, and mod-matrix pitch — all multipliers on `Voice::increment`. One `pow` per trigger (already precedented in `Trigger()`); optionally a 12+1-entry ratio cache refreshed when the table changes, making trigger cost two multiplies.

Tuning is **engine-global** (one table at a time, like hardware synths), not per-instrument — multi-timbral split tunings are out of scope until someone asks.

## 3. Protocol, files, UI

- `MSG_TUNING_SET` (0x68, E→D): `{int16_t master_cents_x10; int16_t table_cents_x10[12]; uint16_t scale_mask; uint8_t scale_root; uint8_t reserved}` — one idempotent whole-state set (34 B). Daisy applies at a control-tick boundary and refreshes the ratio cache. Round-trip + dispatch tests, doc row, same commit.
- Files: `0:/wavex/tunings/<name>.wxt` (WXCF file_type=4, one chunk mirroring the message payload + name). A handful of built-in presets ship in ESP32 flash (const tables), no SD required. Active tuning persists in project settings.
- UI: Settings → Tuning page — preset list, per-degree cent edit (encoder fine ±0.1 c), master tune, scale mask editor as a 12-key widget with root selector. Scale mask consumption: step editor snaps encoder pitch steps to in-scale notes; arp octave duplication respects the table automatically (it plays notes; tuning is downstream).

## 4. Tests & stages

Host: ratio math goldens (12-TET zeros = exact 2^(n/12); just-intonation table hits known ratios within float tolerance); cache refresh on table swap mid-run affects only *new* triggers (sounding voices keep their increment — documented behavior, matches hardware norms); scale-snap helper (ESP-side, host-tested as a pure function).

Stages (one commit each): 1) engine table + cache + `pitch_ratio_mul` fold, host-tested; 2) protocol 0x68 + tests + docs; 3) UI page + presets + `.wxt` save/load; 4) scale-snap wiring in step editor + arp.
