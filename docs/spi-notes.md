# SPI Notes: libDaisy 8.1 and the WaveX Inter-MCU Link

These engineering notes preserve the 2026-09-13 UART/SPI cutover findings,
callback loads, acknowledged control timings, electrical experiments, and
remaining adoption gates. They also describe libDaisy 8.1.0 limitations and
a separate possible slave-mode proof. UART remains the production transport.

> **Status:** UART remains the default. The shared selector now controls both
> transports' startup and all application traffic. SPI is opt-in, requires
> matched firmware on both boards, and has no automatic fallback. See
> [flashing](flashing.md#uartspi-comparison) for cutover and rollback.

Pin assignments remain exclusively in
[`pin_config.h`](../firmware/shared/config/pin_config.h), and transport flags
remain in [`link_config.h`](../firmware/shared/config/link_config.h). This
document does not duplicate either table.

## Table of Contents

- [libDaisy 8.1 capability](#libdaisy-81-capability)
- [Slave-mode bring-up recipe](#slave-mode-bring-up-recipe)
- [Small libDaisy fork changes](#small-libdaisy-fork-changes)
- [What WaveX attempted](#what-wavex-attempted)
- [Why the old link was unstable](#why-the-old-link-was-unstable)
- [Retained transport contract](#retained-transport-contract)
- [Measured cutover comparison](#measured-cutover-comparison)
- [Latency interpretation](#latency-interpretation)
- [Final hardware state](#final-hardware-state)
- [Verification and remaining gates](#verification-and-remaining-gates)
- [Other Daisy SPI consumers](#other-daisy-spi-consumers)
- [Recommendation for WaveX](#recommendation-for-wavex)
- [Related](#related)

## libDaisy 8.1 capability

libDaisy 8.1.0 implements slave mode and asynchronous SPI DMA in
[`src/per/spi.cpp`](../firmware/daisy/libs/libDaisy/src/per/spi.cpp):

- `DmaReceive()` reaches `HAL_SPI_Receive_DMA()`.
- `DmaTransmitAndReceive()` reaches `HAL_SPI_TransmitReceive_DMA()` for full
  duplex.
- SPI1 through SPI5 select distinct `DMA_REQUEST_SPIx_RX` and
  `DMA_REQUEST_SPIx_TX` requests.
- Receive and duplex completion reach the libDaisy end callback through the HAL
  completion callbacks.

This is implemented capability, not a documented, proven slave configuration.
The v8.1 examples contain only blocking-master transmit and DMA-master transmit;
there is no SPI slave example.

The remaining hard limits are:

| Constraint | libDaisy 8.1 behavior |
|---|---|
| SPI6 DMA | Unsupported. DMA setup returns `ERR`; SPI6 is a BDMA/D3 problem on the H750 and has no libDaisy SPI IRQ path. |
| DMA streams | SPI1-SPI5 all use DMA2 Stream 2 for RX and Stream 3 for TX. The wrapper permits only one active SPI DMA peripheral at a time. |
| Transfer mode | DMA is `DMA_NORMAL`, so every receive must be re-armed after completion. |
| DMA initialization | `InitDma()` runs for every transfer instead of once during peripheral initialization. |
| Wrapper blocking | A DMA transfer is asynchronous after launch, but the launch path can busy-wait for the HAL state or for a one-slot libDaisy job queue. Treat the API as capable of stalling at entry. |
| Slave NSS pulse | `NSSPMode` is enabled for every mode. That is a master feature and is an unproven, suspicious setting for slave framing. |

The old RX-request-wired-to-TX defect does **not** explain WaveX's 2025 SPI
failures. Git history shows those experiments used libDaisy v8.0.0, whose SPI
source already had the correct request mapping and HAL RX/duplex DMA calls; the
SPI implementation is unchanged between the v8.0.0 and v8.1.0 tags.

## Slave-mode bring-up recipe

Use this recipe if a future experiment reverses the WaveX bus roles so the
ESP32-P4 is master and the Daisy is slave:

1. Use SPI1 through SPI5, never SPI6.
2. Configure `Mode::SLAVE`, `Direction::TWO_LINES`, and `NSS::HARD_INPUT`.
3. Put RX and TX buffers in `DMA_BUFFER_MEM_SECTION`. Do not use stack or DTCM
   buffers with DMA1/DMA2.
4. Arm `DmaTransmitAndReceive()` before the P4 asserts chip select.
5. Use one fixed physical frame size and re-arm immediately from the completion
   path. Keep parsing and application dispatch out of the ISR.
6. If a buffer ever moves to cacheable memory, give it exclusive 32-byte cache
   lines and perform the required clean/invalidate operations at ownership
   transitions.

The first bench test should be deliberately smaller than the WaveX protocol:

- 256-byte full-duplex frames
- P4 master at 10 MHz
- incrementing or pseudorandom patterns in both directions
- exact buffer comparison and counters for CRC, timeout, overrun, and missed
  frames

Walk clock rate and frame size upward only after a long zero-error run. Do not
mix the packet router, browser, waveform data, ATTN protocol, or audio engine
into this first proof.

## Small libDaisy fork changes

A local fork does not need a replacement SPI stack. The useful changes are
small and isolated:

1. Disable NSS pulse mode when `mode == SLAVE`.
2. Initialize and link the DMA streams once, then only program and start them
   per transfer.
3. Add an optional circular/listen mode, or an equally race-free fixed-frame
   re-arm path, so the slave is ready before the next chip-select edge.
4. Add a bounded error/abort path instead of the current unbounded HAL-state and
   queued-job waits.

Do not add SPI6 DMA to this patch. BDMA, D3/SRAM4 buffer placement, and its IRQ
path are a separate design. If the minimal SPI1-SPI5 test still fails after the
NSS change, call `HAL_SPI_TransmitReceive_DMA()` directly on the initialized HAL
handle for the experiment. That separates STM32 HAL slave behavior from the
libDaisy DMA scheduler.

## What WaveX attempted

The retained implementation uses Daisy SPI1 as master and ESP32-P4 SPI3 as
slave, mode 0, full duplex, with GPIO chip select on Daisy and ATTN from ESP32.
The optional reverse-role experiment above is a separate future design.

Both sides use the existing packet codec in
[protocol.h](../firmware/shared/spi_protocol/protocol.h). Physical framing
and ownership rules are centralized in
[spi_transport.hpp](../firmware/shared/spi_protocol/spi_transport.hpp).
The device adapters are
[daisy_spi_link.cpp](../firmware/daisy/src/comm/daisy_spi_link.cpp) and
[esp_spi_link.cpp](../firmware/esp32/main/links/esp_spi_link.cpp).

The libDaisy slave NSS-pulse concern was therefore not active in the failed
WaveX configuration. The project used SPI1, not SPI6. The reviewed DMA request
mapping and TX/RX argument forwarding were correct. The source evidence
instead identified ownership, handshake, receive publication and recovery
defects.

## Why the old link was unstable

The September 2026 audit reproduced these mechanisms with mocked DMA drivers.
The fixes remain behind the disabled SPI configuration.

ESP buffer ownership, completion length and setup callback semantics follow
the [ESP-IDF SPI slave API](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32p4/api-reference/peripherals/spi_slave.html).

| Previous failure | Source correction |
|---|---|
| ESP recycled descriptors and buffers after a result timeout, although IDF still owned them. | One descriptor and one TX/RX pair stay reserved until that exact descriptor returns. A timeout repeats only the wait. |
| Completion of an older empty frame consumed a newly queued message. | A frame reserves its own queue head, including the fact that it was empty; only its completion can consume that head. |
| Daisy send DMA discarded simultaneous RX, and another launch could overwrite pending RX. | Every transfer follows one duplex lifecycle. Foreground parsing and dispatch complete before either buffer is released. |
| Daisy EXTI prepared buffers and launched DMA concurrently with foreground send. | EXTI records a falling-edge generation only. One foreground owner reserves, prepares and launches DMA. |
| ATTN meant “message queued,” which did not establish slave readiness; requests could lose their only rising edge. | The IDF setup callback asserts READY after hardware setup. The completion callback clears it for every frame. Daisy polls the level and consumes each readiness cycle once. |
| Daisy timeout cleared a software flag without stopping DMA; some transfers had no deadline. | Every transfer has a deadline. Recovery masks only SPI/DMA IRQs, stops requests and streams, resets SPI, then waits without blocking until both streams are disabled before restoring HAL/library state. |
| TX was consumed at launch, or retained after successful piggyback delivery. | Both directions retain their outgoing head until full physical completion. Short/failed transfers retain it for retry. |
| Daisy passed an uninitialized parser capacity; ESP retained a dead unsafe small-buffer parser. | Daisy supplies explicit capacity. The unused ESP parser is removed; both RX paths validate the frame and apply sequence gating before dispatch. |
| ESP's eight-bit sequence counter emitted reserved zero every 256 packets. | The counter spans the wire's sixteen bits and skips zero on wrap. |
| ESP used descriptor capacity as received length. | Only the actual completed bit count can establish a full physical frame. Partial frames are discarded and cannot consume TX. |
| libDaisy restored SPI1 IRQ priority 0 above audio. | Initialization and recovery apply the audio-first priority policy to SPI1 and both DMA IRQs. |

This corrects concrete ways the link could stall, lose traffic or deliver
duplicates. It does not prove which particular mechanism caused each
historical freeze. That requires hardware traces.

## Retained transport contract

Each fixed physical frame carries one logical packet in each direction, or
zeros when that sender has no packet. Both adapters now use the existing
length-bearing codec in `uart_protocol.h`, preserving exact payload lengths;
the older dormant size-class codec padded lengths and could not serve strict
application handlers. UART framing, shared payloads and the protocol version
remain unchanged. Old experimental SPI images are incompatible. Physical frame
size, legacy poll interval and transfer deadline live in `spi_transport.hpp`.
The scheduling A/B selector lives in
[`hardware_config.h`](../firmware/shared/config/hardware_config.h).

Daisy validates completed RX and copies it into a bounded foreground queue
before releasing the DMA slot. A TX-only pump during sample loading can drain
replies without recursively dispatching another command. The outer service
dispatches one saved command at a time; a full RX queue prevents another launch.
Fast scheduling retires completed DMA, dispatches one command, then prepares
the next frame so an immediately queued reply can use it. It removes the
artificial launch interval, including for empty polls, while retaining one
DMA owner and at most one launch per pump. When a command has no reply queued
yet, the outer service defers an empty launch until the next foreground pass,
allowing the existing deferred reply producers to run first. Explicit TX-only
pumping inside long operations remains available. The legacy
selection preserves the original cadence and preparation order.

ESP queues an empty frame when idle and leaves it immutable. Daisy clocks READY frames, including empty ones; a message arriving behind an already
armed empty frame is sent on the next frame. READY is a hardware ownership
signal, so there is no delay-based readiness assumption or ATTN watchdog that
withdraws a driver-owned descriptor.

Daisy consumes each READY assertion once. Its falling-edge counter retains a
short deassertion that the foreground might miss; level polling also handles
an observed low level and a slave already ready at startup. An unstable
level/edge sample defers launch. No SPI launch, packet preparation, parsing or
logging runs in EXTI or the audio callback.

Recovery has two deliberate limits:

- Daisy must exclusively own libDaisy's shared SPI DMA stream pair. Stage A
  meets this condition; the dormant adapter rejects a combined Stage B SPI CV
  configuration at compile time. Recovery clears the library scheduler only
  after both streams stop. It never resets the DMA controller used by UART.
- If hardware never stops, Daisy retains the buffers and stays offline while
  returning from foreground service. After an abort it requires fresh READY;
  a peer stuck high cannot be assumed ready. ESP stop likewise returns a
  timeout without freeing driver-owned memory when the master does not finish
  its pending frame. Retry stop after the descriptor returns.

Daisy buffers occupy aligned, noncacheable DMA SRAM. ESP allocates complete
internal DMA/cache lines using the configured P4 cache-line size; ESP-IDF owns
cache maintenance. The callbacks only set READY and request no IRAM-only
interrupt allocation. Audio callback code and the default transport are unchanged.

Completion establishes that a full frame was clocked, not application-level
acknowledgment. CRC rejection is detected, but this transport does not promise
automatic delivery after every corruption or reset. Any future production
revival must define and test that retry policy before production adoption.

### Late DMA interrupt exposed by the cutover

The 2026-09-13 heavy kit-setup trial exposed another failure after the initial
browse/waveform/sequencer smoke checks passed. SWD repeatedly stopped in
`DMA2_Stream3_IRQHandler`: libDaisy's active peripheral was -1, both streams'
enable bits and remaining counts were zero, and TX half/transfer-complete
flags were still pending. No Cortex fault was recorded. The ownerless vendor
handler returned without acknowledging the flags, starving foreground service
and USB, including the foreground transfer watchdog.

The WaveX completion callback now retires both streams' remaining peripheral
and NVIC interrupts after a successful completion has proved both stopped.
An error or still-enabled stream masks those IRQs and retains the buffers for
foreground recovery. This stays within the adapter's exclusive DMA ownership;
no libDaisy fork or audio callback change is required. Regression tests model
the captured late TX event and reject premature buffer publication.

## Measured cutover comparison

The 2026-09-13 `experiment/mcu-link-switch` branch comparison used persistent
QSPI images, the same profiling configuration, and the same eight-voice
playback/sequencer workload. Each completed capture ran about 306 seconds with
SD streaming, live Instrument/pad edits, four parameter locks per enabled
step, touch-grid/playhead readback, and two pattern save/load cycles. These are
short diagnostic trials of a dirty checkout, not the callback capacity or
production soak gate.

| Link tested | Calculated maximum-payload wire time | Callback average | Callback peak | Result |
|---|---:|---:|---:|---|
| UART, 2 Mbaud, 8N1 | 10.290 ms | 30.71% / 307.1 us | 67.41% / 674.1 us | Passed |
| SPI, 1.5625 MHz, corrected completion | 11.141 ms | 31.30% / 313.0 us | 67.11% / 671.1 us | Passed |
| SPI, 6.25 MHz | 2.785 ms | 31.39% / 313.9 us | 64.07% / 640.7 us | Passed |
| SPI, 12.5 MHz | 1.393 ms | 31.55% / 315.5 us | 67.89% / 678.9 us | Passed |
| SPI, 24 MHz, original low slew | 0.725 ms | Unavailable | Unavailable | Failed before workload setup completed |
| SPI, 24 MHz, corrected output slew | 0.725 ms | 31.64% / 316.4 us | 69.59% / 695.9 us | Fresh-boot retry passed; earlier SD error retained |

Callback percentages use the 1 ms audio deadline (48 frames at 48 kHz). Wire
times are calculated from the configured clock and physical framing, without
queue, READY, dispatch, or application latency. UART sends the exact logical
length; SPI always clocks its fixed padded slot, even for a short command.
Consequently faster bulk wire throughput does not guarantee faster small
command response. No scope measurement of CS-to-CS or end-to-end latency was
made.

All five completed runs recorded zero audio underruns, console drops, or
sequence drops. The successful SPI runs also recorded zero invalid received
frames and transfer timeouts. Foreground link-service elapsed time averaged
21.64% for UART, 9.42% for slow SPI, 10.66% at 6.25 MHz, 13.71% at 12.5 MHz, and 10.85% at 24 MHz with corrected slew; this includes
preempting audio and is not an exclusive CPU-load measurement. TX queue-full
counter increases were 436, 5017, 2007, 2024, and 2072 respectively. Those count rejected
enqueue attempts, including retries, rather than proving lost musical events.
The similar callback averages/peaks do not establish UART as the cause of the
peaks or demonstrate a repeatable audio performance improvement from SPI.

At 24 MHz, non-halting SWD reads confirmed the SPI123 mux selected PLL1Q and
SPI1 used its divide-by-eight setting. The kernel is 192 MHz; no PLL was
retuned. The first sequencer readback failed with zero valid received messages
and 11 invalid frames on Daisy. Rebooting the ESP32 and retrying reproduced the
failure; invalid frames reached 22. Both consoles and Daisy audio callbacks
remained responsive. This is a high-speed receive failure, not the earlier
ownerless DMA interrupt storm. Electrical/sampling timing remains unproven;
the evidence does not identify the precise cause. Exact 25 MHz was not tested.

Local raw artifacts live under `logs/` (gitignored):

- `perf-link-uart-before-wavex-20260913-085136.{log,json}`;
  Daisy image SHA-256 begins `631472b68427`.
- `perf-link-spi-fixed-wavex-20260913-091714.{log,json}`;
  Daisy image SHA-256 begins `db28ebee3c62`.
- `perf-link-spi-6250-wavex-20260913-093954.{log,json}`;
  Daisy image SHA-256 begins `e713a49139a2`.
- `perf-link-spi-12500-wavex-20260913-095322.{log,json}`;
  Daisy image SHA-256 begins `e16da93f9531`.
- `perf-link-spi-24000-wavex-20260913-094848.json`, the subsequent retry
  metadata, and `link-cutover-20260913/*24000*` console/register evidence;
  Daisy image SHA-256 begins `a7586d09e88f1`.
- `perf-link-spi-24000-slew-retry2-wavex-20260913-102619.{log,json}`;
  Daisy image SHA-256 begins `80b51778564c`.
- `link-cutover-20260913/comparison.json` holds the parsed successful captures;
  retained firmware and the independent UART rollback images are alongside it.

### Output slew experiment

A register-only A/B check kept the same 24 MHz image and changed only the
SCLK/MOSI speed fields from libDaisy's low-slew defaults to very-high slew.
The three browse/load, waveform and sequencer HIL tests then passed in 55.73
seconds, with zero Daisy invalid frames and transfer timeouts. The original
values and non-halting SWD writes are preserved in
`logs/link-cutover-20260913/spi-24000-slew-registers.log`. This supports an
edge-speed/timing-margin explanation; it does not certify wiring or establish
that termination resistors are or are not needed.

The subsequent full workload did not pass: the first pattern save returned
`SEQ_FILE_IO`, with `PATTERN_FILE: op=1 phase=3 error=5 fatfs=1` and
`PATTERN_SD: fr=1 hal_err=0x00000006 state=0 offset=5120`. The received
error is an acknowledged backend SD/FatFS disk error; Daisy SPI invalid-frame
and timeout counters remained zero. Preserve this failure as an outstanding
storage/system-load concern rather than classifying the full trial as clean.
Its capture is `perf-link-spi-24000-slew-wavex-20260913-101231.{log,json}`.

A later fresh-boot run of the firmware-configured slew completed 306.090
seconds, both save/load cycles and 191 live-edit cycles, with zero underruns,
invalid SPI frames and transfer timeouts. It measured 31.64% average and 69.59%
peak callback load. This successful retry does not erase the earlier SD error
or close the long-soak gate. An intervening retry was interrupted by a debugger
recovery-helper invocation and is explicitly excluded from successful captures.

The application adapter now applies the same measured SCLK/MOSI slew after
initialization and recovery, preserving alternate functions, pulls and all
other GPIO fields. Its A/B macro lives in
[`hardware_config.h`](../firmware/shared/config/hardware_config.h). A hardware
mock resets slew at vendor initialization and verifies both initial setup and
recovery preserve every other pin's speed. ITCM placement is unchanged.

### Control latency and code placement

A separate comparison timestamped real Instrument AMP edits immediately before
ESP link admission and their matching completed state at ESP reception, using
one `esp_timer_get_time()` clock. One hundred alternating pan edits were
acknowledged in each phase. The playing phase used eight looped voices,
sequencing and SD preview streaming; it was a simpler workload than the full
callback-load capture above. All edits completed and both runs had zero audio
underruns. The optional timestamp macro lives in
[`hardware_config.h`](../firmware/shared/config/hardware_config.h) and defaults
off. It adds no wire fields, shared profiling state, or audio instrumentation.

| Transport / workload | Mean control RTT | p95 | Maximum |
|---|---:|---:|---:|
| UART / idle | 0.753 ms | 0.799 ms | 0.944 ms |
| SPI 12.5 MHz / idle | 12.521 ms | 14.990 ms | 19.060 ms |
| UART / playback + streaming | 0.907 ms | 1.038 ms | 1.450 ms |
| SPI 12.5 MHz / playback + streaming | 13.259 ms | 18.209 ms | 21.643 ms |

These are backend-acknowledged control round trips, excluding UI event dispatch;
they are not direct one-way or audible-response measurements. Console arrival
times are not used. Both transports used the same trace code; timestamping
precedes printing. Raw events, all samples and the driver are retained in
`logs/link-cutover-20260913/control-*` and `control_latency.py`.

The legacy SPI pump used for the comparison above enforces a minimum five-millisecond interval between
**all** transaction launches (at most 200 transactions/second even when SCLK
is much faster). An ESP command can also wait behind an already
armed empty frame. The resulting multiple-slot control path is a concrete
latency cost, even when wire time is reduced by increasing SCLK. Changing that
policy or physical framing requires a separately measured ownership-preserving
optimization. Under the user's criterion of keeping SPI only without slower
controls, these measurements select UART.

The measured 12.5 MHz ELF places `Spi_PumpTx`, `FinishDma`, SPI/DMA IRQ handlers,
and `HAL_SPI_IRQHandler` in QSPI (`0x900...`). ITCM occupancy is 23,744 of 65,536
bytes, leaving 41,792 bytes. Selective relocation has room, but no SPI ITCM
performance comparison has been run. CPU service time and gaps between frames
could improve; ITCM cannot directly repair the SCLK/MISO sampling margin while
hardware DMA is transferring. Measure parsing, copying and launch costs before
choosing functions, and retain the audio-first interrupt priorities.

Pinned libDaisy initializes its SPI pins with `GPIO_SPEED_FREQ_LOW`. That is a
slew-rate setting, distinct from the peripheral divider, and is a concrete
candidate to test for the high-speed failure before choosing hardware changes.
The P4's [official SPI slave documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/spi_slave.html#sclk-frequency-requirements)
specifies operation up to 60 MHz with suitable clock timing; this is not a
verified rate for the assembled WaveX link. Scope SCLK at the slave and MISO
at the master, including CS setup and duty cycle, to distinguish slew,
ringing/termination, propagation delay and sampling configuration.

### Fast scheduling and 48 MHz follow-up

The subsequent branch experiment removed the artificial interval and reordered
the foreground service to dispatch RX before preparing TX. Instrument replies
are deferred by existing producers later in the main loop; the final scheduler
also gives those producers one foreground pass before launching another empty
frame. Explicit TX-only progress during long operations remains available.
The hardware-header scheduling selector restores the original interval and
preparation order for comparison. No audio callback, ITCM placement, ESP
descriptor ownership, payload format or PLL tuning changed in this follow-up.

The same ESP timestamp image and 100 idle plus 100 playing Instrument AMP
edits measured these acknowledged round trips:

| Selection | Idle mean / maximum | Playback mean / p95 / maximum |
|---|---:|---:|
| Earlier UART reference | 0.753 / 0.944 ms | 0.907 / 1.038 / 1.450 ms |
| Earlier SPI 12.5 MHz, legacy cadence | 12.521 / 19.060 ms | 13.259 / 18.209 / 21.643 ms |
| SPI 24 MHz, initial fast scheduler | 3.237 / 4.046 ms | 3.710 / 4.766 / 5.257 ms |
| SPI 24 MHz, final deferred-reply scheduler | 2.307 / 2.839 ms | 2.794 / 4.022 / 4.912 ms |
| SPI 48 MHz, final scheduler | Failed before timing samples | Unavailable |
| SPI 48 MHz, legacy cadence | Failed before timing samples | Unavailable |

Both successful 24 MHz control trials acknowledged all 200 edits, with zero
reported underruns, invalid SPI frames and transfer timeouts. These use the
simpler eight-looped-voice plus sequencer/streaming workload described above.
The two 24 MHz scheduler revisions isolate the deferred-reply change; comparing
either against the older 12.5 MHz trial changes both clock and scheduling.

At 48 MHz, startup verified the existing 192 MHz kernel divided by four.
Non-halting register reads confirmed the mux, divider and fast SCLK/MOSI slew.
A 2176-byte physical frame would take 0.363 ms and two frames 0.725 ms, calculated
without waiting or processing. The link could not complete the prerequisite
Instrument OSC readback, so no valid 48 MHz control RTT or comparable full-load
result exists. The first fast-scheduler attempt and retry after an ESP reset
both failed; Daisy's counters reached zero accepted ESP packets, 78 invalid
frames and zero timeouts. Repeating at the original five-millisecond cadence
also failed, with zero accepted packets, 113 invalid frames and zero timeouts.
Both consoles stayed responsive. This establishes a clock-sensitive receive
failure independent of the fast scheduling policy, but does not identify
whether ESP output timing, Daisy sampling, or the wiring causes it.

The initial fast-scheduler 24 MHz full workload also failed at its first
pattern save: `fatfs=1`, `hal_err=0x00000006`, offset 0. SPI invalid-frame and
timeout counters remained zero. The retained profiling windows (130,040
callbacks) measured 32.35% average and 69.07% maximum callback load with zero
reported underruns; these are **partial failed-run measurements**, not a
completed five-minute comparison. They do not establish a load improvement.
The final deferred-reply scheduler has control-latency and waveform-navigation
coverage, but no completed heavy-load comparison. This repeats the storage
concern from the earlier 24 MHz slew trial and remains an adoption blocker.

Final fast and legacy device paths pass the SPI sanitizer coverage (41 cases
across device/ownership tests), and both firmware builds plus all three normal
host suites pass. The experiment is preserved on `experiment/mcu-link-switch`; UART
remains the source default and the selected hardware transport after rollback.

Raw evidence is retained under `logs/link-cutover-20260913/`:
`control-spi-24000-scheduled*`, `control-spi-48000-*`,
`spi-48000-*-registers*.log`, `spi-48000-retry-rx-state.log`,
`spi-48000-failure-*.log`, `scheduling-images.json` and
`scheduling-partial-load.json`. The failed heavy capture is
`logs/perf-link-spi-24000-scheduled-wavex-20260913-132919.{log,json}`.
The final Daisy image hashes begin `9d4ff186b7ae` at 24 MHz and
`1f46fd6ba146` at 48 MHz. Separate images preserve the initial scheduler and
the 48 MHz legacy-cadence test.

### Intermediate-rate search

The next branch trial added a nominal 32 MHz preset using the already-running
64 MHz HSI clock through CKPER and a divide-by-two SPI prescaler. Only the
SPI kernel mux changes; the shared clock sources and PLL configuration remain
unchanged. Startup verifies the expected kernel rate and divider before
enabling the link. Non-halting register reads confirmed the selected mux and
divider; the actual RC oscillator frequency was not measured with a scope.

With the final deferred-reply scheduler and both boards freshly flashed,
32 MHz failed the prerequisite Instrument OSC readback. Daisy reported zero
accepted ESP packets, 39 invalid frames and zero transfer timeouts. Both
consoles remained responsive. No valid control RTT or load comparison was
collected. Calculated wire time is 0.544 ms per 2176-byte frame, or 1.088 ms
for two frames, before waiting and processing.

The observed search bracket is therefore a successful 24 MHz control trial
and a failed nominal 32 MHz trial. This is not a proven stability threshold:
the 32 MHz trial uses a different clock source, the failure need not be
strictly monotonic with frequency, and the successful control trial does
not resolve the documented SD failures or satisfy the full soak gate.

The existing SPI prescalers are powers of two, so arbitrary midpoint requests
such as 28 or 36 MHz cannot be obtained by changing only the prescaler.
The unchanged clock tree offers no useful midpoint between 24 and 32 MHz.
With the existing 960 MHz PLL1 VCO, a separately implemented boot-time PLL1Q
profile could nominally produce 25.263, 26.667, 28.235 and 30 MHz, or 36.923 MHz
above the failed trial. These are calculated candidates, not implemented
presets or verified rates. The pinned STM32 HAL refuses to change PLL1Q while
PLL1 supplies the system clock; finer search therefore requires a deliberate
clock-initialization change and validation of other clock consumers. No live
PLL retuning was attempted.

The 32 MHz preset passed all 41 SPI ASan/UBSan cases. Both normal firmware
builds and all three host test suites also passed the required hooks. Evidence
is retained in `logs/link-cutover-20260913/midpoint-images.json`,
`control-spi-32000-scheduled*`, `spi-32000-registers.log`,
`spi-32000-failure-*.log` and `spi-midpoint-checks.log`. The Daisy image hash
begins `e30c30c9b159`; the same ESP timing image was reused.

### Return-path investigation

The next comparison isolated ESP MISO drive and matched SPI clock mode.
The existing Daisy SCLK/MOSI slew fix remained enabled throughout; the
scheduler, fixed-slot framing, DMA ownership and clock tree did not change.
The ESP driver already configures MISO as push-pull, and the pinned P4 slave
API exposes clock mode but no separate slave-output-delay setting. SPI3 uses
the GPIO matrix; this observation alone does not establish an added delay
on P4. The [P4 slave documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/spi_slave.html)
describes operation up to 60 MHz with suitable clock timing, not a guarantee
for this assembled full-duplex path.

Optional diagnostics capture only the first eight completed nonempty ESP TX
and rejected Daisy RX prefixes. Capture occurs after DMA returns ownership,
in task/foreground context; no audio callback or interrupt logs were added.
The host-side analysis compares the first four complete 32-byte prefixes
because some later ESP console lines interleave with UI debug replies.

| Nominal rate | SPI mode | ESP MISO drive | Result |
|---|---:|---:|---|
| 32 MHz | 0 | 2, observed default | Instrument readback failed; RX 0, invalid 39 |
| 32 MHz | 0 | 3, strongest | Instrument readback failed; RX 0, invalid 39 |
| 32 MHz | 1 | 2, observed default | Readback completed; first AMP edit failed; RX 6, invalid 24 |
| 32 MHz | 1 | 3, strongest | All 200 edits completed; zero reported invalid frames |
| 48 MHz | 1 | 3, strongest | Instrument readback failed; RX 0, invalid 39 |

All listed counter snapshots reported zero transfer timeouts. The two
32 MHz mode-0 snapshots were taken after the failed trial and before the
next flash; the mode-1 default-drive snapshot likewise includes the idle
interval after its failed control edit. Counters are CPU-reported values,
not debugger reads of potentially dirty cached statistics.

In the 32 MHz mode-0 strong-drive capture, all 56 mismatched bits in the
four compared prefixes were zero-to-one errors immediately after a
transmitted one. At 48 MHz mode 1, all four received prefixes exactly
matched a one-bit-delayed transmit prefix with a leading zero. For example,
transmitted A5 0A became 52 85. These observations strongly support a return
data/sample-edge timing problem. They do not establish whether the delay
comes from ESP output timing, wiring/loading, clock distortion, or Daisy
input timing. A scope comparison of SCLK and MISO at the Daisy receiver
remains necessary to distinguish them. No attached USB scope/analyzer was
available for this investigation.

The first successful 32 MHz mode-1/drive-3 run completed 100 idle and 100
playing AMP edits with zero reported audio underruns, invalid Daisy frames
and transfer timeouts. Acknowledged RTT was 1.935 ms mean / 3.391 ms maximum
idle, and 2.315 ms mean / 3.392 ms p95 / 4.765 ms maximum playing. This is the
same simpler eight-voice, sequencer and SD-preview control workload used
above, not the heavy DSP/load soak. Packet diagnostics were enabled for
this first run. A fresh paired flash with diagnostics disabled repeated all
200 edits without reported invalid frames, transfer timeouts or audio
underruns. Idle RTT measured 1.945 ms mean / 2.164 ms p95 / 4.054 ms maximum;
playing RTT measured 2.282 ms mean / 2.982 ms p95 / 4.011 ms maximum. This
remains slower than the earlier UART acknowledged-control reference. These
two runs cover 400 edits, not a sustained heavy-load or signal-integrity soak.
The diagnostics-disabled pair also passed all four selected two-board HIL
checks (browse/load, pattern save/load, waveform navigation and sequencer)
in 96.22 seconds. A fresh CPU-reported snapshot after those checks recorded
875 accepted return packets, zero invalid frames, zero transfer timeouts and
zero audio underruns, with both boards idle. The successful short pattern-file
test does not erase the earlier failures in the distinct heavy DSP/storage
workload.

The settings remain opt-in hardware-header selectors and do not establish
a production rate or close the earlier SD/adoption gates. Normal builds
retain UART and the prior electrical defaults. Both device builds and all
normal host suites passed; SPI ASan/UBSan coverage passed 42 cases with
explicit drive selection, including cleanup if pad configuration fails,
and 41 cases for the default-drive mode-1 path.

Evidence is retained under `logs/return-path-20260913/`: image copies and
hashes, exact compiler flags, per-trial JSON/control/board logs,
`drive3-prefix-analysis.json`, and `48-prefix-analysis.json`. Two setup
runs used stale/default UART ESP objects despite intended flag overrides;
they are explicitly marked `invalid_setup` and excluded from the electrical
results. Subsequent images passed executable-content checks before flashing.
See the [build/flash caveat](flashing.md#uartspi-comparison).

### Latency interpretation

ATTN already has a falling-edge interrupt on Daisy. That interrupt records a
readiness generation; foreground code polls the high READY level and owns
DMA launch. READY means the ESP has armed a descriptor, even when its outgoing
packet is empty. It does not mean that an application message is waiting.
The fast scheduler removes the old five-millisecond launch interval, but does
not remove the immutable empty descriptor or fixed physical slot.

At nominal 32 MHz, SPI clocks 4 MB/s in each direction before framing and
software gaps. UART at 2 Mbaud with 8N1 carries 200 kB/s of framed bytes in
each direction. SPI therefore has twenty times the raw byte capacity here;
sustained application goodput was not measured. A short command nevertheless
occupies a complete 2176-byte SPI slot: 544 us, or at least 1.088 ms for
separate request and reply slots. An already armed empty ESP frame can cost
another slot. Task wakeup, foreground dispatch and reply preparation add
further time; their individual contributions were not instrumented.

The diagnostics-disabled 32 MHz playing trial measured 2.282 ms mean RTT,
versus the UART reference's 0.907 ms, approximately 2.52 times as long.
Neither faster byte capacity nor the observed foreground service percentages
establish lower control latency or lower exclusive CPU load. These findings
motivate investigating UART controls **and their confirming replies**, with
SPI carrying bulk and replaceable periodic telemetry. No hybrid routing was
implemented or measured in this experiment.

### Final hardware state

The saved normal UART images were restored on both boards after comparison
(profiling off), including after the return-path investigation.
Browse/load, named pattern save/load, waveform navigation and sequencer HIL
checks passed 4/4 in 91.01 seconds after that final rollback. A final console
read confirmed zero voices, streaming stopped and zero underruns. The latest
rollback image hashes and state are retained in
`logs/return-path-20260913/uart-restored.json`, with
`restore-uart-*.log` and `hil-uart-restored.log`.
The experiment changes remain on `experiment/mcu-link-switch`; UART is still
the source default. The user permitted retaining SPI only if controls were
not slower, which the measured control comparison did not satisfy.

For durable image attribution, the final successful SPI pair and the restored
UART pair have these full SHA-256 identities. SPI packet-prefix diagnostics
were disabled in the final pair; its latency/DWT profiling remained enabled.
UART rollback profiling was disabled. These are measured binaries built from
the working experiment, not a claim that rebuilding the later commit produces
byte-identical images. Exact compiler flags and local binaries remain in the
artifact paths above; Git preserves the source and this measurement record.

| Image | SHA-256 |
|---|---|
| SPI final Daisy | `c550b5eaed8d566a245d9b70cb637e65f51e82748df9cacb4e3db08ccf51c264` |
| SPI final ESP32 | `358b1787ff0bfb1b057e690428109c5b6cea00563012291fb87abe3706362654` |
| UART rollback Daisy | `87a94650bcd3125f1cbab31e09d71379078ba2ab6a67ea3472682c7533b35bdb` |
| UART rollback ESP32 | `c56c8f7bfacc794305dc7d733dac86903616bf64f8129111cb0f57fb9d8ec9ca` |

## Verification and remaining gates

The host suite compiles the actual Daisy and ESP adapters against hardware
mocks, in addition to testing their shared ownership state. Regression cases
cover repeated ESP timeouts, empty-frame completion, short transfers, retained
TX, RX dispatch during full duplex, reentrant queueing, EXTI races, stuck DMA
enable bits, recovery ordering, priority restoration, duplicate rejection,
all packet sizes and sequence wrap. These tests can run under ASan/UBSan via
the shared test CMake sanitizer option.

Separate experiment builds compile both selections using the real ARM and
ESP-IDF toolchains. The repository configuration and normal build outputs
keep SPI disabled. Compilation and mocked registers are not timing or soak
verification.

A debugger GPIO test on 2026-09-07 observed SCLK, MOSI, MISO, CS and ATTN at the
opposite MCU in their natural directions. Each line passed eight low/high
states with opposite receiver pulls; all five peer inputs were checked at each
state (40 driven states, 200 observations), with no observed cross-coupling.
GPIO configuration was restored and both CPUs resumed. No SPI peripheral was
enabled or firmware flashed for this test. It establishes static connectivity,
not signal integrity at SPI clock rates.

The corrected slow-SPI image passed three selected two-board HIL tests;
the 12.5 MHz image repeated browse/load, waveform navigation, and sequencer
coverage with 3/3 passing in 56.33 seconds. ASan/UBSan passed all 37 SPI cases
for each tested clock-code branch (6.25, 12.5, and 24 MHz). The added GPIO-speed
recovery case brings this to 38/38 with both fast and original low slew. The
original 24 MHz hardware failure despite passing mocks demonstrates the
remaining physical test gap.

Before production adoption:

1. Keep both firmware selections matched. Macro-controlled startup is implemented;
   UART remains the production choice.
2. Verify READY versus CS/clock timing and physical full-duplex patterns on the
   bench, then exercise simultaneous application traffic.
3. Inject short transfers, CRC errors, DMA errors, disconnects and either
   MCU rebooting. Verify recovery and application retry semantics.
4. Run audio/SD load with DWT timing and underrun/error counters over a long
   soak. Confirm the exclusive SPI DMA ownership assumption for the intended
   hardware configuration.

## Other Daisy SPI consumers

There is no second **live** general-purpose SPI user on the Daisy in the Stage A
build:

- The MCP4728 CV DAC uses I2C.
- The SD card uses 4-bit SDMMC; the SPI-SD implementation was removed. Some
  legacy SPI-SD pin macros remain, but no code consumes them.
- The Seed's QSPI flash uses the dedicated QUADSPI peripheral, not
  `SpiHandle` or the shared DMA2 streams.

There is, however, an important **planned** non-link SPI user. Phase 3's Stage B
CV backend reserves a write-only SPI bus for the MCP48CMB28 DAC chain. The
backend is currently only a compiling stub in
[`mcp48_backend.hpp`](../firmware/daisy/src/cv/mcp48_backend.hpp).

The current reserved DAC macros do not yet form a libDaisy-supported hardware
SPI pin set: the configured clock macro fails the relevant `SpiHandle` SCLK
alternate-function validation. Phase 3 must therefore select and validate the
actual SPI peripheral and pin set before PCB or driver bring-up, regardless of
what happens to the MCU link.

Even after that pin plan is corrected, all libDaisy SPI1-SPI5 DMA transfers
share DMA2 Streams 2 and 3. A DMA-driven MCU link and a DMA-driven 1 kHz CV bus
would therefore serialize through one global owner and one queued job per
peripheral. That is not automatically impossible, but it creates timing
coupling between control voltage updates and bulk link traffic—the opposite of
the deterministic ownership WaveX wants.

## Recommendation for WaveX

Keep UART as the inter-MCU transport unless measurement shows its 200 kB/s
framed-byte rate is a real bottleneck. It already carries the current traffic with
large margin, has continuous DMA on the Daisy, and does not compete with the
planned Stage B CV SPI path.

The authorized Stage A branch experiment can compare the existing topology
without implementing Phase 3. Before production adoption or a topology change:

1. Resolve the Phase 3 CV SPI peripheral/pin plan and shared DMA ownership before
   combining the two SPI consumers.
2. Prefer a separate, minimal P4-master/Daisy-slave proof using SPI1, hardware
   NSS, DMA-safe 256-byte buffers, and no WaveX protocol.
3. Apply the small slave-specific libDaisy changes above or use the HAL directly
   for the proof.
4. Require a zero-error soak, audio-underrun counters, DWT/logic-analyzer timing,
   peer-reboot recovery, and simultaneous CV-update testing before choosing a
   production topology.
5. Keep the experiment opt-in and restore UART after comparison. Passing a short
   trial does not close the production gate.

The planned CV DAC bus is therefore a valid additional reason to avoid SPI for
the MCU link, but not because the H750 lacks multiple SPI peripherals. The real
constraint is that libDaisy funnels their DMA through one shared stream pair,
while WaveX currently has no measured need for the extra link bandwidth.

## Related

- [`architecture.md`](architecture.md) — current UART transport and dormant SPI topology
- [`features/inter-mcu-protocol.md`](features/inter-mcu-protocol.md) — live wire contract
- [`features/analog-voice-board.md`](features/analog-voice-board.md) — planned Stage B CV bus
- [`daisy_rt_audio_coding_guide.md`](daisy_rt_audio_coding_guide.md) — DMA, cache, ISR, and audio rules
- [`roadmap.md`](roadmap.md) — SPI revival gates and rationale
