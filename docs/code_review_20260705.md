# WaveX Code Review — 2026-07-05

**Scope**: full first-party firmware tree (`firmware/shared`, `firmware/daisy/src`, `firmware/esp32/main`, `firmware/esp32/components/ui`), configuration, build system, tests, and docs — reviewed against `docs/architecture.md`, `docs/roadmap.md`, and the AGENTS.md engineering constraints.
**Method**: Graphify dependency-graph analysis (regenerated at review time: 64k nodes / 158k edges over 8,393 files; first-party hub/fan-in analysis), file-by-file manual review of all ~24k first-party lines, targeted `grep` verification of every "is this actually called?" claim, and a run of all three host test suites.
**Verification status**: host tests run during this review and green — shared 79/79, Daisy 76/76 (+4 disabled), ESP32 79/79. Device builds were *not* run here (devcontainer required); nothing below depends on a device build.

---

## 1. Executive summary

The codebase is two strata with very different quality. The **2026-06/07 vintage code** — `voice_manager.hpp`, `envelope.hpp`, `midi_stream_parser.hpp`, `sequence_tracker.hpp`, `attn_watchdog.hpp`, the CV router/backend seam, the output-sink seam, and the associated host tests — is genuinely good: HAL-free, host-testable, correctly reasoned about concurrency, and well-commented about *why*. The **older scaffolding** underneath it (protocol layer, both UART links, message dispatchers, `audio_engine.cpp`, config headers) carries substantial debt: dead legacy DSP surfaces, unconditional debug logging on hot paths, duplicated-and-divergent implementations, config files that contradict themselves, and several real latent bugs.

The single most important finding: **the flagship Phase-1 MIDI note path is broken at its final hop**. The Daisy's UART message dispatcher never routes `MSG_NOTE_ON`/`MSG_NOTE_OFF` to the audio engine — the handlers are log-only stubs, and the adapter that would wire them (`audio_adapter.cpp`) is dead code with zero callers. Roadmap item 8 is marked "Done (code-complete)" and the CHANGELOG says notes "now play loaded samples," but no note arriving over the wire can reach the voice manager. Host tests pass because they test units, not the dispatch glue. This is the third instance of a roadmap/doc claim not matching code (after the "dead UART link" and "SPI-SD" corrections recorded in roadmap 0.2) — §7 proposes a structural fix (dispatch-level tests), not just a patch.

Second theme: **the transport story is documented backwards**. `architecture.md` §4.4 calls SPI the active link and UART legacy; in reality `WAVEX_SPI_LINK_ENABLED` is hard-coded to `0` in `link_config.h` ("TEMPORARILY DISABLED"), all SPI link code (including the item-7 robustness classes) is compiled out of every build, and UART is the only live transport — which is also the only transport *without* duplicate/reboot sequence protection.

Third theme: **memory waste on the Daisy**. A bitmap sizing error in `memory.h` (~190 KB) plus a dead 64 KB conversion buffer waste roughly a quarter of the STM32H750's internal RAM budget in static BSS.

None of this is architectural rot — the target architecture (§5.3 seams, HAL-free cores, wire-contract centralization) is sound and the recent work follows it. The debt is concentrated, enumerable, and mostly deletable.

---

## 2. Critical — functional breaks (fix before any new Phase-1/2 work)

### C1. MIDI note path: dispatcher never calls the audio engine

- `firmware/daisy/src/comm/daisy_inter_mcu_message_handlers.cpp:214-222` — `HandleNoteMessage` / `HandleNoteOffMessage` are stubs that only `PrintLine`. `HandleControlChangeMessage` (:208) and `HandleSampleControlMessage` (:257) likewise.
- `firmware/daisy/src/audio/audio_adapter.cpp` — the adapter that forwards to `AudioEngine::OnNoteOn/OnNoteOff/OnControlChange/OnSampleCtrl` has **zero callers anywhere in the tree** (verified by grep; only its own header includes it).
- Commit `cb9c2f7` ("wire voice manager into audio callback via MIDI note path") changed only `audio_engine.cpp`/`voice_manager.hpp` — it built the engine side of the bridge and never touched the dispatcher. The ESP32 side (`midi_task.cpp` → `inter_mcu_send_note_on` → UART) is complete and correct.

**Effect**: a MIDI keyboard plugged into either DIN or USB produces log lines on the Daisy console and no sound, ever. Everything downstream (SPSC note queue, voice manager, stealing, pitch compensation) is implemented and host-tested but unreachable.

**Fix**: route the four stubs through `AudioAdapter` (or call `AudioEngine::On*` directly and delete the adapter), guarded by `WAVEX_AUDIO_ENGINE_ENABLED`. Then add the dispatch-level test described in §7 so this class of break can't recur silently. Update CHANGELOG/roadmap claims.

### C2. The legacy synth-DSP surface is entirely inert (and lies about it)

In `audio_engine.cpp`, `s_oscillator`, `s_filter` (SVF), `s_envelope` (ADSR), `s_lfo`, `s_params`, `s_envelope_gate`, `s_env_level` are initialized (:888-907) and written by `OnControlChange` (:1061-1098) — but **`Callback()` never renders any of them**. Verified by grep: no `Process()` call on any of these objects exists. Consequences:

- Every `MSG_CONTROL_CHANGE` parameter (volume, cutoff, resonance, ADSR, LFO) is audibly a no-op. `PARAM_VOLUME` isn't applied anywhere in the output path.
- The "test oscillator fallback" in `OnNoteOn` (:1123-1136, "so the MIDI path stays verifiable on hardware with an empty SD card") sets oscillator frequency and gate on objects that are never mixed — the fallback is silent. The comment (and roadmap item 8's "24-bit falls back to the test oscillator") is wrong.
- `Sampler` (`sampler.hpp`) is fully inert: `FeedInputBlock` has no callers (recording captures nothing — known, roadmap item 3 note) **and** `Next()` has no callers either, so `SAMPLE_PLAY_START` plays nothing. On top of that, its control path was already dead at the dispatcher (C1's `HandleSampleControlMessage` stub). The record/play UI feature is a triple-decker of disconnection.
- `GetInputMeters` (:1625) reads `s_last_in_block`/`s_last_block_size`, which nothing writes (audio input is `(void)in`) — the record page's input meters can only ever show zero.
- `OnSampleData` (:1547) is unreachable: `s_sample_load.loading` is never set `true` anywhere, so `MSG_SAMPLE_DATA` is a dead wire message.

**Fix**: decide per-surface — delete (oscillator/SVF/ADSR/LFO/`OnControlChange` mapping, until Phase-2 defines real parameter routing), or wire (sampler input feed + render, if record/play is wanted before Phase 4). Either is fine; the current state — control surfaces that accept input and do nothing — is the worst option, and it also skews the CPU-load and RAM numbers everything else is planned around.

### C3. `inter_mcu.cpp` inverts every send-error result

`send_uart_message` returns `-1` on failure / `len > 0` on success, and every wrapper does `return result ? ESP_OK : -1;` — e.g. `inter_mcu_send_note_on` (`firmware/esp32/main/inter_mcu.cpp:127-128`), and identically for note-off, control-change, sample-ctrl, preview-req, browse-req (:557), status-request. Since `-1` is truthy, **every send failure is reported as `ESP_OK`**, and only the impossible zero-length success would report failure.

**Effect**: all upstream error handling is dead — e.g. `midi_task.cpp:30-45`'s "note-on dropped (link send failed)" warnings can never fire; the UI can't distinguish a wedged link from success. Fix: `return result >= 0 ? ESP_OK : ESP_FAIL;` in all wrappers (one-line each), then re-check callers that relied on the broken contract.

### C4. Transport reality vs. documentation (SPI is off; UART is load-bearing and unprotected)

- `firmware/shared/config/link_config.h:18`: `#define WAVEX_SPI_LINK_ENABLED 0  // TEMPORARILY DISABLED for UART-only testing` — not `#ifndef`-guarded, so no build can enable it without editing the file. All of `daisy_spi_link.cpp` (1,246 lines), `esp_spi_link.cpp` (870 lines), and the Phase-1-item-7 `SequenceTracker`/`AttnWatchdog` integrations are compiled out of every image.
- `architecture.md` §4.4 states the opposite ("UART link code is legacy — the design is SPI-only") and §2/§3.1 describe the SPI link as "working". Roadmap 0.2.1 already corrected this ("UART carries heartbeat, meter, status…SPI reserved for browse/wave") — but even that is stale: **browse responses and wave chunks are sent over UART today** (`daisy_filesystem.cpp:443`, `audio_engine.cpp:200/248`). UART is currently the *only* transport, for everything.
- The consequence chain: the live transport has **no sequence-number validation at all** (both `daisy_uart_link.cpp` and `esp_uart_link.cpp` parse `seq` and use it only for logging), so `SequenceTracker`'s duplicate/reboot protection — built precisely because this bit the project before — protects only dead code. ACK/NACK flags exist in the frame format but have no retransmit semantics anywhere (received ACKs are logged and dropped).

**Fix**: make a decision and write it down in `architecture.md` — either (a) UART is the transport of record until the SPI link is re-validated on hardware (then: wire `SequenceTracker` into both UART RX paths — it's transport-agnostic and host-tested — and delete or clearly quarantine the SPI code), or (b) SPI is coming back imminently (then: track re-enablement as a roadmap item with a hardware gate). Today's state — a doc pointing at compiled-out code while the real link lacks the protections built for it — is the most confusing possible configuration for the next contributor.

---

## 3. High severity

### H1. ~254 KB of Daisy internal RAM wasted in static BSS

- `memory.h:68`: each `SlabPage` bitmap is `uint32_t bm[WXM_SMALL_PAGE_BYTES / 32u]` = 128 words = 512 B. The worst-case need is 4 words (smallest class 32 B → 4096/32 = 128 slots → 128 bits). The "safe cap" is 32× oversized: 6 classes × 64 pages × ~500 wasted bytes ≈ **190 KB** of BSS for the `SampleMemMgr` *bookkeeping object* (the managed memory itself is SDRAM; the manager lives in internal RAM). Correct cap: `WXM_SMALL_PAGE_BYTES / 32u /*min block*/ / 32u /*bits per word*/` = 4.
- `audio_engine.cpp:421-422`: `s_conversion_buffer` (4096 × 8 ch × 2 B = **64 KB**) is declared and never referenced — the scratch-pool refactor obsoleted it.

On an STM32H750 (128 KB DTCM + 512 KB AXI + 288 KB D2 + 64 KB D3), this is roughly a quarter of all internal RAM held by two declarations. It currently links, but it forecloses future buffers (streamed-voice slots, render-job scratch) that Phase 1/4 will need. Both fixes are mechanical; re-run `make daisy` and check the map file delta.

### H2. Daisy UART RX can wedge permanently on garbage without a start byte

`daisy_uart_link.cpp:147-171` (`process_rx_frames`): when `FindFrameStart` finds no `0xA5` in the buffered data, the code consumes only `offset` bytes — which is 0 on a fresh call — and returns. A frame buffer full of start-byte-free garbage therefore never drains; incoming bytes are then discarded at `pull_pending_into_frame_buffer` (buffer full), and the "frame buffer stuck" recovery (:741) can never trigger because it requires `s_frame_len == 0`. The CRC-storm recovery doesn't help either (this path increments no counter). The ESP32 twin **already has the fix** — `esp_uart_link.cpp:150-153` consumes everything except the last `UART_FRAME_OVERHEAD - 1` bytes. Port that guard to the Daisy side. (Reachable in practice via baud glitches or a peer reset spraying garbage — exactly the scenario the recovery machinery exists for.)

### H3. `PacketRouter::route_by_message_type` trusts wire length (nullptr deref / garbage reads)

`firmware/esp32/main/comm/packet_router.cpp:115-145`: every case does `memcpy(&msg, payload, sizeof(msg))` without checking `payload_len >= sizeof(msg)`. `route_uart_message` passes `payload = nullptr` when `payload_len == 0` (`esp_uart_link.cpp:191-192`), so a CRC-valid frame with an empty payload and type `MSG_HEARTBEAT` (etc.) is a `memcpy(dst, nullptr, n)` — undefined behavior, realistically a crash. Short-but-nonzero payloads read stale stack garbage into typed messages (bounded by the caller's 2 KB buffer, so not memory-unsafe, but silently wrong). One `if (payload_len < sizeof(msg)) { warn; break; }` per case — or a small template helper — closes it. The Daisy dispatcher has the same pattern but does validate sizes on the handlers that matter; make both sides uniform.

### H4. Latent buffer overflow in `CreateWaveXPacket` for payloads 2043–2048 B

`protocol.cpp:38-54`: `GetOptimalSizeCode` returns `PKT_SIZE_2048` for *any* oversize payload instead of failing. `CreateWaveXPacket` (:109-143) then only checks `total_size > buffer_size`: for `payload_size` in 2043–2048 it memcpys past the CRC region into the caller's buffer, and `memset(buffer + 4 + payload_size, 0, total_size - 4 - payload_size - 2)` **underflows `size_t`** → multi-GB memset → hard fault. Today's callers stay under the line only by accident of their own checks (`CreateBrowseRespPacket`/`CreateWaveChunkPacket` guard against 2048, not the real 2042 limit — a 2043–2048 B payload passes their guard and hits the overflow). Fix: make `GetOptimalSizeCode` fail (return an invalid code) above 2042, validate in `CreateWaveXPacket` (`payload_size > total_size - 6 → return 0`), and fix the two `> sizeof(temp_payload)` guards to `> 2042`. Add a round-trip test at the boundary.

### H5. `ParseUartPacket` has no output-capacity contract

`uart_protocol.cpp:113-146` copies up to `UART_MAX_PAYLOAD` (2048) bytes into `payload_out` with no capacity parameter — the exact bug class fixed in `ParseWaveXPacket` (the in/out `payload_size` clamp, documented at `protocol.cpp:172-178`). All current callers happen to pass 2048-byte buffers, so it's latent, but the API invites the next caller to overflow. Mirror the SPI-side fix (make `payload_size` in/out), since the header even cites that fix's review finding.

### H6. Browse path: stack pressure, capacity mismatches, and a forked wire format

- **Stack**: `ProcessBrowseRequest` (`daisy_filesystem.cpp:316-447`) puts `FileEntry entries[50]` + `FileEntry all_entries[50]` + `FileEntryWire wire_entries[50]` + `uint8_t browse_payload[2048]` ≈ **11 KB** of locals on the stack, and calls `ListDir` (`fs_browse.cpp:69`) which adds `FileEntry all_entries[256]` ≈ **14 KB** more. ~25 KB transient stack in one call chain, on the same stack the audio IRQ interrupts. Verify against the linker's stack budget; regardless, these should be static (the call path is main-loop-only and non-reentrant).
- **Latent overflow**: `browse_payload` is 2048 B but 50 entries × 65 B + 5 = 3,255 B. Only the hardcoded `max_entries = 20` (`daisy_inter_mcu_message_handlers.cpp:373`) keeps `entries_written ≤ 20` (1,305 B). Anyone raising the constant past 31 corrupts the stack. Add a static_assert tying the entry cap to the payload capacity.
- **Capacity mismatches vs. the roadmap's "browse a 500-entry directory" target**: wire `start_index` is sent as one byte (`inter_mcu.cpp:532` — max 255), `ListDir` silently truncates directories at 256 entries, and the play-by-index cache keeps only the first 50 (`s_current_file_entries[50]`), so **selecting any file past index 49 in a large directory fails**. These limits are mutually inconsistent and none is documented in the protocol doc.
- **Forked wire format**: the live `MSG_BROWSE_REQ` format is `[start_index u8][path][NUL]` (built at `inter_mcu.cpp:527-534`, parsed at `daisy_inter_mcu_message_handlers.cpp:356-373`), while `ProtocolHandler::ParseBrowseReq` (`protocol.cpp:407-438`) documents and parses a *different* format (path-first, u32 start_index, u8 max_entries) — and has zero callers plus a bogus hardcoded `buffer + 1024` bounds check. Delete `ParseBrowseReq`/`ParseSamplePlayReq` or make them the real format; the wire contract must not exist in two disagreeing copies inside `protocol.{h,cpp}` itself (AGENTS.md constraint 4).

### H7. `OnSampleLoad` stalls the Daisy main loop for the whole load

`audio_engine.cpp:1273-1545` reads the entire WAV synchronously in the message-handler context: 1 KB `f_read` chunks (`s_sample_io`) + memcpy to SDRAM, plus a fixed `System::Delay(10)`. A 20 MB sample at realistic SDMMC throughput stalls the main loop for seconds: no `UartLinkProcess()`, no heartbeats (the ESP32 UI will show the backend dead), no TX pumping, and the ESP32→Daisy RX ring (2 KB DMA buffer + 8 KB pending) can overflow. Audio is safe (streaming stopped, voices hard-stopped) but the *link* contract (§7.1.4 spirit) is violated. Acceptable short-term for user-initiated loads of drum hits; must become a chunked main-loop job (the Phase-4 render-job scheduler shape) before large-sample workflows land. Two cheap immediate improvements: raise the I/O chunk from 1 KB to 8 KB (matches the streaming path), and send a failure `SampleStatusMessage` on the error paths (currently only success is reported — the UI waits forever on a failed load).

### H8. `hw.StartLog(true)` — potential boot-block on standalone hardware

`firmware/daisy/src/main.cpp:124`: in libDaisy, `StartLog(true)` blocks until a USB-CDC host connects. If that's accurate for the pinned libDaisy version, a WaveX unit not attached to a PC never boots its audio engine. This may be masked in bench bring-up (always tethered). Verify on hardware; if confirmed, use `StartLog(false)` (or gate on a build flag) for anything that might run standalone.

---

## 4. Medium severity

### M1. `pin_config.h` contradicts itself (and it's the single source of truth)

- **GPIO46/47 double-assigned**: `WAVEX_ESP_PCNT1_A/B 46/47` (:63-64) vs `WAVEX_ESP_SPI2_SCLK 46 / WAVEX_ESP_SPI2_MOSI 47` (:96-99). PCNT1 is enabled by default; SPI2 is the planned TLC5947/MCP3008 bus (first real consumer in Phase 2). This collision must be resolved before LED bring-up.
- **Daisy D15 triple-claimed**: `WAVEX_DAISY_CTRL_1 15` (pin_config.h:159), `WAVEX_DAISY_SD_CARD_DETECT_PIN 15` (hardware_config.h:108, active by default), `WAVEX_DAISY_LOOP_PROBE_PIN 15` (hardware_config.h:153, disabled). Card-detect actively reads D15 at boot (`sd_sdio.cpp:23-33`).
- **"Available Pins" table is wrong**: (:164-174) lists D0/D7/D8 as available; D0 is ATTN_IN, D7 is SPI CS, D8 is SPI SCK per the same file.
- **Stale header claims**: "ESP32-S3 Frontend Pin Assignments" / "VERIFIED for ESP32-S3-DevKitC-1" (:12, :21) on a P4 board; `WAVEX_ESP_ENCODER_BTN 40` and `TOUCH_RST/INT 14/15` are outside the file's own stated valid-GPIO list (:27-28); "SD Card Interface (SPI)" pins D17–D20 (:152-156) survive the deleted SPI-SD backend.
- `WAVEX_ESP_SPI_CLK_HZ 4000000  // 10 MHz` (:183) — comment/value disagree.

### M2. `hardware_config.h` dependency guard is toothless

The `#error` check (:620-625) tests `WAVEX_DAC_CV_OUTPUTS_ENABLED`, `WAVEX_ENCODER_PCNT_ENABLED`, `WAVEX_PCNT1_ENABLED`, `WAVEX_4067_MUX_ENABLED`, `WAVEX_TCA8418_BUTTON_MATRIX_ENABLED`, `WAVEX_USB_MIDI_ENABLED` — none of which exist (real names differ, e.g. `WAVEX_ESP_ENCODER_PCNT_ENABLED`). Undefined macros are `0` in `#if`, so the guard silently checks only audio-engine and LCD. Also: the SD-speed doc comment contradicts itself (20/40 MHz vs. 12.5/25 MHz, :117-135), and `DISABLE_SD_SPI_BACKEND 0` (:97-99) is dead and inverted-sounding.

### M3. `link_config.h` dangling references and copy-paste

`PIN_IRQ_DAISY2ESP → WAVEX_ESP_DAISY_IRQ` (:31) and `SPI_POOL_SIZE → WAVEX_SPI_POOL_SIZE` (:55) reference macros defined nowhere — any code using them fails to compile, proving they're dead. The Daisy fallback block maps `PIN_SPI_MOSI` to `WAVEX_DAISY_SPI_MISO` (:41) — a copy-paste landmine. `SPI_CLOCK_SPEED_HZ 10000000 // 8 MHz` (:47) — third comment/value mismatch in the config set.

### M4. Sequence numbers wrap through the reserved 0

All three generators (`protocol.cpp:11/102`, `daisy_uart_link.cpp:66/656`, `esp_uart_link.cpp:68/476`) do bare `s_next_sequence++` on a `uint16_t`; after 65,535 packets the next seq is 0, which `SequenceTracker::Evaluate` (and the protocol comment "0 is reserved") rejects. On the UART path today nothing validates seq so nothing breaks — but the moment `SequenceTracker` is wired in (C4's fix), one packet per 64 K would be dropped. Skip 0 at the generators (`if (++s == 0) s = 1;`) now, while it's free.

### M5. Unconditional logging on hot paths (Daisy)

The Daisy prints, unconditionally (not behind any `WAVEX_*` log gate), on **every received frame** (`daisy_uart_link.cpp:216-218`), on **every dispatched message plus an up-to-8-byte hex dump** (`daisy_inter_mcu_message_handlers.cpp:52-121` — 70 lines of switch just to format the dump), per second from `UartLinkProcess` (:760-769), per second from the main loop (`main.cpp:503-509`, including a bare `printf`), per `ListDir` call (`fs_browse.cpp:43-150` `printf`s), and 10+ numbered lines per sample load ("[1/10]"…). USB-CDC logging costs main-loop time exactly where H7 already hurts, and it drowns real diagnostics. Route everything through the existing `logging_config.h`/`UART_LOG*` gates and default the chatty ones off. (The ESP32 side is mostly disciplined — `UART_LOGI` is compiled out at the default level — but `packet_router.cpp:41` logs per-packet at INFO, which includes every 50 ms meter push.)

### M6. Daisy TX failure semantics: 1 s head-block, then silent drop

`daisy_uart_link.cpp:250-470`: on a `BlockingTransmit` failure, `s_tx_inflight` stays set; the next `process_tx_queue` calls see "in flight" and do nothing until the 1 s force-clear drops the frame. One transient TX error therefore stalls *all* outbound traffic (heartbeats included) for a second and never retries the transmit. Clearing `s_tx_inflight` on failure (retry next pass, bounded by the existing 1 s give-up) matches the intent of the surrounding comments. Also note the 30 lines of "validate the frame we ourselves just built" dead defensive code (:344-374).

### M7. Preview pipeline: unbounded heap + per-chunk vector churn

`OnPreviewReq` (`audio_engine.cpp:1237`) does `s_preview.reserve((end-start)/decim + 1)` with wire-controlled `end`/`decim` — a request with `decim=1` over a long sample asks for megabytes of newlib heap; with exceptions disabled, a failed `operator new` terminates the firmware. Clamp preview length server-side (the UI never needs more than a few thousand points). `SendPreviewChunks` (:186-270) also allocates a fresh `std::vector` per chunk — main-loop-legal but needless churn; a static 2 KB staging buffer suffices.

### M8. `memory.h` correctness/robustness gaps (beyond H1)

- Stats lie: `in_use_bytes`/`objects_alive`/`failed_allocs` track only the small-slab pool; large-extent allocations (i.e., *samples* — the dominant user) are invisible in `in_use_bytes`, and large-pool alloc failures don't bump `failed_allocs` (`memory.h:290-304`, `fill_small_stats`). The UI's sample-memory page shows misleading numbers.
- `LargeExtentPool::release` doesn't detect double-free (inserts the run again → overlapping future allocations); the small pool does detect it. `insert_run` silently leaks pages when the run table is full.
- `retain()`/`release()` refcounts live *inside* the copied-by-value handle (`wxsamp_t.refcnt`), so two copies of a handle each think they own the last reference — broken sharing semantics. Nothing uses `retain()` yet; either remove it or move refcounts into the manager before Phase-2 kits share samples across pads.
- Header says `FILE: include/wxsamp_mem.h` (:3) — stale.

### M9. ESP32 event-dispatch consolidation is incomplete

Roadmap 0.2.4 ("one event-dispatch owner… `PacketRouter` owns fan-out") is marked done, but `StatisticsManager` (`comm/statistics.h`) still owns the browse-response, sample-status, and meter callbacks (plus their mutexes), and `inter_mcu.cpp` still owns the wave-chunk listener and the sample-mem cache. So subscription is split across three components while routing lives in a fourth. It works, but the name `StatisticsManager` actively misleads — the next contributor will look in `PacketRouter` for listener registration and not find it. Either move subscriptions into `PacketRouter` or rename/split (`BackendEvents` + `LinkStatistics`).

### M10. Protocol-layer type debt

- `WaveXPacket` (`protocol.h:20-26`) is misleading and unused: `crc` sits at struct offset 4 while the wire puts CRC at the end; `payload[0]` is a GNU extension. Delete it or make it a doc comment.
- The size table exists three times (`PKT_GET_SIZE` macro, `GetPacketSizeFromCode`, `GetOptimalSizeCode` comments) — one inline function should own it.
- `MAX_PAYLOAD_SIZE = 220` (`protocol.h:14`) contradicts the real 2,042-byte limit and is used only by the disabled `esp_spi_link.cpp` (:268, :708), whose payload buffer it caps — meaning the SPI link, if ever re-enabled, can't carry the 1.3 KB browse pages that motivated the "SPI for browse/wave" split. Rename/resize before any SPI revival.
- `spi_protocol.h` is a **third, competing framing** (`pkt_hdr_t`/`pkt_t`, own CRC, own `ring_t` "lock-free" ring with no memory barriers) — included in three files, with none of its types referenced anywhere (verified). It predates `protocol.h` and violates the "never hand-roll a competing message format" rule by existing. Delete.
- Unsynchronized `static s_next_seq_num` (`protocol.cpp:11`) is racy if two FreeRTOS tasks ever create packets concurrently (today all sends funnel through `uart_link_send`'s mutex, but the protocol layer shouldn't depend on that).

### M11. Daisy `main.cpp` dead scaffolding

`measure_cpu_baseline()` burns 100 ms at boot to compute `s_cpu_baseline_ticks_per_second`, which nothing reads; `busy_start_ticks` (:424), `spi_duration` (:443), `wav_path`, `last_sync`, `last_tx_pump` are computed/declared and never used; the CPU-usage variable block (:42-49) is superseded by `CpuLoadMeter`. The boot banner logs a hardcoded "Revision 0x20036450 // STM32H7B3" — wrong chip. All deletable.

### M12. WAV parsing duplicated and non-conforming

`OpenWav` (`audio_engine.cpp:1670-1793`) and `OnSampleLoad` (:1374-1424) carry two hand-rolled copies of the same RIFF chunk walk; `ParseWavMetadata` (`daisy_filesystem.cpp:107+`) is a third. None pads odd-sized chunks to word boundaries (RIFF requires it), so a WAV with an odd-length `LIST`/`INFO` chunk before `data` mis-parses on all three. Extract one shared, host-testable parser (same pattern as `midi_stream_parser.hpp`) and fix padding once.

### M13. Miscellaneous concurrency nits

- `audio_engine.cpp` `s_last_block_meters` (4 floats) is written in the callback and read from the main loop with no synchronization — meter values can tear (cosmetic, but trivially fixed with the release/acquire pattern already used for the note queue).
- `rb_pop_stereo` executes ~4 `__DMB()` barriers per *sample* (≈192/block plus per-sample function call); a batched block-pop with one barrier pair would reclaim measurable callback headroom. Measure with DWT per AGENTS.md before/after.
- `uart_link_stop` (`esp_uart_link.cpp:500-523`) deletes the event queue/mutex 20 ms after clearing `s_uart_running` while the task may still be blocked on them — use-after-free if ever called (currently never called; note it or fix it).
- `append_rx_data_isr` (Daisy) increments stats fields also touched by the main loop without protection — counters only, but worth a comment.

---

## 5. Low severity / smells inventory

| Where | What |
|---|---|
| `daisy/src/config.hpp:17` | Comment "UART support removed - using SPI only" — reality is exactly inverted (see C4). |
| `wavex_application.cpp:133` | `vTaskDelay(1000)` commented "2 second loop". |
| `uart_debug_config.h:16` | Default level 2 commented "Enable INFO level" — 2 is WARN, INFO is 3. |
| `audio_engine.cpp:301-312` | `AuditionState` duplicates `WavState` fields that are never used (only `active` + `current_path` are). |
| `audio_engine.cpp:1051-1053` | `Timebase::Tick1kHz([]{})` — empty control tick placeholder; fine, but the §5.1 "envelopes, LFOs, CV staging" work it advertises all happens elsewhere or not at all. |
| `audio_engine.cpp` `Init` | `s_cpu_load_meter.Init(sample_rate, 48, 200)` hardcodes block size 48 next to `s_block_size` which nothing updates; `GetBlockPeriodMs` reads the variable. Tie both to `Timebase::kBlockSize`. |
| `protocol.h:433` | `SamplePathResponseMessage.path[200]` vs `BROWSE_PATH_MAX 96` vs Daisy-side `char file_path[200]` — pick one path-length constant. |
| `voice_manager.hpp` Render | Repeated `envelope.Release()` every sample after end-of-sample (idempotent, harmless); linear pan (documented); block-constant filter/env params — fine for Phase 1, note for Phase 2 param smoothing. |
| `fs_browse.cpp:50-54` | `FILINFO.lfname` under `FF_USE_LFN` — current FatFs exposes LFN via `fname`/`altname`; this block likely never compiles in (harmless but stale). |
| `sd_sdio.cpp:76` | Logs "(4-bit, STANDARD)" regardless of the configured speed/width just printed above. |
| `Makefile` | Heavy emoji/banner echo noise; `help` mentions `daisy-flash` "(if supported)" while the target exists. |
| `.pre-commit-config.yaml` | clang-format excludes `firmware/.*/libs/` and `managed_components/` but not the `libDaisy`/`DaisySP` submodule paths. |
| `scripts/graphify-refresh.sh` | Passes `--no-viz` to `graphify update`, which graphify 0.8.50 rejects — the repo's own refresh entry point (`make ai-graph`) is broken. Also the committed `graphify-out/` now contains only `manifest.json` (no `graph.json`), and the new `.graphifyignore` pulls all vendored code into the graph (8,393 files / 64k nodes — first-party signal is drowned; consider re-excluding `libDaisy`, `DaisySP`, `managed_components`). |
| Repo root | `protocol.o` (ignored but present), `node_modules/` with no `package.json`, `.specstory/`, `2026-07-02/` snapshot dirs in `graphify-out` history — cruft. |
| Git | Committed binaries: `firmware/daisy/tests/lib/lib{gtest,gtest_main,gmock,gmock_main}.a` and `firmware/daisy/tests/libwavex_test_lib.a` — obsolete since the vendored-source gtest switch (roadmap 0.2.6); remove from tracking. |
| `firmware/esp32/main/main.cpp:15-19` | "!!!!!!!!!! APP_MAIN HAS STARTED !!!!!!!!!!" + `printf` early-debug scaffolding. |
| `esp_uart_link.cpp:206-231` | `dequeue_tx_entry` copies a ~2.1 KB struct by value per send (fine at this rate; note only). |
| Naming | `daisy_filesystem.cpp` lives in `comm/`, `fs_browse.cpp` in `storage/` — the split is by accident of history, not responsibility. |

---

## 6. Architecture assessment

**What's working well (keep doing this):**

- The **§5.3 seam strategy** (output sink + CV group router behind flags, both flag sets in CI via `make daisy-stageb`) is exactly right for the Stage A→B hardware transition, and the implementations are clean.
- **HAL-free, host-testable cores** (`VoiceManager`, `Envelope`, `OnePoleFilter`, `MidiStreamParser`, `SequenceTracker`, `AttnWatchdog`) with real host tests. The MIDI parser in particular is a model of scope discipline (running status, real-time interleave, alignment).
- **Real-time discipline in new code**: the SPSC note queue with release/acquire indices, resolved-in-main-loop trigger params, and the `StopAll` memory-invalidation handshake are all correctly reasoned and documented in place.
- **CI** builds both MCUs plus the Stage-B flag set and runs all host suites in the dev toolchain image.
- The dma-timing-review workflow (numbered findings, fixes referencing them in comments) left an unusually good audit trail.

**Structural concerns:**

1. **The dispatch layer is the weakest link** — literally. `daisy_inter_mcu_message_handlers.cpp` is 476 lines of which ~350 are log scaffolding and stubs; it's where C1 hid. It has no tests (nothing asserts "message X reaches subsystem Y"). Since every future feature (sequencer ops, kit management, render-job progress) lands here, harden it first: table-driven dispatch (type → {min payload size, handler}) would eliminate both the per-case length-check boilerplate (H3's cousin) and the stub-that-compiles failure mode.
2. **Two ~800-line UART link implementations** (`daisy_uart_link.cpp`, `esp_uart_link.cpp`) share the frame format but have divergently-fixed copies of the same scanning/queueing logic (H2 exists precisely because a fix landed on one side only). The frame scanner and TX-queue state machine are transport-agnostic and belong in `firmware/shared/` next to `uart_protocol.cpp`, with per-platform I/O adapters — same extraction pattern that already succeeded with `SequenceTracker`.
3. **`audio_engine.cpp` is the god-file** (2,126 lines; highest fan-out in the graph): WAV parsing, streaming, prebuffering, resampling, preview generation, sample-load I/O, registry, meters, note dispatch, and the callback all in one translation unit with ~40 file-scope statics. The roadmap already prescribes the split direction implicitly (streamed-voice refactor, item 2). Suggested cuts: `wav_format.{h,cpp}` (shared parser, M12), `sample_store.{h,cpp}` (load/registry/`SampleMemMgr` glue), `stream_player.cpp` (ring/prebuffer/pump), leaving `audio_engine.cpp` as callback + wiring. Do it opportunistically, as roadmap 0.2.5 already prescribes for `daisy_spi_link.cpp`.
4. **Doc drift is systemic, not incidental** — architecture.md's "as-built" sections lag the code in both directions (says SPI active/UART legacy; says voice manager "not yet wired"/"design-only" §5.2/§10.4; says sampler still heap-grows §10.2; says partition table still 2 MB §3.3.3 — that one's fixed in code). Given AGENTS.md makes architecture.md canonical, stale "as-built" text is actively dangerous. Add "update architecture.md as-built sections" to the definition-of-done for roadmap items (the cross-cutting rules already require this for protocol changes only).
5. **Logging has no strategy** (M5): three mechanisms (`WAVEX_LOG_DAISY`, `UART_LOG*`, bare `PrintLine`/`printf`) with the loudest one ungated. Pick one gate per subsystem and enforce "hot paths log only behind compile-time flags" in review.

---

## 7. Test-coverage gaps (why C1 survived a green CI)

The suites are healthy at the unit level (234 tests, all green) but stop exactly at the seams:

1. **No dispatch-level tests.** Nothing asserts `ProcessInterMcuMessage(MSG_NOTE_ON, …)` produces a voice trigger (or even calls the engine). Add a host test that runs the real dispatcher against a mock/real engine and checks observable effect for every routed type — this single test class would have caught C1, the `HandleSampleControlMessage` stub, and the `MSG_SAMPLE_DATA` dead path.
2. **No end-to-end frame tests.** `CreateUartPacket` → `process_rx_frames` → dispatcher is never exercised as a chain (the Daisy RX scanner has no tests at all — H2 lives there; the ESP32 side's equivalent logic is likewise untested). The scanner extraction (§6.2) makes this trivially host-testable.
3. **Protocol boundary cases**: no test for payloads at 2042/2043 (H4), zero-length payloads through the router (H3), or seq wrap through 0 (M4).
4. **Four `MetricsTest`s are permanently disabled** in the Daisy suite — either make them run or delete them; disabled tests rot.
5. The ESP32 suite mocks broadly (`esp32_mocks`, `ui_mocks`) but has no test for `inter_mcu_send_*` return-code mapping — which is how C3 survived.

---

## 8. Build / config / repo hygiene summary

Covered above in specifics; consolidated actions: fix `scripts/graphify-refresh.sh` for graphify 0.8.50 and re-scope `.graphifyignore` to first-party code; untrack the five stale `.a` binaries; remove `protocol.o`/`node_modules` cruft; align `.pre-commit-config.yaml` excludes with the submodule layout. `partitions.csv` matches its roadmap claim (verified: 16 MB mapped, OTA slots present). Host test infra (vendored gtest, fresh build dirs) works as advertised — `make test` was run successfully on a bare host for this review.

---

## 9. Prioritized action list

**P0 — broken product behavior (small diffs, do immediately)**
1. Wire `MSG_NOTE_ON/OFF/CONTROL_CHANGE/SAMPLE_CTRL` dispatch to the audio engine; delete or use `audio_adapter`; correct CHANGELOG/roadmap item 8 (C1).
2. Fix `inter_mcu.cpp` return-code inversion in all send wrappers (C3).
3. Add the dispatch-level host test so P0.1 can't regress (§7.1).

**P1 — latent crashes/wedges and RAM (small-to-medium diffs)**
4. Daisy RX no-start-byte consume guard (H2).
5. Router payload-length validation (H3).
6. Protocol size-code overflow fixes + boundary tests (H4, H5).
7. `memory.h` bitmap cap fix + delete `s_conversion_buffer` (H1).
8. Verify/fix `StartLog(true)` standalone boot (H8).
9. Seq-generator skip-0 (M4).

**P2 — decide and document**
10. Transport decision: UART-of-record (wire `SequenceTracker` into UART RX, quarantine SPI code) or SPI revival plan; update architecture.md §4.4 and the other stale as-built sections (C4, §6.4).
11. Dead-DSP decision: delete or wire the oscillator/SVF/ADSR/LFO surface and the sampler record/play path (C2).
12. Resolve pin conflicts GPIO46/47 and D15; refresh pin_config.h prose (M1); fix the hardware_config dependency guard (M2).

**P3 — debt reduction (opportunistic, aligned with roadmap 0.2.5 style)**
13. Gate all hot-path logging; strip debug scaffolding (M5, M11).
14. Extract shared UART frame scanner + TX queue; fix TX-failure retry semantics in the process (§6.2, M6).
15. Extract shared WAV parser with odd-chunk padding (M12).
16. Browse-path statics + capacity static_asserts + one wire format; delete `ParseBrowseReq`/`spi_protocol.h`/`WaveXPacket` (H6, M10).
17. `memory.h` stats/large-pool accounting + refcount decision (M8).
18. Preview clamping + fixed staging buffer (M7).
19. Split `audio_engine.cpp` along the §6.3 lines as Phase-1 items touch it.
20. Repo cruft removal (§8).

---

## Appendix A — Doc-drift table (architecture.md vs. code, as of fafb122/0ae5d12)

| architecture.md claim | Reality in code |
|---|---|
| §2/§3.1/§4.4: SPI link "working"/"active", UART "legacy, removal is a cleanup item" | `WAVEX_SPI_LINK_ENABLED=0` hard-coded; UART carries **all** traffic including browse/wave; SPI code compiled out (link_config.h:18) |
| §4.1: `comm/ … listeners` | `ListenersManager` deleted (roadmap 0.2.4) |
| §4.1: `sd_spi + diskio (legacy)` in storage/ | Deleted (roadmap 0.2.1) |
| §3.3.3: partition table "only allocates 2 MB … rework pending" | Reworked and committed (partitions.csv, full 16 MB + OTA) |
| §5.2/§10.4: voice manager "design-only", "not yet wired into the audio callback" | Implemented and wired into `Callback()` (item 8 stage 2) — though unreachable from the wire, see C1 |
| §10.2: `Sampler` "records into a heap std::vector from the audio path" | Fixed — preallocated extent (item 3); but sampler is unreachable/inert (C2) |
| §5.1: control tick does "envelopes, LFOs, mod matrix, CV staging" | `Tick1kHz` body is empty; per-voice envelopes run per-sample in `VoiceManager::Render` |
| §4.4/§7.3: SequenceTracker/AttnWatchdog "implement" link degradation | True only for the compiled-out SPI path; live UART path has no seq protection |

## Appendix B — Graphify observations

First-party hub analysis from the regenerated graph: highest-fan-in symbols are `CreateUnifiedPacket` (17 call sites), `send_uart_message` (12), and the `wavex_file_browser_*` family; highest fan-out file is `audio_engine.cpp` (10 first-party deps), which together with its 2,126 lines and ~40 file-scope statics confirms it as the primary split candidate. `inter_mcu.cpp` is the top fan-in *file* on the ESP32 (8), consistent with its facade role — worth keeping thin as roadmap 0.2.4 intended. The graph also made the dead code findable: `audio_adapter.*`, `spi_protocol.h`'s types, and `ListDir`'s callers were all confirmed via zero inbound edges plus grep. Note the current `.graphifyignore` includes all vendored code (64k nodes), which buries this signal — see §8.
