# Backlog

Work that is worth doing but is not scheduled into a roadmap phase. Items here
are deliberately *not* in [`roadmap.md`](roadmap.md): that file tracks the
phased build-out of the instrument, and mixing opportunistic engineering work
into it makes the phase gates harder to read.

Each entry should record **why it is not urgent**, so a future reader can tell
whether the reasoning still holds. An item whose justification has expired is
more useful than an item with no justification at all.

---

## Runtime-tunable debug logging (bitmask)

**Want:** a 32- or 64-bit mask of debug toggles in one config header, so
logging can be tuned per subsystem for a given investigation and compiles away
entirely in release builds.

**Current state:** `firmware/shared/config/logging_config.h` already carries
**39** individual `WAVEX_LOG_*` / `*_DEBUG` macros, each `#ifndef`-guarded and
overridable from the build (`make daisy CMAKE_EXTRA_ARGS="-DCMAKE_CXX_FLAGS=-DWAVEX_DAISY_SD_DEBUG=1"`).
They already compile away when zero. So the *capability* mostly exists; what is
missing is discoverability and a single place to flip several at once.

**Why it is not urgent:** the existing macros solve the compile-away
requirement, which is the part that matters for release builds. A bitmask would
mainly improve ergonomics. Converting 39 call sites across both firmwares is a
wide, mechanical change with real regression surface — the kind of churn that
has repeatedly turned out to be where bugs enter this codebase.

**If picked up, the shape that seems right:**

- One `WAVEX_DEBUG_MASK` constant (`uint64_t`) plus named bit constants, so a
  build override sets several categories at once.
- Keep every log site wrapped so a zero mask still compiles the code away:
  `#if (WAVEX_DEBUG_MASK & WAVEX_DBG_SD)` rather than a runtime `if`, because a
  runtime check leaves the format strings and argument evaluation in the binary.
- Migrate one subsystem at a time, leaving the existing macros as aliases, so
  the change is bisectable.
- Runtime tuning (as opposed to build-time) needs a transport to set the mask
  and would put a variable on the audio-adjacent path — worth a separate
  decision, and probably not worth it at all given the log ring already keeps
  logging off the critical path.

---

## Self-implemented bidirectional SPI link (replacing UART4)

**Want:** a faster, DMA-driven, non-blocking link between the MCUs, ideally
with the ESP32 as master since most control actions originate there.

**Numbers, for the record:**

| | UART4 (today) | SPI1 (hypothetical) |
|---|---|---|
| Rate | 2 Mbaud, 8N1 → **200 KB/s** | 25–50 MHz → **3–6 MB/s** |
| 64-byte frame | ~320 µs | ~20 µs |
| Transport | full-duplex DMA, IRQ priority 7 | DMA, would need the same |

**Why it is not urgent:** bandwidth is not the constraint. The link carries
1 Hz heartbeats, meters, occasional ~200-byte browse responses and note events —
a rounding error against 200 KB/s. Measured during sample playback, the main
loop held `dt=5000` ms exactly with `blocks=+5001` (audio callback at its
nominal 1 kHz) and CPU at 4–7%, i.e. no evidence the link costs the audio
engine anything. `WAVEX_DAISY_UART_PERF_DEBUG` now measures this directly.

**Trigger to reconsider:** a measurement showing the link is the bottleneck.
The plausible candidates are waveform streaming to the display and bulk sample
transfer into SDRAM.

**Constraints if picked up:**

- libDaisy's `SpiHandle::Config` does have a `SLAVE` enumerator, so
  ESP32-as-master is at least expressible — but **slave mode with DMA plus the
  ATTN handshake is unvalidated**, and that needs proving before anything
  depends on it.
- The existing dormant SPI link (`WAVEX_SPI_LINK_ENABLED`, currently `0`)
  already solves ESP32-originated messages the other way: an ATTN line on
  `EXTI15_10` at priority 14.
- Keep it flag-gated exactly as the SPI code is now, with UART remaining the
  default, so reverting is a build flag rather than a revert.

---

## Non-frame-aligned WAV data chunks

**Want:** files whose `data` chunk payload starts at an offset not divisible by
the frame size should play correctly.

**Observed:** `data_start % 4 == 2` correlates exactly with audible artefacts
across the files tested (182 → stutters, 268 → clean, 262 → stutters), while
`data_start % 512` is non-zero in all of them and does not discriminate. Such
offsets are legal RIFF — chunks only require *even* alignment, and a `LIST` or
`fact` chunk of odd length before `data` produces them.

**Not yet understood:** reading from `data_offset` should be frame-correct
regardless of its alignment, and `ReadSample16()` assembles samples byte-wise,
so there is no CPU alignment fault. The mechanism between FatFS's partial-sector
window path and the conversion is not identified. **Do not "fix" this by
rounding `data_start` up to a frame boundary** — that is only correct if the
parser is landing 2 bytes off, which is unproven, and would misalign genuinely
odd-offset files.

**Workaround meanwhile:** re-encode so `data` lands at offset 44.

```bash
ffmpeg -i in.wav -map_metadata -1 -fflags +bitexact -flags:a +bitexact \
  -c:a pcm_s16le in.fixed.wav
```

---

## SD remount after card insertion

Explicit `HAL_SD_Init()` + settle delay landed, along with a fix for
`SD_initialize()` masking failures behind a stale `Stat`. Whether the
underlying re-identification now succeeds is unconfirmed on hardware; the log
will name the HAL error if it does not.

---

## GT911 touch range mismatch

`display_manager.cpp` passes `x_max = 800, y_max = 480` to the GT911 driver,
which matches neither the native panel (720×1280,
`CONFIG_BSP_LCD_TYPE_720_1280_5_INCH_A`) nor the rotated canvas (1280×720).
The values look inherited from an 800×480 board variant.

**Why it is not urgent:** touch has been working well enough that nobody
noticed — the GT911 reports its own coordinates and the softkey targets are
large. But if touch ever feels offset, compressed, or dead near two edges,
this is the first suspect (verify by tapping all four corners). Do not fix
blind: change it with the panel attached and corner-tap before/after, since
the correct values depend on how the driver interacts with the panel's own
configuration.

---

## ITCM placement for the per-voice render loop

**Want:** `VoiceManager::Render()` in ITCM (`WAVEX_ITCM_CODE`). It is the
hottest code in the system — an inner loop over 8 voices × every frame of
every block, running in the audio callback — and its *data* is already in DTCM
(`s_voice_manager WAVEX_DTCM_DATA`). Instruction fetch is the half that is
still going through the cache from QSPI-backed flash, where a miss is
expensive and, worse, variable.

**Current state:** the mechanism exists and is proven — `memory_sections.h`
defines `WAVEX_ITCM_CODE`, `wavex_memory_sections.ld` copies the section from
its QSPI load image, `main.cpp` calls `InitItcm()` before IRQs are enabled, and
`uart4_dma_transport.cpp`'s `ProcessRxPosition` already uses it. ITCMRAM is
528 B of 64 KB — essentially empty.

**Why it is not urgent — and what would have to be solved first:**

1. **No number justifies it.** `daisy_rt_audio_coding_guide.md` and AGENTS.md
   both require a DWT cycle-counter measurement before claiming a placement
   win. Nobody has measured the callback with and without, so moving it now
   would be a guess that happens to be plausible. This is the same standard
   the existing DTCM placements are held to, and they have not met it either
   (see roadmap § Outstanding hardware verification).
2. **The attribute does not fit where the code lives.** `Render()` is an inline
   member function of `voice_manager.hpp`, which is deliberately HAL-free and
   compiled on the host for `voice_manager_test`. `WAVEX_ITCM_CODE` is a GCC
   section attribute naming a section that exists only in the Daisy linker
   script, so applying it there needs per-target guarding, and a section
   attribute on an inline function in a header is fragile besides (it applies
   per translation unit, and the linker is free to keep only one copy).
   Solving this probably means moving the hot loop out of line into a `.cpp`
   compiled only for the target — which costs the host-testability that makes
   this class easy to work on.

**When to revisit:** when there is a DWT profile of the audio callback. If the
callback has comfortable headroom, leave it alone — the host-testability is
worth more than an unmeasured win. If it is tight, item 2 becomes worth paying
for, and it should be measured before *and* after the restructure so the
out-of-lining and the placement are not credited to each other.

---

## SPI-link revival is gated on five recorded defects

**Want:** when the SPI link is re-enabled (`WAVEX_SPI_LINK_ENABLED`,
`link_config.h` — decision of 2026-07-05 made UART the transport of record),
the quarantined `esp_spi_link.cpp` must first be fixed.

**Current state:** the 2026-08-29 ESP32 review recorded five blockers as
SPI-1..SPI-5 in
[`code_review_esp32_20260829.md` §7](code_review_esp32_20260829.md): driver-owned
transaction descriptors/RX buffers reused on result timeout, no sequence
gating on the live SPI RX path, an uninitialized in/out capacity that can
overflow a 220-byte stack buffer, an 8-bit TX sequence wrapping through the
reserved value 0, and configured-vs-actual transfer length confusion.

**Why it is not urgent:** the code is compiled out of every image today, so
none of it is reachable. It becomes urgent the moment anyone flips the flag —
which is why it is recorded here rather than fixed opportunistically: fixing
dead code cannot be verified on hardware, and the project's standard is not to
claim fixes without a way to observe them.

**When to revisit:** at SPI revival planning. Copy SPI-1..SPI-5 into that
roadmap item's gate before any bring-up work starts.
