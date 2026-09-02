# SPI Notes: libDaisy 8.1 and the WaveX Inter-MCU Link

These engineering notes record what libDaisy 8.1.0 actually supports, how to
bring up an STM32H750 SPI slave without relying on assumptions, and what the
dormant WaveX SPI link tells us about its earlier instability. They are a
reference for a future measured SPI experiment, not a decision to replace the
live UART transport.

> **Status:** The WaveX transport of record remains full-duplex UART at 2 Mbaud.
> There is a single shared flag, `WAVEX_SPI_LINK_ENABLED`, currently `0` and
> governing both sides — the ESP32 header defers to the shared config rather
> than carrying its own. The dormant implementation is not safe to revive by
> changing that flag alone.

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

The dormant WaveX link is **not** the slave arrangement above. Its present
topology is:

- Daisy SPI1 master
- ESP32-P4 SPI3 host in slave mode
- mode 0, full duplex, software-controlled chip select on the Daisy
- an ESP-to-Daisy ATTN GPIO for slave-originated traffic
- logical packets of 32 to 2048 bytes carried in fixed 2048-byte DMA
  transactions on the active DMA path

The Daisy code is in
[`daisy_spi_link.cpp`](../firmware/daisy/src/comm/daisy_spi_link.cpp); the P4
code is in
[`esp_spi_link.cpp`](../firmware/esp32/main/links/esp_spi_link.cpp).

This means the libDaisy slave NSS-pulse concern was not active in the failed
WaveX configuration. The project used SPI1, not SPI6, and its DMA buffers were
eventually moved into DMA-safe memory. Those are useful exclusions: the
remaining failure evidence points primarily to transaction ownership,
handshake races, recovery, and interrupt policy in the WaveX link layer.

## Why the old link was unstable

The repository history moves from “working bidirectional SPI, with some
bugginess,” through “DMA hangs sometimes,” to “SPI link still buggy” before the
UART migration. The current dormant source retains several mechanisms that can
produce exactly those symptoms.

### ESP32 transaction ownership is broken

`spi_slave_task()` queues a transaction, waits only 50 ms for its result, and
continues around the loop on timeout. A timeout does not cancel or return the
queued transaction. The next iteration rotates to another pool entry and
queues again; after enough timeouts it also reuses descriptors and buffers that
the SPI driver may still own.

Consequences include a full driver queue, stale zero-filled transactions ahead
of a newly arrived message, DMA buffer corruption, and apparent random hangs.
This is the main source-level explanation for the observed instability. The
six revival blockers recorded in [`backlog.md`](backlog.md) add missing
sequence gating, unsafe capacity handling, sequence wrap, actual-versus-configured
length confusion, and an unaudited ISR callback path. All must be resolved as
one ownership design before bench revival.

### The ATTN handshake can lose a request

The Daisy ATTN interrupt calls `Spi_ReceivePacket()` immediately. If a Daisy TX
DMA operation is already active, receive is rejected. ATTN remains level-high,
so there is no second rising edge, and the main loop does not call the existing
level-poll retry function. The P4 eventually deasserts ATTN through its watchdog
without delivering the message.

Starting a large `memset`, packet preparation, DMA initialization, and logging
from the EXTI handler also makes the interrupt path much larger than it needs to
be. The ISR should publish a flag; the foreground transport state machine
should own the transfer.

### Daisy timeout recovery does not recover the peripheral

The Daisy timeout helper is not called by the main loop. Even if it were, it
only deasserts chip select and clears one software in-flight flag. It does not
abort/reset the HAL DMA transaction, clear libDaisy's global DMA owner, cover
the duplex in-flight state, or reclaim a queued job. A single real DMA/HAL
wedge can therefore make all later transfers wait forever.

### SPI can preempt audio at the highest priority

`HAL_SPI_MspInit()` assigns the SPI peripheral IRQ priority 0. WaveX lowers the
DMA stream priorities before SPI initialization, but it never lowers the SPI1
peripheral IRQ after libDaisy raises it. A busy or faulty link can therefore
preempt the audio callback, contrary to the project's audio-first interrupt
hierarchy. This is a plausible contributor to the historical playback freezes,
although only a hardware trace can prove which freezes it caused.

### Framing and delivery semantics are inconsistent

The DMA path clocks 2048 bytes even for a small logical packet. Outgoing
messages are removed when DMA launch succeeds rather than when wire completion
succeeds, while a message piggybacked on an ATTN-driven duplex transfer is not
consumed from the same queue. These rules allow loss or duplicate delivery
around errors. The P4 path also caps locally created payloads below the size of
the browse pages SPI was meant to accelerate.

The conclusion is not that SPI or libDaisy DMA is inherently unreliable. The
dormant link combines a complicated bidirectional ATTN protocol, unclear DMA
buffer ownership, incomplete cancellation, and inconsistent completion
semantics. A small fixed-frame transport proof avoids all of those at once.

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
5. If the one-day fixed-frame proof is not clean, stop. Do not debug the dormant
   2,000-line link layer or rewrite the SPI driver first.

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
