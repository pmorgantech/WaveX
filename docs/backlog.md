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
