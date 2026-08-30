# WaveX Field Findings — 2026-08-29

Observed on hardware during the first bench session after the ESP32-P4 review
remediation (see `code_review_esp32_20260829.md`). Unlike that document, these
findings come from **running the instrument**, which is why several of them are
things no amount of source reading had surfaced.

Each entry states the symptom as observed, the root cause traced in source, and
whether it is a regression from recent work or long-standing. Work order is the
one set by the bench session, recorded in §8.

---

## 1. Daisy CPU load — under analysis

**Observed:** idle 7–8% (previously ~6–7%); one stereo sample playing 15%
(previously ~6–7%, so roughly doubled).

Analysis in progress; this section will record the per-sample cost breakdown and
name the responsible change. What is already known: the ESP32 remediation work
did **not** touch `firmware/daisy/` at all (verified — no commit in that series
has a Daisy file in its diff), so the cause lies in the Daisy/audio commits that
predate it. The prime suspect is recorded and was flagged in advance:

> **roadmap.md § Outstanding hardware verification — "Per-voice SVF cost":** the
> one-pole became a 2-pole state-variable filter in the callback's inner loop,
> ×8 voices. Host tests prove it is *correct*; nothing proves it is
> *affordable*. The guide requires a DWT number before a DSP change in the
> callback is accepted.

That item was written precisely because this number was never taken. The bench
measurement above is the first evidence either way, and it is not encouraging.

**Note the discriminator:** if the SVF is the cause, the cost should appear on
the **note-on / VoiceManager** path only — the browser's Audition uses the
*streaming* ring-buffer path, which does not run the voice filter. A doubling on
streaming playback would therefore point somewhere else (per-sample fades, gain,
resampler, or the ring-buffer atomics). Establishing which is what the analysis
is for.

---

## 2. Keyboard pads produce no sound and no log on either MCU

**Observed:** tapping pads on the Keyboard page is laggy, emits no sound, and
prints nothing on the ESP32 *or* the Daisy console. Reported twice.

**Root cause of the silence — the logging, not the notes.** Two independent
gates mean an absence of log output carries no information here:

- **ESP32:** the send path logs through `UART_LOGI`, and
  `uart_debug_config.h` sets `WAVEX_UART_DEBUG_LEVEL 2` (WARN). Every `UART_LOGI`
  is compiled out. The ESP32 is *expected* to be silent on a note send.
- **Daisy:** `OnNoteOn`'s "dropped (no playable sample)" line is
  `WaveX::Log::PrintLine`, which writes to a ring drained over **USB CDC**, not
  the UART console. It is also gated behind `if (s_hw)`.

So "nothing is logged" does not distinguish *note never sent* from *note sent
and dropped*. **The instrument cannot currently tell you why it is silent, and
that is the defect to fix first** — not the note path.

**What is known to be correct** (traced end to end): pad → `pad_event_cb`
(`ui_keyboard_page.cpp:275`) → `press()` (:317) → `inter_mcu_send_note_on` →
UART → `HandleNoteMessage` (`daisy_inter_mcu_message_handlers.cpp:223`) →
`AudioEngine::OnNoteOn`. `WAVEX_AUDIO_ENGINE_ENABLED` is 1, so the call is
compiled in. ESP32→Daisy transmission demonstrably works, because browse
requests reach the Daisy and return listings.

**Most likely cause of no sound:** `find_playable_sample()`
(`audio_engine.cpp`) requires a resident sample with `bit_depth == 16` and 1–2
channels. If none is loaded — or the loaded file is 24-bit, which the browser
accepts — every note is dropped. See §3.

**Fix order:** make the failure visible (a real status on the Keyboard page,
using the `SampleMetadata` the frontend already receives), *then* re-test. Do
not chase the note path until the instrument can report on itself.

---

## 3. "Loaded" samples cannot be played from the keyboard

**Observed:** a sample is loaded from the browser, but the Keyboard page cannot
play it and its caption says a 16-bit sample is needed.

**Root causes, two of them:**

1. **The caption is static.** `ui_keyboard_page.cpp:388` unconditionally appends
   `(needs a 16-bit sample loaded)` to the info line whether or not one is
   loaded. It reads as a status report and is not one.
2. **Audition is not Load.** The browser's **Audition** softkey sends
   `MSG_SAMPLE_PLAY_INDEX_REQ`, which streams a *file by listing index* and never
   makes it RAM-resident. Only **Load** sends `MSG_SAMPLE_LOAD`. A sample that
   was auditioned and not loaded is genuinely absent from `s_loaded_samples`.
3. **Bit depth is accepted then silently unplayable.** `loadSample()`
   (`ui_sample_browser.cpp:1012`) accepts 8, 16 and 24-bit. `find_playable_sample()`
   accepts only 16-bit. A 24-bit file reports `Sample loaded: id=…` and can never
   be triggered, with no feedback anywhere.

---

## 4. Sample Edit: audition plays the wrong sample, and ignores every edit

**Observed:** the edit page's Audition does not play the loaded/edited sample but
whatever the browser cursor is on; start/end, loop start/end and the other
controls have no audible effect; loop appears not to work at all.

**One root cause explains all of it.** The page edits one thing and auditions
another:

| | Targets | Mechanism |
|---|---|---|
| Editing | the **loaded sample** in RAM | `sendEdit()` → `MSG_SAMPLE_EDIT_SET` → `AudioEngine::SetEditParams(slot, …)` |
| Audition | a **file on disk, by browser cursor index** | `toggleAudition()` → `inter_mcu_send_sample_play_index_req(state->selected_file_index)` → streaming path |

`toggleAudition()` (`ui_sample_edit_page.cpp`) uses
`state->selected_file_index`, while `currentSampleId()` uses
`state->last_load_sample_id`. Two different addressing schemes — a browser
listing position versus a RAM sample id. The page's own status string already
half-admits it: *"Auditioning whole file (range not sent - no protocol)"*.

**Important: the edits themselves are not broken.** They are applied in RAM and
they *are* honoured — by the **note-on voice path**. `OnNoteOn`
(`audio_engine.cpp:1821-1823`) passes `loop_enabled`, `loop_start` and
`loop_end` from the sample record into the voice, and `VoiceManager`
(`voice_manager.hpp:319`) wraps the phase at `loop_end` back to `loop_start`.
Markers, gain and fades come from the same record. So looping works; it is only
inaudible because the Audition button plays a different code path that knows
nothing about any of it.

**Fix:** make the edit page's Audition trigger the **loaded sample through the
voice path** (note-on at the sample's root note; note-off to stop) rather than
streaming a file by index. That is close to free, immediately makes every edit
audible, and has the useful side effect of exercising the same path the Keyboard
page uses — so it doubles as a test for §2.

**Persistence does not exist.** `Save` and `Save As` are deliberately unwired —
`keys[3] = {"Save", nullptr, false, "needs marker persistence"}` and
`keys[4] = {"Save As", nullptr, false, "needs filename entry"}`. Edits live in
RAM and die at power-off. There is no message in the protocol for writing a
sample record back to the card.

---

## 5. No sample management

**Observed:** no way to unload, delete, rename or reorder samples in RAM.

**State:** the Daisy owns `s_loaded_samples` plus a memory manager, and
`SampleMetadata` already carries `name`, `sample_id`, `generation` and a
resident flag. The frontend has a read-only Sample Memory page. What is missing
is any *protocol message to act on* a loaded sample — no unload, no rename, no
reorder. This is a feature with a protocol design in front of it, not a bug.

---

## 6. Voice / Preset does not exist as an entity

**Observed:** envelopes and filter settings are global page state on the Keyboard
page, not properties of anything nameable or saveable.

**Target shape** (from the bench session): a **Voice/Preset** is a named entity,
saveable to and loadable from the card, consisting of:

- a **sample** with key-tracking settings,
- **Env → Gain**,
- **Env → Filter**,
- **filter settings**,
- **modulation settings and wirings**.

Today the pieces exist but unowned: `VoiceLiveParams`/`VoiceTriggerParams` carry
filter and envelope values on the Daisy, the Keyboard page edits them globally
via `MSG_CONTROL_CHANGE`, and nothing associates them with a sample or a name.
This is the largest item here and needs a protocol and on-disk format decided
before any UI is built.

---

## 7. Busy overlay misrepresents errors as timeouts

**Observed:** "Sample will not fit" spins for several seconds and then becomes
"No response from backend — Tap to dismiss". "Backend timeout" also hangs around.

**Root cause:** error messages are shown with the same call used for operations
*in flight* — `BusyOverlay::show(caption, detail, 6000)` — so they get a spinner
and a 6-second timeout, and when it expires `onTimeout()` rewrites the caption to
"No response from backend". For "will not fit" that is simply false: the backend
answered, and the answer was no.

**Fix:** errors need a distinct presentation from in-flight work — no spinner, no
timeout mutation, auto-dismiss after ~2 s.

---

## 8. Work order

Set at the bench, and followed in this order:

1. **Daisy CPU load** — establish whether it is expected or a regression, then
   act. (§1)
2. **Browser tap** — the click registers visually but the selection does not
   move. (Not yet root-caused: rows do carry their entry index, the handler reads
   it, and the repaint path is drained each UI pass — so the failure is not where
   static reading can see it. Needs instrumentation.)
3. **Sample Edit page** — audition the edited sample through the voice path;
   make edits audible; then persistence. (§4)
4. **Sample Manager page** — needs protocol messages first. (§5)
5. **Voice/Preset page** — needs the entity, its protocol and its on-disk format
   designed first. (§6)

§2 (keyboard visibility) and §7 (overlay) are small and unblock diagnosis of the
rest; they should ride along with the earliest item they touch rather than wait
for their own slot.
