# WaveX Field Findings — 2026-08-29

Observed on hardware during the first bench session after the ESP32-P4 review
remediation (see `code_review_esp32_20260829.md`). Unlike that document, these
findings come from **running the instrument**, which is why several of them are
things no amount of source reading had surfaced.

Each entry states the symptom as observed, the root cause traced in source, and
whether it is a regression from recent work or long-standing. Work order is the
one set by the bench session, recorded in §8.

---

## 1. Daisy CPU load — REGRESSION, root cause found

**Observed:** idle 7–8% (was ~6–7%); one stereo sample playing 15% (was ~6–7%).

**It is not the SVF.** The pre-registered suspect in `roadmap.md` — the one-pole
becoming a 2-pole SVF in the callback's inner loop — cannot explain this
measurement, because **the SVF does not run on the streaming path at all**. It
lives in `VoiceManager::Render()` (`voice_manager.hpp:364`), which the callback
enters only when a note is sounding (`audio_engine.cpp:1505`). The browser's
Audition streams through the ring buffer and never touches it. Confirmed in the
binary: the only call to `SvfFilter::Process` in the whole image is inside
`VoiceManager::Render`. The roadmap item still stands as unmeasured — it just
does not own *this* number.

**The cause is `378673b fix(daisy): make the WAV ring buffer indices real
atomics`, amplified by the fact that the Daisy image is built with no `-O` flag
at all.**

`firmware/daisy/build/CMakeFiles/wavex-daisy.dir/flags.make` carries
`-std=gnu++14 -mcpu=cortex-m7 … -finline-functions` and **no optimization
level**. `CMAKE_BUILD_TYPE` is empty, and every configure site (`build.sh:36`,
`Makefile:27/40/53`) runs bare `cmake ..`. `-finline-functions` is inert without
`-O`. This contradicts `daisy_rt_audio_coding_guide.md` §8, which specifies
`-O3`.

At `-O0`, `std::atomic<T>::load/store` do not inline and the `memory_order`
argument is not constant-folded, so each access becomes out-of-line libstdc++
calls plus a full seq_cst `dmb` — **even where the source says
`memory_order_relaxed`**. That code sits in `rb_pop_stereo()`, which runs once
per sample. Measured on the shipped ELF:

| `rb_pop_stereo` | instructions | `dmb` | out-of-line calls |
|---|---|---|---|
| before `378673b` (volatile + `__DMB`) | 57 | 4 | 0 |
| after `378673b` (`std::atomic`) | 173 | 6 | **11** |
| **after this fix** | **69** | **3** | **0** |

Following the executed branches, the streaming path went 49 → 251 instructions
per sample, with 5 extra barriers per sample (240 per block). That asymmetry —
large on streaming, small on idle — is what the SVF hypothesis could never
account for, and it matches the observed +8 points playback vs +1 point idle.

**Fix applied:** the three ring indices now use the `__atomic_*` builtins with
literal memory orders, exactly as the note queue in the same file already did
(`audio_engine.cpp:234-262`). Identical semantics; they inline at every
optimization level. This was the *one* SPSC handoff in the file converted to the
`std::atomic` class template, and the only one in a per-sample loop.

**Also fixed here:** two `VDIV.F32` per sample on the streaming path
(`out[ch][i] = (float)x / 32768.0f`) became a multiply by the reciprocal —
bit-identical, since 1/32768 is a power of two, and VDIV is ~14 non-pipelined
cycles on Cortex-M7.

### Still open: the build has no optimization level — your decision

The narrow fix removes the regression, but the underlying condition remains:
**the whole Daisy image, including all DSP, is compiled `-O0`.** Turning on
`-O2`/`-O3` is not a change to make silently on a real-time audio target — it
alters timing everywhere and can expose latent UB that `-O0` was masking. It
needs a deliberate decision and a bench pass.

**Cheapest discriminating test, no source change:** rebuild once with
`-DCMAKE_BUILD_TYPE=Release` and repeat the two measurements.

### Process note

`AGENTS.md:54` and the guide require a DWT measurement for anything touching the
callback. `7fe2116` and `38f0ff3` at least state "compile/link-verified only".
`378673b` — the commit that made the inner loop ~5x more expensive — was filed as
a correctness fix and carries no performance note at all. The rule exists for
exactly this case: a change that is semantically right and costly.

### Separate correctness bug found while investigating

`.dtcmram_bss` is a `(NOLOAD)` section (`STM32H750IB_qspi.lds:139`) and
libDaisy's startup zeroes only `_sbss.._ebss`, so **nothing ever cleared DTCM**.
Every `WAVEX_DTCM_DATA` object started as whatever DTCM held — the previous
run's data on a warm reset — and its initializer was silently discarded. The
sharp edge: `s_voice_live_dirty` lives there, so a non-zero value at boot makes
the *first audio callback* push an uninitialized `s_voice_live_params` (garbage
cutoff, resonance, ADSR) into all eight voices. `s_rb_low_water`'s
`0xFFFFFFFF` initializer never landed either, so that diagnostic may have been
reporting nonsense. Fixed by zeroing the section in `MemorySections::InitDtcmBss()`
before anything reads it.

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
