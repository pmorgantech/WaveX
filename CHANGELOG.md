# Changelog

All notable changes to WaveX are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Frontend (ESP32-P4) and backend (Daisy Seed) firmware share a single version
number, defined in the root [`VERSION`](VERSION) file. See `AGENTS.md` for the
versioning and release process.

## [Unreleased]

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
