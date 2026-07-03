# WaveX System Architecture

**Status**: Canonical architecture document — this file is the single source of truth for system design.
**Last updated**: 2026-07-02
**Supersedes**: `system-architecture.md`, `communication-protocol.md`, `daisy_devel.md` (all moved to `docs/archive/`).

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
┌──────────────────────────────┐         ┌──────────────────────────────────┐
│  ESP32-P4 "Frontend"         │         │  Daisy Seed (STM32H750) "Backend"│
│  ESP-IDF 5.5.1 / FreeRTOS    │         │  libDaisy v8.0.0 (bare-metal)    │
│                              │         │                                  │
│  • LVGL 9.3 touchscreen UI   │  SPI    │  • Audio engine @48 kHz          │
│    (1280×720 MIPI-DSI+GT911) │◄───────►│  • Sample streaming from SD      │
│  • Encoders (PCNT), TCA8418  │ Daisy=  │    (SDMMC 4-bit + FatFs)         │
│    button matrix, TLC5947 LEDs│ master │  • 64 MB SDRAM sample RAM        │
│  • MIDI (UART DIN + USB)     │ ESP=    │  • CV outputs (VCF/VCA/CV-Gate)  │
│  • Sample browser / metadata │ slave   │  • PCM1690 8-ch TDM DAC (planned)│
│  • Presets & settings        │  +ATTN  │  • Metrics, heartbeat, profiling │
└──────────────────────────────┘  line   └──────────────────────────────────┘
```

**Division of responsibility**

| Concern | Owner | Rationale |
|---|---|---|
| UI, navigation, waveform display | ESP32-P4 | PSRAM + PPA + MIPI-DSI bandwidth |
| MIDI I/O | ESP32-P4 | USB host/device + UART; forwards note/CC over SPI |
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
| Inter-MCU link | SPI: **Daisy master / ESP32 slave**, mode 0, software CS, ATTN line ESP GPIO31 → Daisy D0 | SPI1 (Daisy) / SPI3_HOST slave (ESP) | working |

### 3.2 Authoritative configuration files

- **Pins**: `firmware/shared/config/pin_config.h` (both MCUs, one file).
- **Feature flags / peripheral config**: `firmware/shared/config/hardware_config.h` (e.g. `WAVEX_DAISY_SD_CARD_BACKEND`, SD bus width/speed, PCM1690 TDM settings).
- **Link tunables**: `pin_config.h` bottom section (`WAVEX_ESP_SPI_HOST`, ring sizes) and `link_config.h`.

Older documents (README pin tables, `system-architecture.md`, `daisy_devel.md`) contained multiple mutually contradictory pin maps (UART vs SPI link, ESP32-S3 vs P4, three different ESP32 pin tables). They are superseded; **never copy pin numbers into docs again**.

### 3.3 Open hardware decisions (must be resolved — tracked in roadmap)

1. **CV DAC part for Stage B**: the two-stage plan (§5.2) is now settled — Stage A uses the **MCP4728 (I2C, 4-ch, 12-bit)** already implemented in `cv_bus.hpp` for the paraphonic prototype. What remains open is the Stage-B part: `pin_config.h` reserves pins for **4× MCP48CMB28 (SPI, dual 12-bit)**. At the 1 kHz control tick, one MCP4728 fast-write (~9 bytes @400 kHz ≈ 225 µs) is fine for one shared CV group but cannot scale to 3–4 CVs × 8 voices; SPI DACs at 20+ MHz do it in <100 µs. **Recommendation**: SPI MCP48CMB28 chain for the production voice board. Decide before the analog voice board PCB is finalized.
2. **Radio**: ESP32-P4 has **no built-in WiFi/Bluetooth** (the WIFI6 board pairs an ESP32-C6 over SDIO). Older docs saying "WiFi/Bluetooth (disabled)" on the P4 were wrong. Decide whether the C6 is ever used (e.g. Ableton Link, sample transfer) or explicitly out of scope.
3. **ESP32 flash partition table** (`firmware/esp32/partitions.csv`) only allocates 2 MB of the 16 MB flash and includes a vestigial 640 KB "samples" partition (samples live on the Daisy SD card). Rework: larger app slots + OTA + assets.
4. **Analog voice count and CV-per-voice** (affects DAC count, TDM slot mapping, panel space): current plan is 8 voices × (cutoff, resonance, VCA, +1 spare). Confirm before PCB.

---

## 4. Firmware Architecture (as-built)

### 4.1 Repository layout

```
firmware/
├── esp32/                  # ESP-IDF 5.5.1 project (target: esp32p4)
│   ├── main/               # app entry, tasks, inter-MCU client
│   │   ├── links/          # esp_spi_link (active), esp_uart_link (legacy)
│   │   ├── comm/           # packet_router, listeners, statistics, ICommInterface
│   │   └── inter_mcu.cpp   # facade over link + router (large; slated for split)
│   ├── components/ui/      # navigator/page/softkey UI framework + pages
│   └── managed_components/ # lvgl 9.3, esp_lvgl_port, hx8394, gt911, p4 BSP
├── daisy/                  # CMake + arm-gcc project (libDaisy v8.0.0, DaisySP)
│   └── src/
│       ├── audio/          # audio_engine (callback, streaming, q15 pipeline), adapter
│       ├── comm/           # daisy_spi_link (active), daisy_uart_link (legacy), msg handlers
│       ├── storage/        # sd_sdio (active), sd_spi + diskio (legacy), fs_browse
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
- **Sample RAM**: `memory.h` slab (32 B–1 KB classes) + extent (64 KB pages) allocator over the 64 MB SDRAM, with stats reported to the UI via `MSG_STATUS_RESPONSE`/`SampleMemStatusMessage`.
- **Profiling**: DWT cycle counters (`profiling/`), `PROFILE_SCOPE` macros behind `WAVEX_PROFILING_ENABLED`, CPU load min/avg/max reported in heartbeats.

### 4.3 ESP32 frontend runtime model

FreeRTOS tasks:

- **UI task**: LVGL handler loop (~30 FPS), deferred-update pattern for data arriving from other tasks (never call LVGL off the UI task — see `ui-architecture.md`).
- **SPI slave task** (`esp_spi_link`): queues DMA slave transactions, raises **ATTN** (GPIO31) when TX data is pending so the Daisy master clocks a transaction, routes received packets to `PacketRouter`.
- **Input tasks**: PCNT encoder polling, TCA8418 interrupt-driven keypad.

Known architectural debt (from the 2026-06-26 assessment, still valid): event/callback fan-out ownership is split across `inter_mcu`, `PacketRouter`, `ListenersManager`, and `StatisticsManager` — one owner must be chosen; raw packed wire structs leak into UI code — wrap in encode/decode helpers.

### 4.4 Shared protocol

See `features/inter-mcu-protocol.md` for the full as-built wire specification (packet framing, size classes 32–2048 B, CRC16-CCITT, sequence numbers, ACK/NACK flags, and every message struct). Summary of the link:

- **Daisy is SPI master** (SPI1, mode 0, 8-bit, software CS); **ESP32 is SPI slave** (SPI3_HOST, DMA). The ESP32 signals pending data via the ATTN line; the Daisy also polls at a steady cadence. Clock is currently conservative (`PS_16` prescaler, set during bring-up); raising it is a roadmap item with scope verification.
- Fixed power-of-two transaction sizes (32/64/128/256/512/1024/2048) simplify DMA slave buffer management.
- Hardware CRC on the Daisy side, software fallback.
- **UART link code (`esp_uart_link`, `daisy_uart_link`) is legacy** — the design is SPI-only. Removal is a roadmap cleanup item; until removed, do not extend it.

---

## 5. Real-Time Audio Engine (as-built + target)

### 5.1 Timing budget (as-built)

| Parameter | Value |
|---|---|
| Sample rate | 48 kHz |
| Block size | 48 samples → **1.0 ms callback period == 1 kHz control tick** |
| Control tick work | envelopes, LFOs, mod matrix, CV staging, meter accumulation |
| CPU load target | ≤ 70% average in callback, measured continuously via DWT |

The 1-block = 1-ms identity is a deliberate design invariant: the control tick is derived from the audio callback, so CV, modulation, and (future) sequencer events are inherently phase-locked to the audio stream. Any change to block size must preserve an integer-ms tick or introduce a proper tick divider.

> **Invariant currently violated (found 2026-07-03, decision recorded)**: the as-built code actually runs at **44.1 kHz** (`main.cpp` sets `SAI_44KHZ` via a local libDaisy patch; `timebase.hpp` says `kAudioRate = 44100`), making the "1 kHz" tick really 918.75 Hz. Decision: return to **48 kHz** as this table states, with 44.1 kHz WAV content handled by the existing streaming resampler (audition) and playback-rate compensation in `VoiceManager` (RAM-resident samples). Details and consequences: `dma-timing-review-2026-07-03.md` Finding 1; sequencing: `roadmap.md` Phase 1 next-steps block. Remove this note once the switch lands.

### 5.2 Voice architecture (target — partially implemented)

8 voices, each: sample oscillator (streamed or RAM-resident) + optional VA oscillator + noise, 4 ADSR, 3 LFO, per-voice mod matrix.

**Implementation status (roadmap Phase 1 items 2 + 4)**: `firmware/daisy/src/audio/voice_manager.hpp` implements the RAM-resident half — 8-voice allocation/stealing (preferring a releasing voice when stealing), per-voice gain/pan, a note-relative pitch ratio, start/end/loop points, a one-pole lowpass filter stand-in (`audio/one_pole_filter.hpp`), and a linear ADSR (`audio/envelope.hpp`). Not yet implemented: streamed voices (still the old singleton WAV-ring-buffer path, not voice-manager-owned), VA oscillator/noise/LFO/mod matrix. Not yet wired into the audio callback — see items 2/4 in `roadmap.md` for why.

The analog output section is deliberately **two-stage**, selected by build flags (see §5.3):

- **Stage A — paraphonic prototype (now)**: all voices render digitally and sum to the **stereo codec on SAI1** (`AudioOutputMode::StereoSAI1`). The stereo mix passes through **one shared analog VCF/VCA pair**, driven by a single **MCP4728** (I2C, 4 ch: cutoff, resonance, VCA, +1 spare). Paraphonic semantics: the shared filter/amp envelope retriggers on each note-on and releases when the last voice releases (classic paraphonic behavior).
- **Stage B — 8 discrete analog voices (later)**: each voice routes to a dedicated PCM1690 TDM slot on SAI2 (`AudioOutputMode::VoiceSAI2`, 8×32-bit slots, 24-bit data, 12.288 MHz BCLK) → per-voice **analog VCF/VCA** → analog summing, with per-voice CV from SPI DACs. See `features/analog-voice-board.md`.

Digital send effects (delay/reverb) return into the stereo mix; per-voice character DSP (bit-crush, drive) runs digitally pre-DAC in both stages.

### 5.3 Output/CV backend abstraction (design rule, seam implemented)

**Implementation status (roadmap Phase 1 item 1, done)**: the flags, sink split, and CV group router below exist as specified — `firmware/shared/config/hardware_config.h` (flags), `firmware/daisy/src/audio/output_sink.hpp` (`StereoMixSink` real, `TdmVoiceSink` a compiling stub), `firmware/daisy/src/cv/` (`CvGroupRouter`, `Mcp4728Backend` real I2C, `Mcp48Backend` a compiling stub). What's *not* done yet: the router/sinks aren't wired into `audio_engine.cpp`'s callback, because there's no array of per-voice buffers to feed them until the voice manager (item 2) exists — today's engine still has exactly one playback source. The paraphonic fold's envelope law (Stage A, M=1) is deliberately not implemented here either; that's item 5. Both flag-set combinations are proven to compile in CI (`make daisy` / `make daisy-stageb`).

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
2. **DMA1/DMA2 cannot access DTCM** (0x20000000) or ITCM. Stack lives in DTCM by default — **never DMA to/from stack buffers**. SDMMC's IDMA requires AXI SRAM (D1); SDRAM is DMA-reachable but slow — stage SD reads through internal-RAM slots (the triple-buffer design does this).
3. **SDRAM accesses from the audio callback** should be sequential/batched (the q15 ring buffer does this); random single-word SDRAM access in the hot loop destroys the budget.
4. **No blocking transactions on the main loop longer than ~10 ms** (lesson learned from the UART-era ESP32-restart hang: a 100 ms blocking TX froze the system when the peer disappeared). All link I/O needs a timeout and a stuck-queue recovery path.
5. **Interrupt priorities**: audio SAI DMA highest; SPI link DMA below audio; EXTI (ATTN) below that; SysTick lowest. Any ISR added must be justified against the 1 ms budget.
6. **QSPI-resident code** (app runs from QSPI via bootloader, `BOOT_QSPI`): hot paths (audio callback, ring buffer ops) should be `ITCM`/`IRAM`-placed or verified cached; measure with DWT before and after moving code.

### 7.2 ESP32-P4

1. **SPI slave DMA buffers** must be in internal, DMA-capable memory (`MALLOC_CAP_DMA`), cache-line aligned (64 B on P4). Transactions use the fixed power-of-two sizes from the protocol.
2. **LVGL framebuffers**: MIPI-DSI scans from PSRAM; keep the two LVGL partial buffers in internal RAM sized per `esp_lvgl_port` guidance, let the DSI peripheral + PPA handle blits. Avoid CPU-touching the active scanout buffer.
3. **PSRAM (hex-mode @200 MHz)** is fast but shared with display refresh — bulk copies during UI animation cause bandwidth contention; schedule waveform-preview decode between frames.
4. **Never call LVGL from a non-UI task** (deadlocks under lock contention); use the deferred-update pattern (`ui-architecture.md`).

### 7.3 Cross-MCU timing contract

- Parameter changes (UI → audio): target < 5 ms end-to-end (touch → SPI → applied at next control tick).
- Meters/heartbeat: 20–50 ms cadence, coalesced, lowest priority.
- The link must degrade gracefully: either MCU rebooting must never wedge the other (timeouts + resync; regression-tested — roadmap Phase 1 item 7). UART's stuck-TX recovery (`daisy_uart_link.cpp`, 10ms transmit timeout + 500ms/1000ms force-clear) and the SPI-path reboot detection (`firmware/shared/spi_protocol/sequence_tracker.hpp`'s `SequenceTracker`, `attn_watchdog.hpp`'s `AttnWatchdog`) implement this; the latter two are HAL-free and host-tested, since real GPIO/SPI timing isn't testable without hardware.

---

## 8. Memory Architecture (as-built)

### Daisy Seed

| Region | Size | Use |
|---|---|---|
| ITCM/DTCM | 64/128 KB | hot code / stack, **not DMA-reachable** |
| AXI + D2/D3 SRAM | ~512 KB total | audio ring buffer, DMA slots, link buffers, bss |
| SDRAM (external) | 64 MB | sample RAM (slab+extent allocator), preview buffers, (future) offline-render scratch |
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
| Analog voice board (PCM1690 TDM, VCF/VCA, CV calibration) | `features/analog-voice-board.md` |
| Implementation order & upgrades | `roadmap.md` |
| Testing how-to | `testing_guide.md` |
| DWT/CPU profiling reference | `performance_monitoring.md` |
| Historical snapshots & superseded plans | `archive/` |

---

## 10. Known Design Gaps (summary — details and sequencing in `roadmap.md`)

1. **No sequencer exists** — the defining groovebox feature is unstarted (design doc now exists).
2. **Offline editing pipeline is unstarted** (design doc now exists); the current `Sampler` records into a heap `std::vector` from the audio path — must move to the sample RAM allocator with preallocated extents.
3. **CV DAC hardware decision** (§3.3) blocks the analog voice board.
4. **Polyphony**: engine currently plays one streamed WAV + preview; the 8-voice manager (allocation, stealing, per-voice params) is design-only.
5. **Event dispatch ownership on ESP32** is fragmented (four overlapping components).
6. **Legacy transports/backends** (UART links, SPI-SD) linger in-tree and confuse contributors.
7. **MIDI**: UART/USB MIDI is scaffolding only; clock sync (MIDI clock in/out) is required for a groovebox.
8. **Project/preset persistence format** is undefined (kits, patterns, songs, sample references).
