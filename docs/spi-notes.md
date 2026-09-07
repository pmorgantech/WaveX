# SPI Notes: libDaisy 8.1 and the WaveX Inter-MCU Link

These engineering notes record what libDaisy 8.1.0 actually supports, how to
bring up an STM32H750 SPI slave without relying on assumptions, and what the
dormant WaveX SPI link tells us about its earlier instability. They are a
reference for a future measured SPI experiment, not a decision to replace the
live UART transport.

> **Status:** The WaveX transport of record remains full-duplex UART at 2 Mbaud.
> There is a single shared flag, `WAVEX_SPI_LINK_ENABLED`, currently `0` and
> governing both sides — the ESP32 header defers to the shared config rather
> than carrying its own. The source fixes below remain compiled out; they
> do not establish hardware bring-up or enable SPI startup on ESP32.

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

The packet layout and version are unchanged. Each fixed physical frame carries
one logical packet in each direction, or zeros when that sender has no packet.
The frame size, poll interval and transfer deadline live in spi_transport.hpp.

ESP queues an empty frame when idle and leaves it immutable. Daisy periodically
clocks READY frames, including empty ones; a message arriving behind an already
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
interrupt allocation. UART, audio, application routing and feature defaults
are untouched.

Completion establishes that a full frame was clocked, not application-level
acknowledgment. CRC rejection is detected, but this transport does not promise
automatic delivery after every corruption or reset. Any future production
revival must define and test that retry policy along with startup integration.

## Verification and remaining gates

The host suite compiles the actual Daisy and ESP adapters against hardware
mocks, in addition to testing their shared ownership state. Regression cases
cover repeated ESP timeouts, empty-frame completion, short transfers, retained
TX, RX dispatch during full duplex, reentrant queueing, EXTI races, stuck DMA
enable bits, recovery ordering, priority restoration, duplicate rejection,
all packet sizes and sequence wrap. These tests can run under ASan/UBSan via
the shared test CMake sanitizer option.

Separate scratch builds compile the dormant paths using the real ARM and
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

Before any revival:

1. Make an explicit transport/startup decision; ESP application startup still
   starts UART only.
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

Keep UART as the inter-MCU transport unless measurement shows its 200 KB/s
payload rate is a real bottleneck. It already carries the current traffic with
large margin, has continuous DMA on the Daisy, and does not compete with the
planned Stage B CV SPI path.

If a measurement justifies revisiting SPI:

1. Fix and validate the Phase 3 CV SPI peripheral/pin plan first, so the two
   buses are designed together rather than competing accidentally.
2. Prefer a separate, minimal P4-master/Daisy-slave proof using SPI1, hardware
   NSS, DMA-safe 256-byte buffers, and no WaveX protocol.
3. Apply the small slave-specific libDaisy changes above or use the HAL directly
   for the proof.
4. Require a zero-error soak, audio-underrun counters, DWT/logic-analyzer timing,
   peer-reboot recovery, and simultaneous CV-update testing before choosing a
   production topology.
5. Keep the experiment separate from the live UART configuration until the
   measured gate passes. Source fixes alone do not authorize enabling SPI.

The planned CV DAC bus is therefore a valid additional reason to avoid SPI for
the MCU link, but not because the H750 lacks multiple SPI peripherals. The real
constraint is that libDaisy funnels their DMA through one shared stream pair,
while WaveX currently has no measured need for the extra link bandwidth.

## Related

- [`architecture.md`](architecture.md) — current UART transport and dormant SPI topology
- [`features/inter-mcu-protocol.md`](features/inter-mcu-protocol.md) — live wire contract
- [`features/analog-voice-board.md`](features/analog-voice-board.md) — planned Stage B CV bus
- [`daisy_rt_audio_coding_guide.md`](daisy_rt_audio_coding_guide.md) — DMA, cache, ISR, and audio rules
- [`backlog.md`](backlog.md) — SPI revival gates and rationale
