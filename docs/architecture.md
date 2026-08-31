# WaveX System Architecture

**Status**: Canonical architecture document — this file is the single source of truth for system design.
**Last updated**: 2026-07-05 (transport-reality corrections from `code_review_20260705.md`)
**Supersedes**: the former `system-architecture.md`, `communication-protocol.md` and `daisy_devel.md`, which carried mutually contradictory hardware claims (ESP32-S3 vs P4, UART vs SPI link, conflicting pin tables). Deleted; see git history.

When this document and the code disagree, the code wins for *as-built* sections and this document wins for *target design* sections; each section is labeled. Pin assignments live in exactly one place: `firmware/shared/config/pin_config.h`. Hardware feature flags live in `firmware/shared/config/hardware_config.h`. Do not duplicate pin tables into documentation.

---

## 1. Product Vision

WaveX is a modern **sampler / groovebox / drum machine** built around:

- A **5" 1280×720 MIPI-DSI touchscreen** with encoders, a button/pad matrix, and LED feedback — fast, tactile performance workflow (Elektron/MPC-style).
- A **digital sample engine** (multi-voice playback, streaming from SD, per-voice modulation) on a Daisy Seed (STM32H750, 480 MHz Cortex-M7, 64 MB SDRAM).
- A **per-voice analog signal path** — VCF (SSI2144) and VCA (SSI2164) per voice, driven by CV DACs — for genuinely analog filtering and level control.
- **CV/Gate outputs** for modular integration.
- **Modern editing and DSP**: waveform display, trim/slice, normalize, time-stretch, pitch-shift, and "mangling" effects. **All destructive sample editing/mangling is an offline (non-real-time) render**; the real-time audio path only ever plays back prepared data. See `features/offline-sample-editing.md`.
- A **step/pad sequencer** with song mode as the groovebox core. See `features/sequencer.md`.

### Non-negotiable engineering constraints

1. **The audio callback never blocks.** No SD I/O, no heap allocation, no I2C/SPI transactions, no logging inside the callback.
2. **All bulk data movement is DMA-driven** (audio SAI, SDMMC, inter-MCU SPI, MIPI-DSI) with explicit cache/alignment discipline (§7).
3. **Sample editing is offline.** DSP that cannot be guaranteed to complete within the audio block budget runs as a background render job, never in the real-time path.
4. **The inter-MCU wire contract is centralized** in `firmware/shared/spi_protocol/protocol.h` and covered by round-trip tests.

---

## 2. System Overview (as-built)

Dual-MCU split, each processor doing what it is best at:

```
┌──────────────────────────────┐          ┌──────────────────────────────────┐
│  ESP32-P4 "Frontend"         │          │  Daisy Seed (STM32H750) "Backend"│
│  ESP-IDF 5.5.1 / FreeRTOS    │   UART   │  libDaisy v8.1.0 (bare-metal)    │
│                              │ 2 Mbaud  │                                  │
│  • LVGL 9.3 touchscreen UI   │ UART1 ↔  │  • Audio engine @48 kHz          │
│    (1280×720 MIPI-DSI+GT911) │◄────────►│  • Sample streaming from SD      │
│  • Encoders (PCNT), TCA8418  │  UART4   │    (SDMMC 4-bit + FatFs)         │
│    button matrix, TLC5947 LEDs│         │  • 64 MB SDRAM sample RAM        │
│  • MIDI (UART DIN + USB)     │ (SPI link│  • CV outputs (VCF/VCA/CV-Gate)  │
│  • Sample browser / metadata │ wired but│  • PCM1690 8-ch TDM DAC (planned)│
│  • Presets & settings        │ disabled)│  • Metrics, heartbeat, profiling │
└──────────────────────────────┘          └──────────────────────────────────┘
```

**Division of responsibility**

| Concern | Owner | Rationale |
|---|---|---|
| UI, navigation, waveform display | ESP32-P4 | PSRAM + PPA + MIPI-DSI bandwidth |
| MIDI I/O | ESP32-P4 | USB device + DIN UART; forwards notes over the inter-MCU link |
| Sample storage (SD card) | Daisy | Audio engine streams directly; no sample data crosses the SPI link during playback |
| Real-time audio, voices, mixing | Daisy | Deterministic bare-metal loop, CMSIS-DSP, SDRAM |
| CV/Gate + analog voice control | Daisy | Generated at the 1 kHz control tick, phase-aligned with audio |
| Offline sample rendering | Daisy (SD→SD) | Data locality; see `features/offline-sample-editing.md` |
| Sequencer clock & event engine | Daisy (target) | Timing must be sample-accurate; UI only edits patterns |

The **file browsing model** follows from the storage split: the SD card is on the Daisy, so the ESP32 browses it remotely via `MSG_BROWSE_REQ`/`MSG_BROWSE_RESP` (paginated directory listings with WAV metadata), and requests auditioning/loading by path or index.

---

## 3. Hardware Architecture

### 3.1 Boards and major components (as-built)

| Component | Part | Interface | Status |
|---|---|---|---|
| Frontend MCU | ESP32-P4 (Waveshare ESP32-P4-WIFI6, 16 MB flash, PSRAM hex-mode @200 MHz) | — | working |
| Display | 5" 1280×720, HX8394 controller | MIPI-DSI 2-lane | working |
| Touch | GT911 capacitive | I2C0 (shared) | working |
| Button matrix | TCA8418, 8×8 | I2C0 (shared) + INT | driver present |
| Encoders | PCNT quadrature (+ MCP3008 ADC plan for endless encoders/pots) | PCNT / SPI2 | PCNT working |
| LEDs | TLC5947 48-ch PWM | SPI2 | planned |
| MIDI | DIN via UART2 @31250; USB MIDI | UART/USB | partial |
| Backend MCU | Daisy Seed rev (STM32H750, 480 MHz, 64 MB SDRAM, 8 MB QSPI) | — | working |
| Audio codec | Built-in (stereo in/out, 24-bit) | SAI1 | working |
| Multi-out DAC | PCM1690 8-ch | SAI2 TDM-8 + I2C control | planned (Phase: analog voice board) |
| SD card | microSD, SDMMC 4-bit via libDaisy `SdmmcHandler` + FatFs | SDMMC | working (SPI-SD legacy code still in tree) |
| CV DACs | **open decision — see §3.3** | I2C or SPI | prototype (MCP4728 I2C in code) |
| Inter-MCU link (live) | UART @ 2 Mbaud, framing in `firmware/shared/uart_protocol/` | UART1 (ESP) ↔ UART4 (Daisy) | working — carries **all** inter-MCU traffic |
| Inter-MCU link (SPI) | SPI: **Daisy master / ESP32 slave**, mode 0, software CS, ATTN line ESP → Daisy (`WAVEX_ESP_ATTN_OUT` / `WAVEX_DAISY_ATTN_IN`) | SPI1 (Daisy) / SPI3_HOST slave (ESP) | wired but **compiled out** (`WAVEX_SPI_LINK_ENABLED=0` in `link_config.h`); revival requires bench re-validation |

### 3.2 Authoritative configuration files

- **Pins**: `firmware/shared/config/pin_config.h` (both MCUs, one file).
- **Feature flags / peripheral config**: `firmware/shared/config/hardware_config.h` (e.g. `WAVEX_DAISY_SD_CARD_BACKEND`, SD bus width/speed, PCM1690 TDM settings).
- **Link tunables**: `pin_config.h` bottom section (`WAVEX_ESP_SPI_HOST`, ring sizes) and `link_config.h`.

Older documents (README pin tables, `system-architecture.md`, `daisy_devel.md`) contained multiple mutually contradictory pin maps (UART vs SPI link, ESP32-S3 vs P4, three different ESP32 pin tables). They are superseded; **never copy pin numbers into docs again**.

### 3.3 Open hardware decisions (must be resolved — tracked in roadmap)

1. **CV DAC part for Stage B**: the two-stage plan (§5.2) is now settled — Stage A uses the **MCP4728 (I2C, 4-ch, 12-bit)** already implemented in `cv_bus.hpp` for the paraphonic prototype. What remains open is the Stage-B part: `pin_config.h` reserves pins for **4× MCP48CMB28 (SPI, dual 12-bit)**. At the 1 kHz control tick, one MCP4728 fast-write (~9 bytes @400 kHz ≈ 225 µs) is fine for one shared CV group but cannot scale to 3–4 CVs × 8 voices; SPI DACs at 20+ MHz do it in <100 µs. **Recommendation**: SPI MCP48CMB28 chain for the production voice board. Decide before the analog voice board PCB is finalized.
2. **Radio**: ESP32-P4 has **no built-in WiFi/Bluetooth** (the WIFI6 board pairs an ESP32-C6 over SDIO). Older docs saying "WiFi/Bluetooth (disabled)" on the P4 were wrong. Decide whether the C6 is ever used (e.g. Ableton Link, sample transfer) or explicitly out of scope.
3. ~~**ESP32 flash partition table** rework~~ **Done** (roadmap 0.2.2): full 16 MB mapped, factory + two 4 MB OTA slots, vestigial "samples" partition dropped. Flash+boot verification on hardware still pending.
4. **Analog voice count and CV-per-voice** (affects DAC count, TDM slot mapping, panel space): current plan is 8 voices × (cutoff, resonance, VCA, +1 spare). Confirm before PCB.

---

## 4. Firmware Architecture (as-built)

### 4.1 Repository layout

```
firmware/
├── esp32/                  # ESP-IDF 5.5.1 project (target: esp32p4)
│   ├── main/               # app entry, tasks, inter-MCU client
│   │   ├── links/          # esp_uart_link (live), esp_spi_link (compiled out)
│   │   ├── comm/           # packet_router, statistics, ICommInterface
│   │   └── inter_mcu.cpp   # facade over link + router (large; slated for split)
│   ├── components/ui/      # navigator/page/softkey UI framework + pages
│   └── managed_components/ # lvgl 9.3, esp_lvgl_port, hx8394, gt911, p4 BSP
├── daisy/                  # CMake + arm-gcc project (libDaisy v8.1.0, DaisySP)
│   └── src/
│       ├── audio/          # audio_engine (callback, streaming, q15 pipeline), voice_manager
│       ├── comm/           # daisy_uart_link (live), daisy_spi_link (compiled out), msg handlers
│       ├── storage/        # sd_sdio, fs_browse
│       ├── metrics/ profiling/  # CPU load, DWT cycle counters
│       ├── memory.h        # SDRAM sample RAM manager (slab + extent allocator)
│       ├── sampler.hpp cv_bus.hpp timebase.hpp
│       └── main.cpp        # init + cooperative main loop
└── shared/
    ├── spi_protocol/       # WIRE CONTRACT: protocol.h/.cpp (+ tests in shared/tests)
    └── config/             # pin_config.h, hardware_config.h, link/logging config
```

### 4.2 Daisy backend runtime model

Bare-metal cooperative model — **two execution contexts only**:

1. **Audio callback** (highest priority, DMA-driven, 48 kHz / block 48 = 1 ms): pulls decoded q15 frames from the ring buffer, mixes voices, updates meters, advances the 1 kHz control tick (envelopes, LFOs, CV staging). Never blocks (§1).
2. **Main loop** (`main.cpp`): SPI link servicing, message dispatch, `PumpWavIO()` (SD streaming producer), underrun logging, metrics, and (future) offline render jobs — all cooperative, chunked, and preemptible by audio DMA interrupts.

Key subsystems:

- **Sample streaming**: triple-buffered SD read slots with ready/consumed flags; `PumpWavIO()` refills while the callback drains; conversion (mono/stereo → output mode, resampling via CMSIS `arm_linear_interp_q15`) happens in the pump path, not the callback's per-sample loop; `rb_push_frames()` batches ring-buffer writes with minimal barriers.
- **Sample RAM**: `memory.h` slab (32 B–1 KB classes) + extent (64 KB pages) allocator over a 60 MB arena; the final 4 MB is reserved for offline-render scratch. `sdram_layout.h` is the single ownership map. Stats report the complete reserved pool to the UI via `MSG_STATUS_RESPONSE`/`SampleMemStatusMessage`.
- **Profiling**: DWT cycle counters (`profiling/`), `PROFILE_SCOPE` macros behind `WAVEX_PROFILING_ENABLED`, CPU load min/avg/max reported in heartbeats.

**Why bare-metal, not an RTOS (decision, 2026-07-07).** The Daisy backend runs no RTOS by design, and musical timing *depends on* that choice rather than being limited by it:

- **The timebase is the audio DMA clock, not a software scheduler.** The SAI/DMA block-complete interrupt fires the callback every 48 samples at a hardware-derived rate; the 1 kHz control tick and all sequencer events are counted in audio *frames* (`frame = tick × frames_per_tick`, placed at a sample offset within the block), so they are sample-accurate and phase-locked to the audio they trigger (§5.1). A FreeRTOS SysTick is a *separate* clock domain and would beat against the SAI clock, reintroducing exactly the drift the frame-counting design eliminates — the same failure class as the 44.1 kHz regression (`dma-timing-review-2026-07-03.md` Finding 1).
- **Two priority levels don't need a scheduler.** The workload is audio (DMA IRQ) vs. everything-else (main loop) — the foreground/background split above. An RTOS earns its keep with many concurrent, I/O-bound tasks across several preemption levels; the Daisy has exactly one hard-real-time thread.
- **Hand-off is lock-free, and must stay that way.** Audio↔main state passes single-writer-per-field with release/acquire atomics (`__atomic_store_n(..., __ATOMIC_RELEASE)`); a control tick racing a main-loop write is benign and self-correcting. The audio callback must **never** block on a mutex (priority inversion → xrun), so sequencer pattern edits between steps use an atomic buffer/pointer swap, not a lock. That is both safer and cheaper than an RTOS mutex here.
- **The real risk is CPU budget, which an RTOS only worsens.** The timing failure mode is a callback exceeding its 1 ms budget (xrun), not scheduler jitter; it is measured continuously via DWT (§4.2 Profiling, §5.1). Context-switch and tick-ISR overhead would eat into that budget, not protect it.

The ESP32 frontend *does* run FreeRTOS (§4.3) — correct there, because it juggles many I/O-bound tasks (UI, link, input). The asymmetry is intentional: RTOS where there is genuine task concurrency, bare-metal foreground/background where there is one real-time thread. **Do not add an RTOS to the Daisy image.**

### 4.3 ESP32 frontend runtime model

FreeRTOS tasks:

- **UI task**: LVGL handler loop (~30 FPS), deferred-update pattern for data arriving from other tasks (never call LVGL off the UI task — see `ui-architecture.md`).
- **UART link task** (`esp_uart_link`): drains the ESP-IDF driver's RX ring, scans framed packets, and pumps queued TX into its software TX ring. The driver is interrupt-driven rather than GDMA-backed.
- **Dormant SPI slave task** (`esp_spi_link`): compiled only when `WAVEX_SPI_LINK_ENABLED=1`; its DMA transactions and ATTN signaling are not present in the shipped image.
- **Input tasks**: PCNT encoder polling, TCA8418 keypad FIFO polling.

Full task inventory (as-built, 2026-08-29). The guide requires name, priority,
stack, affinity and blocking behaviour to be written down; these were previously
only inline magic numbers:

| Task | Prio | Stack | Core | Blocks on | Notes |
|---|---|---|---|---|---|
| `main` (app_main) | 1 | 32768 | 0 | 1 s delay loop | Logs heap every 60 s; otherwise idle |
| `uart_link` | 6 | 16384 | any | driver event queue, 10 ms timeout | Woken on TX by a marker posted to the same queue |
| `ui_task` | 2 | 16384 | 1 | 32 ms delay | Takes the LVGL port lock per input event |
| LVGL port task | 4 | 7168 | any | esp_lvgl_port | Owns the tick and the display; created by the BSP |
| `pcnt_task` | 5 | 4096 | any | 2 ms delay | Polls quadrature counters; consumer runs at ~31 Hz |
| `tca8418_task` | 5 | 4096 | 1 | 10 ms delay | Polls the keypad event FIFO; does not use the INT line |
| `din_midi` | 5 | 4096 | any | UART read, 100 ms timeout | Bounded so it can observe a stop request |
| `usb_midi` | 5 | 4096 | any | task notification | Woken by TinyUSB's device task |
| `log_drain` | 1 | 3072 | any | 20 ms delay | Drains the log ring to the console |
| `scrshot` | 3 | 4096 | any | UART read, 200 ms | Debug builds only |
| TinyUSB device | esp_tinyusb default | — | — | USB events | Calls `tud_midi_rx_cb` |
| esp_timer task | 22 | — | 0 | timer queue | Shared; keep callbacks short (guide §11) |

Two of these still poll where an interrupt would do (`pcnt_task` at 500 Hz for a
31 Hz consumer, and the keypad at 100 Hz). Both are deliberate for now and
explained at the call site; converting either needs bench time.

**Lock order is LVGL → UART.** UI-task code takes the LVGL port lock and then
sends over the link, which briefly takes `s_uart_mutex`. Nothing may take them
in the other order — in particular, comm callbacks running on the UART task must
never touch LVGL; they stage data behind an atomic flag and let the owning page
draw it.

Known architectural debt (from the 2026-06-26 assessment, still valid): event/callback fan-out ownership is split across `inter_mcu`, `PacketRouter`, `ListenersManager`, and `StatisticsManager` — one owner must be chosen; raw packed wire structs leak into UI code — wrap in encode/decode helpers.

### 4.4 Shared protocol

See `features/inter-mcu-protocol.md` for the message catalog. Every message struct lives in `firmware/shared/spi_protocol/protocol.h` regardless of transport. Transport status (**as-built; decision recorded 2026-07-05**):

- **UART is the transport of record.** All inter-MCU traffic — heartbeat, meters, status, browse requests/responses, wave-preview chunks, note on/off, sample load/control — runs over UART1 (ESP32) ↔ UART4 (Daisy) at 2 Mbaud, using the framing in `firmware/shared/uart_protocol/uart_protocol.h` (0xA5/0x5A markers, 16-bit length, CRC16-CCITT, 16-bit sequence numbers) with `protocol.h` structs as payloads. Daisy UART4 uses independent continuous RX DMA1 Stream 5 and asynchronous TX DMA2 Stream 4 through the WaveX-owned `uart4_dma_transport`; this bypasses libDaisy v8.1.0's single-operation UART DMA scheduler (upstream issue #653). The ESP32 legacy UART driver is interrupt/ring-buffer driven. New messages target this link.
- **The SPI link is wired but compiled out**: `WAVEX_SPI_LINK_ENABLED` is `0` in `firmware/shared/config/link_config.h`, so `daisy_spi_link.cpp` / `esp_spi_link.cpp` (Daisy master / ESP32 slave, ATTN line, fixed power-of-two transaction sizes 32–2048 B) are in no shipped image. Re-enabling SPI — whether for bulk browse/wave data or full consolidation — is future work requiring bench re-validation, and roadmap Phase 1 item 3 ("raise SPI link clock") is blocked on it. Until then, do not extend the SPI path.
- The pre-2026-07-05 revision of this section stated the opposite ("SPI active, UART legacy"); see `docs/code_review_20260705.md` finding C4 for the correction trail.

---

## 5. Real-Time Audio Engine (as-built + target)

### 5.1 Timing budget (as-built)

| Parameter | Value |
|---|---|
| Sample rate | 48 kHz |
| Block size | 48 samples → **1.0 ms callback period == 1 kHz control tick** |
| Control tick work | envelopes, LFOs, mod matrix, CV staging, meter accumulation |
| CPU load target | ≤ 70% average in callback, measured continuously via DWT |

The 1-block = 1-ms identity is a deliberate design invariant: the control tick is derived from the audio callback, so CV, modulation, and (future) sequencer events are inherently phase-locked to the audio stream. Any change to block size must preserve an integer-ms tick or introduce a proper tick divider — `timebase.hpp` now enforces this with a `static_assert`. Because the tick is derived from the audio DMA clock rather than a software scheduler, no RTOS is used or needed on the Daisy — see §4.2 "Why bare-metal, not an RTOS" for the full rationale. (The engine briefly ran at 44.1 kHz, silently making the "1 kHz" tick 918.75 Hz; found and reverted 2026-07-03 — `dma-timing-review-2026-07-03.md` Finding 1.) Non-48 kHz WAV content is rate-converted at playback: the streaming/audition path resamples in `PumpWavIO`, and RAM-resident samples use playback-rate compensation (`VoiceTriggerParams::sample_rate_hz` scales `Voice::increment` by native/engine rate).

### 5.2 Voice architecture (target — partially implemented)

8 voices, each: sample oscillator (streamed or RAM-resident) + optional VA oscillator + noise, 4 ADSR, 3 LFO, per-voice mod matrix.

**Implementation status (roadmap Phase 1 items 2 + 4 + 8)**: `firmware/daisy/src/audio/voice_manager.hpp` implements the RAM-resident half — 8-voice allocation/stealing (preferring a releasing voice when stealing), per-voice gain/pan, a note-relative pitch ratio, start/end/loop points, a resonant state-variable lowpass (`audio/svf_filter.hpp` — TPT topology, cutoff + resonance, stable under modulation; it replaced the one-pole stand-in so `PARAM_FILTER_RESONANCE` has a digital consumer), and a linear ADSR (`audio/envelope.hpp`). It **is** wired into `Callback()` via an SPSC note-event queue (item 8 stage 2), and the UART message dispatcher feeds that path — `HandleNoteMessage` calls `AudioEngine::OnNoteOn()` (`daisy_inter_mcu_message_handlers.cpp`), fixed 2026-07-05 (`code_review_20260705.md` C1) — so it is reachable from hardware MIDI input, pending the hardware verification tracked in `roadmap.md` § Outstanding hardware verification. Not yet implemented: streamed voices (still the old singleton WAV-ring-buffer path, not voice-manager-owned), VA oscillator/noise/LFO/mod matrix.

The analog output section is deliberately **two-stage**, selected by build flags (see §5.3):

- **Stage A — paraphonic prototype (now)**: all voices render digitally and sum to the **stereo codec on SAI1** (`AudioOutputMode::StereoSAI1`). The stereo mix passes through **one shared analog VCF/VCA pair**, driven by a single **MCP4728** (I2C, 4 ch: cutoff, resonance, VCA, +1 spare). Paraphonic semantics: the shared filter/amp envelope retriggers on each note-on and releases when the last voice releases (classic paraphonic behavior).
- **Stage B — 8 discrete analog voices (later)**: each voice routes to a dedicated PCM1690 TDM slot on SAI2 (`AudioOutputMode::VoiceSAI2`, 8×32-bit slots, 24-bit data, 12.288 MHz BCLK) → per-voice **analog VCF/VCA** → analog summing, with per-voice CV from SPI DACs. See `features/analog-voice-board.md`.

Digital send effects (delay/reverb) return into the stereo mix; per-voice character DSP (bit-crush, drive) runs digitally pre-DAC in both stages.

### 5.3 Output/CV backend abstraction (design rule, seam implemented)

**Implementation status (roadmap Phase 1 item 1, done)**: the flags, sink split, and CV group router below exist as specified — `firmware/shared/config/hardware_config.h` (flags), `firmware/daisy/src/audio/output_sink.hpp` (`StereoMixSink` real, `TdmVoiceSink` a compiling stub), `firmware/daisy/src/cv/` (`CvGroupRouter`, `Mcp4728Backend` real I2C, `Mcp48Backend` a compiling stub). **Update 2026-07-05**: the CV group router IS now driven from the control tick (Stage A paraphonic law, item 5 stages 1-3) with the MCP4728 flushed from the main loop (`AudioEngine::FlushCv()`); the output *sink* abstraction remains unwired - the voice manager mixes directly to stereo, which is StereoMixSink-equivalent, and routing through TdmVoiceSink lands with Stage B. The paraphonic fold's envelope law (Stage A, M=1) is deliberately not implemented here either; that's item 5. Both flag-set combinations are proven to compile in CI (`make daisy` / `make daisy-stageb`).

So that Stage A → Stage B is a configuration change rather than a rewrite, the engine is structured around two seams:

1. **Output sink**: voices always render into per-voice block buffers; a sink stage consumes them. `StereoMixSink` sums into the SAI1 stereo stream (Stage A); `TdmVoiceSink` interleaves voice *i* into TDM slot *i* (Stage B). The existing `AudioOutputMode` enum remains the runtime switch; build flags set the default and exclude dead backend code from the build.
2. **CV group router**: upper layers (sequencer, param locks, UI) always address parameters **per voice**. A router folds voice-indexed CV targets onto *M* physical CV groups: Stage A has M=1 (paraphonic fold — shared envelope logic decides the group value); Stage B has M=8 (identity map). The CV backend behind the router is `Mcp4728Backend` (I2C) or `Mcp48Backend` (SPI chain), same `QueueGroup()/Flush()` interface, both flushed from the main loop per §7.

Configuration flags (defined in `firmware/shared/config/hardware_config.h`):

```c
// Voice output backend
#define WAVEX_VOICE_OUTPUT_STEREO_MIX 0   // Stage A: sum to SAI1 codec
#define WAVEX_VOICE_OUTPUT_TDM8       1   // Stage B: PCM1690 per-voice slots
#define WAVEX_VOICE_OUTPUT_BACKEND    WAVEX_VOICE_OUTPUT_STEREO_MIX

// CV backend
#define WAVEX_CV_BACKEND_MCP4728      0   // I2C, one shared (paraphonic) CV group
#define WAVEX_CV_BACKEND_MCP48        1   // SPI chain, per-voice CV groups
#define WAVEX_CV_BACKEND              WAVEX_CV_BACKEND_MCP4728

// Physical analog CV groups (1 = paraphonic, 8 = full voice board)
#define WAVEX_ANALOG_CV_GROUPS        1

// Calibration tables are always sized for this many groups, regardless of
// WAVEX_ANALOG_CV_GROUPS, so stored calibration data survives Stage A -> B.
#define WAVEX_ANALOG_CV_GROUPS_MAX    8
```

Invariants that keep the transition safe: voice index == TDM slot index == CV group index in Stage B; calibration tables are always sized for 8 groups regardless of backend; nothing above the router may branch on the backend flags.

### 5.3 Streaming vs RAM playback

- Short samples (drum hits) load fully into SDRAM sample RAM (slab/extent allocator) — zero I/O at trigger time.
- Long samples stream: triple-buffered SD slots, prebuffer before start (`IsPrebufferReady()`), pump in main loop. Worst-case SD latency must stay under (slots × slot duration); underruns are counted and logged from the main loop, never inside the callback.

---

## 6. Offline Sample Editing & DSP (target design)

**Rule: destructive editing and DSP "mangling" never run in the real-time path.** They are render jobs executed on the Daisy main loop in bounded chunks (or, for preview-only effects, on the ESP32 against decimated preview data). The full design — edit model, render-job scheduler, chunk budget, progress reporting, cancellation, temp-file/atomic-rename strategy, and the effect catalog (trim, normalize, fades, resample, time-stretch, pitch-shift, bit-crush, etc.) — is in `features/offline-sample-editing.md`.

Real-time-safe *playback-time* parameters (start/end/loop points, playback rate, per-voice filter/level) remain non-destructive and live.

---

## 7. DMA, Cache, and Timing Rules (normative)

These rules are mandatory for all new code. Most past instability (SPI corruption, SD glitches, display artifacts) traces to violations of one of them.

### 7.1 STM32H750 (Daisy)

1. **D-Cache is enabled.** Every DMA buffer must be 32-byte aligned and padded to a multiple of 32 B (`__attribute__((aligned(32)))`), and either:
   - placed in a **non-cacheable region** (libDaisy `DMA_BUFFER_MEM_SECTION` → D2 SRAM configured non-cacheable), or
   - explicitly maintained: `SCB_CleanDCache_by_Addr` before TX, `SCB_InvalidateDCache_by_Addr` after RX. Never invalidate a buffer that shares a cache line with unrelated data.
2. **DMA1/DMA2 cannot access DTCM** (0x20000000) or ITCM. Stack lives in DTCM by default — **never DMA to/from stack buffers**. SDMMC's IDMA requires AXI SRAM (D1); SDRAM is DMA-reachable but slow. All active SD paths, including `OnSampleLoad`, stage reads through aligned AXI-SRAM buffers before copying into SDRAM.
3. **SDRAM accesses from the audio callback** should be sequential/batched (the q15 ring buffer does this); random single-word SDRAM access in the hot loop destroys the budget.
4. **No blocking transactions on the main loop longer than ~10 ms** (lesson learned from the UART-era ESP32-restart hang: a 100 ms blocking TX froze the system when the peer disappeared). All link I/O needs a timeout and a stuck-queue recovery path.
5. **Interrupt priorities**: audio SAI DMA highest (5/6); UART RX/TX DMA below audio (7); SPI link DMA below UART (10); SysTick lowest. Any ISR added must be justified against the 1 ms budget.
6. **QSPI-resident code** (app runs from QSPI via bootloader, `BOOT_QSPI`): `WAVEX_ITCM_CODE` places measured hot functions in the copied-at-boot `.itcm_text` section. UART4 DMA RX position handling uses it; the IRQ entry points stay in QSPI so no vector can target ITCM during its early-boot copy. Audio callback code must move only after before/after DWT measurement.

### 7.2 ESP32-P4

1. **SPI slave DMA buffers** must be in internal, DMA-capable memory (`MALLOC_CAP_DMA`), cache-line aligned (64 B on P4). Transactions use the fixed power-of-two sizes from the protocol.
2. **LVGL framebuffers**: MIPI-DSI scans from three full framebuffers in DMA-capable PSRAM; two LVGL partial buffers plus PPA rotation scratch use internal DMA-capable RAM. Log capability-specific free/minimum heap before and after display creation. Avoid CPU-touching the active scanout buffer.
3. **PSRAM (hex-mode @200 MHz)** is fast but shared with display refresh — bulk copies during UI animation cause bandwidth contention; schedule waveform-preview decode between frames.
4. **Never call LVGL from a non-UI task** (deadlocks under lock contention); use the deferred-update pattern (`ui-architecture.md`).

### 7.3 Cross-MCU timing contract

- Parameter changes (UI → audio): target < 5 ms end-to-end (touch → SPI → applied at next control tick).
- Meters/heartbeat: 20–50 ms cadence, coalesced, lowest priority.
- The link must degrade gracefully: either MCU rebooting must never wedge the other (recovery + resync; regression-tested — roadmap Phase 1 item 7). Daisy UART TX is asynchronous DMA with a bounded one-second retry/drop policy; it never waits for peer wire time in the main loop. `SequenceTracker` (`firmware/shared/spi_protocol/sequence_tracker.hpp`) is wired into **both live UART RX paths** (duplicate/out-of-order drop + peer-reboot resync, counted in link stats) as well as the compiled-out SPI path; `AttnWatchdog` (`attn_watchdog.hpp`) is SPI-path-only by nature (there is no ATTN line on UART). Both are HAL-free and host-tested.

---

## 8. Memory Architecture (as-built)

### Daisy Seed

| Region | Size | Use |
|---|---|---|
| ITCM/DTCM | 64/128 KB | explicitly annotated hot code / stack, **not DMA-reachable**; static DTCM is link-capped at 64 KB to preserve at least 64 KB for descending stacks |
| AXI + D2/D3 SRAM | ~512 KB total | audio ring buffer, DMA slots, link buffers, bss |
| SDRAM (external) | 64 MB | 60 MB sample arena + 4 MB dedicated offline-render scratch (`sdram_layout.h`) |
| QSPI flash | 8 MB | application (BOOT_QSPI via Daisy bootloader) |
| SD card | up to SDXC | samples, projects/presets, rendered files |

### ESP32-P4

| Region | Size | Use |
|---|---|---|
| Internal SRAM (L2MEM) | 768 KB | tasks, LVGL working buffers, SPI slave DMA buffers |
| PSRAM | module-dependent (hex-mode @200 MHz) | framebuffers, UI assets, waveform preview caches |
| Flash | 16 MB | app + assets (partition table rework pending, §3.3) |

---

## 9. UI, Testing, and Feature Documentation Map

| Topic | Document |
|---|---|
| UI framework (navigator, pages, softkeys, LVGL threading) | `ui-architecture.md` (canonical) + `ui-system-implementation-guide.md` (how-to) |
| Inter-MCU protocol wire spec | `features/inter-mcu-protocol.md` |
| Offline sample editing & DSP | `features/offline-sample-editing.md` |
| Sequencer / groovebox engine | `features/sequencer.md` |
| Feature-expansion suite index (2026-07-05) + message-ID reservations | `features/feature-expansion-ideas.md` |
| Instrument model (presets/zones/velocity layers, WXCF container) | `features/instrument-model.md` |
| MIDI clock sync (tempo follower) | `features/midi-sync-tempo-follower.md` |
| Melodic sequencing / live record | `features/melodic-sequencing.md` |
| Param locks, mod matrix, LFOs | `features/param-locks-and-modulation.md` |
| Sampling & recording | `features/sampling-and-recording.md` |
| Arpeggiator | `features/arpeggiator.md` |
| Scenes, macros, param slew | `features/scenes-and-performance.md` |
| Tuning & scales | `features/tuning-and-scales.md` |
| Output routing & mixer | `features/output-routing-and-mixer.md` |
| Analog voice board (PCM1690 TDM, VCF/VCA, CV calibration) | `features/analog-voice-board.md` |
| Implementation order & upgrades | `roadmap.md` |
| Testing how-to | `testing_guide.md` |
| DWT/CPU profiling reference | `performance_monitoring.md` |
| Historical snapshots & superseded plans | git history (`git log -- docs/`) |

---

## 10. Known Design Gaps (summary — details and sequencing in `roadmap.md`)

1. **No sequencer exists** — the defining groovebox feature is unstarted (design doc now exists).
2. **Offline editing pipeline is unstarted** (design doc now exists). The legacy `Sampler` (and the never-rendered mono-synth DSP surface around it) was removed 2026-07-05 as inert end-to-end (code review C2); recording will be rebuilt against the voice/streaming architecture when it is actually scheduled. `MSG_SAMPLE_CTRL`/`MSG_CONTROL_CHANGE` remain routed wire hooks that are documented no-ops until then.
3. **CV DAC hardware decision** (§3.3) blocks the analog voice board.
4. **Polyphony**: the 8-voice manager (allocation, stealing, per-voice pitch/filter/ADSR) is implemented, host-tested, and wired into the callback — but unreachable from the wire until the dispatcher fix lands (code review C1). Streamed playback is still the singleton WAV path.
5. **Event dispatch ownership on ESP32** is consolidated onto `PacketRouter` for routing, but listener registration still lives in `StatisticsManager`/`inter_mcu` (code review M9).
6. **The disabled SPI link lingers in-tree** (compiled out via `WAVEX_SPI_LINK_ENABLED=0`) pending a revival-or-delete decision; SPI-SD was removed (roadmap 0.2.1). UART is the transport of record (§4.4).
7. **MIDI**: DIN/USB input and forwarding exist on the ESP32; the Daisy-side dispatch is broken (C1); clock sync (MIDI clock in/out) is required for a groovebox and unstarted.
8. **Project/preset persistence format** is undefined (kits, patterns, songs, sample references).
