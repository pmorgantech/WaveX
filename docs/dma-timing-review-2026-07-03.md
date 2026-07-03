# DMA & Timing Review — UART Link + Audio Path (2026-07-03)

**Status**: Point-in-time code review, not a living document. Findings reference the code as of commit `572b778`. Fix status is tracked in `roadmap.md` (Phase 1 "Next steps" block), not here.
**Scope**: `firmware/daisy/src/comm/daisy_uart_link.cpp`, `firmware/daisy/src/audio/audio_engine.cpp` (ring buffer, prebuffer, SD streaming, callback), `firmware/esp32/main/links/esp_uart_link.cpp`, and the relevant vendored libDaisy v8.1.0 driver sources (`per/uart.cpp`, `sys/dma.c`, `util/sd_diskio.c`, `sys/system.cpp` MPU config), checked against the normative rules in `architecture.md` §7.
**Method**: Full source reading of both link ends and the audio data path; libDaisy behavior verified against the vendored sources (the exact code that ships), ESP-IDF behavior against the official docs and issue tracker. No hardware measurements were possible — everything below is static analysis; wire-time and ISR-duration numbers are computed from clock/baud arithmetic, not measured.

**Decision recorded (2026-07-03)**: Finding 1 is resolved by returning the engine to **48 kHz** (the rate `architecture.md` §5.1 was written for), *not* by building a tick divider for 44.1 kHz. Consequence: 44.1 kHz WAV content must be rate-converted — see Finding 1's "Decision & consequences" for exactly what already handles this and what doesn't.

---

## High severity

### 1. The "1 kHz control tick" actually runs at 918.75 Hz

- `timebase.hpp:4-7`: `kAudioRate = 44100`, `kBlockSize = 48`, and `Tick1kHz()` fires once per audio block → block period is 48/44100 = **1.088 ms**, so the tick runs at 918.75 Hz.
- `main.cpp` sets `SAI_44KHZ` (which only exists via the local libDaisy 44.1 kHz patch), while `architecture.md` §5.1 declares the "1-block = 1-ms identity" a design invariant and labels the section *as-built* at 48 kHz — the doc's as-built claim was wrong.
- Nothing consumes the tick yet (the callback's `Tick1kHz` body is empty), but everything planned on top of it inherits the −8.1% error: CV updates "at the 1 kHz tick" (Phase 1 item 5) and, critically, the Phase 2 sequencer — which would run at ~110.2 BPM when set to 120.

**Decision & consequences (48 kHz):**

- Switch `main.cpp` to `SAI_48KHZ` and `Timebase::kAudioRate` to 48000. The local libDaisy 44.1 kHz SAI patch becomes unnecessary for the engine (keep it in the fork; it's harmless and may be useful for I/O experiments).
- **Audition/streaming path: already handled.** `PumpWavIO` resamples whenever `s_wav.sample_rate != s_sample_rate` (`LinearResampleFrames`), so 44.1 kHz WAVs audition correctly on a 48 kHz engine today — subject to Finding 6's silent-fallback bug, which must be fixed for this to be trustworthy.
- **RAM-resident sample path: not handled.** `OnSampleLoad` copies raw WAV bytes into SDRAM without rate conversion, and `VoiceManager` plays them back assuming engine rate — a 44.1 kHz sample on a 48 kHz engine would play ~8.8% sharp/fast. Two implementation options:
  1. **Playback-rate compensation (cheap, recommended first)**: `VoiceManager` already does fractional-phase linear interpolation; scale `Voice::increment` by `file_rate / engine_rate` (e.g. ×0.919 for 44.1k content) at `Trigger()` time. Zero extra memory, zero load-time cost, quality identical to the linear interpolation already in use. Requires plumbing the sample's native rate (already stored in `LoadedSampleInfo`) into `VoiceTriggerParams`.
  2. **Resample-on-load**: convert to 48 kHz once at load time (offline, main-loop chunked — fits the offline-DSP rule). Costs ~8.8% more SDRAM per 44.1k sample and load-time CPU, but makes all in-RAM data uniform, which simplifies Phase 4 offline editing. Reasonable as a later upgrade; not needed to unblock the rate switch.

### 2. Daisy→ESP32 frames ≥ ~1990 bytes can never be transmitted — and the retry storm starves audio

- At 2 Mbaud (`pin_config.h:129`), one byte ≈ 5 µs. A max frame (2048 payload + 10 overhead = 2058 B, `uart_protocol.h:15-16`) needs **10.29 ms of wire time**; `daisy_uart_link.cpp:390` calls `BlockingTransmit(frame, len, 10)` — a 10 ms cap.
- libDaisy's `BlockingTransmit` (`per/uart.cpp:668`) is a straight `HAL_UART_Transmit`, whose timeout covers the whole transfer: a ≥ ~1990-byte frame *deterministically* times out mid-frame. ~1990 bytes of a truncated frame reach the wire (CRC/sync garbage at the ESP32), the sender marks TX failed **without advancing the queue**, and retries the entire frame every main-loop pass — each attempt blocking ~10 ms — until the 1-second stuck-TX force-clear drops the message.
- During that ~1 s the main loop is mostly blocked, `PumpWavIO()` starves, and the ring buffer (2047 frames ≈ 46 ms at 44.1 kHz / 42 ms at 48 kHz) underruns.
- Current senders sit just under the cliff: browse responses at 20 entries/page ≈ 1315 B (6.6 ms); the single-frame wave-chunk path (`audio_engine.cpp:133`, 900 samples → 1816 B frame) is **9.1 ms — only 0.9 ms of margin**.
- Fix: cap payloads so wire time ≪ timeout (≤ ~1500 B), or derive the timeout from frame length (`len × 10 / baud + margin`). Long-term the honest fix is DMA TX, because a wire-time-honest timeout for max frames (11 ms) violates the ≤10 ms main-loop blocking rule (§7.1.4) — the three constraints (2 Mbaud, 2048-byte payloads, 10 ms max block) are mutually inconsistent at the boundary.

### 3. UART interrupts run at NVIC priority 0 — above audio

- libDaisy hardcodes both halves of the UART RX chain to maximum priority: `HAL_UART_MspInit` sets UART4_IRQn to (0,0) (`per/uart.cpp:896`); `dsy_dma_init` sets DMA1_Stream5 (UART4 RX DMA) to (0,0) (`sys/dma.c:32`).
- `main.cpp:206-215` overrides the SAI streams to 5/6 and SPI to 10 — but never touches the UART pair, and `UartLinkInit` (line 369) runs *after* those overrides, so the (0,0) assignments stand. Exactly the inversion `architecture.md` §7.1.5 forbids ("audio SAI DMA highest").
- The RX callback (`append_rx_data_isr`, `daisy_uart_link.cpp:65-95`) does up to a ~4 KB memmove + 2 KB memcpy in that priority-0 ISR, preempting the audio callback. Modest jitter at today's traffic levels; will matter when Phase 1 item 8 puts frequent MIDI note traffic on this path.
- Fix: two `HAL_NVIC_SetPriority` calls (UART4_IRQn, DMA1_Stream5_IRQn → ~7) after `UartLinkInit`.

## Medium severity

### 4. Whole-frame CRC computed with all interrupts disabled — under a lock that protects nothing

`UartLinkSend` (`daisy_uart_link.cpp:617`) holds `ScopedIrqBlocker` (a global `__disable_irq()` — masks audio too) across `CreateUartPacket`: a ≤2 KB memcpy plus bitwise software CRC16 over ≤2058 bytes ≈ 60–130 µs all-IRQs-off per large send. Every `UartLinkSend` call site runs on the main loop, and the RX ISR never touches TX-queue state — the lock guards no actual concurrency. Build the frame outside the lock (or remove it with a comment stating the single-context invariant).

### 5. Preview chunks silently dropped when the 4-deep TX queue fills

`SendPreviewChunks` (`audio_engine.cpp:157-183`) queues all chunks in a tight loop, ignores `UartLinkSend()` returning −1 on queue-full, and advances `s_prev_sent` regardless — anything past ~4 chunks is lost, leaving gaps in the waveform preview. It runs on the main loop, so it can pump `UartLinkProcess()` between sends or spread sends across iterations.

### 6. Resampling failure path plays audio at the wrong pitch

`PumpWavIO` (`audio_engine.cpp:1807-1828`): if `AcquireScratch` for the resample buffer fails, the **unresampled** frames are pushed — a pitch glitch instead of a skipped pass. Separately, at the upsampling boundary `final_frames` is truncated to `free_frames` while `slot.consumed` advances by the full input count — 1–2 frames silently dropped (potential click). Both should reduce/retry instead of degrading silently. Note this bug gains importance under the 48 kHz decision: the resample path becomes the *normal* path for all 44.1 kHz content.

### 7. CRC-storm recovery window is per-loop-iteration, not per-second

`daisy_uart_link.cpp:678-695`: the ">10 CRC errors" delta is measured between consecutive `UartLinkProcess()` calls (sub-millisecond apart) although the log claims "in 1 sec". The DMA-listener reset only fires if 11+ corrupt frames appear in a single buffer scan — plausible for reboot garbage, but slow-drip corruption never triggers recovery. Make the window time-based.

### 8. ~8.3 KB of the 32 KB DMA RAM wasted on a queue that never touches DMA

`s_tx_queue` (`daisy_uart_link.cpp:54`) sits in `DMA_BUFFER_MEM_SECTION`, but transmission is CPU-polled `BlockingTransmit` — no DMA. That region is the scarcest memory in the system: 28204/32768 B used (86%), and libDaisy's MPU non-cacheable window is exactly 32 KB (`sys/system.cpp:526-527`) so the region cannot grow. Moving the queue to regular RAM frees ~8.3 KB for future DMA-TX/SPI buffers. (The linker region is capped at 32 KB, so overflow is a link error, not silent cacheable-DMA corruption.)

## Lower severity

9. **ESP32 `uart_task` never drains the driver ring when idle** (`esp_uart_link.cpp:278-289`): the queue-timeout branch calls `process_rx_frames()` but never `uart_read_bytes` — bytes left in the driver ring (reads are capped at 256 B/event, lines 24/245) sit until *new* data arrives, stalling partial frames. `uart_wait_tx_done`'s result is also ignored. Drain with a zero-timeout read in the idle branch; loop reads per event.
10. **Overflow handling matches a historical IDF landmine**: `UART_BUFFER_FULL → uart_flush_input` (lines 257-263) is the trigger pattern of ESP-IDF issue #8445 (flush-on-full permanently broke RX until reboot; fixed before IDF 5.5 — one hardware check recommended). The 2048 B RX ring is also only ~10 ms of margin at 2 Mbaud while the same task can block feeding the TX ring; bump the RX ring to 4–8 KB (cheap on the P4).
11. **Cross-core UI handoff has no barriers**: `browse_resp_callback` correctly defers LVGL work (§7.2.4 respected), but writes `browser->entries[]` then a plain non-atomic `ui_update_pending` flag read by the UI task. On the dual-core P4, ordering isn't guaranteed — a torn list could momentarily render. Make the flag a release/acquire atomic.
12. **Doc/code drift**: `OnSampleLoad` DMAs SD reads directly into SDRAM, while §7.1.2 says to stage through internal RAM. Load-time-only and cache-safe (libDaisy clean+invalidate), but §7.1.2 should note the exception.

## Checked and found sound

- **SPSC ring buffer** (`audio_engine.cpp:687-751`): `__DMB()` is both a hardware and compiler barrier; free-running counters with power-of-2 capacity handle wrap; both push sites respect the one-slot-free discipline, so the masked count cannot hide a full buffer.
- **SD DMA cache coherency**: libDaisy enables `ENABLE_SD_DMA_CACHE_MAINTENANCE` (`sd_diskio.c:53`) with clean-before + invalidate-after; the clean-before makes FatFs' misaligned partial-sector paths safe, and `SdBufferSlot.data` is `alignas(32)` with multiple-of-32 size, so slot metadata never shares an invalidated cache line.
- **`DMA_BUFFER_MEM_SECTION` is genuinely non-cacheable** (MPU region at 0x30000000 matches the 32 KB linker region exactly).
- **SAI DMA priorities**: `main.cpp`'s overrides land after libDaisy's blanket priority-0 DMA init, so audio really runs at priority 5 — the inversion is confined to the UART pair (Finding 3).
- **ESP32 `uart_write_bytes`** returns after copying into the TX ring (blocks only while the ring is full) — confirmed against the ESP-IDF documentation.

## Sources

- Vendored libDaisy v8.1.0 sources (authoritative for driver behavior): `firmware/daisy/libs/libDaisy/src/per/uart.cpp`, `sys/dma.c`, `util/sd_diskio.c`, `sys/system.cpp`.
- [ESP-IDF UART driver documentation](https://docs.espressif.com/projects/esp-idf/en/v3.3.3/api-reference/peripherals/uart.html)
- [ESP-IDF uart_events example](https://github.com/espressif/esp-idf/blob/master/examples/peripherals/uart/uart_events/main/uart_events_example_main.c)
- [ESP-IDF issue #8445 — flush with full buffer breaks RX](https://github.com/espressif/esp-idf/issues/8445)
