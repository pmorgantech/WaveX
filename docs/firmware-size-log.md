# Firmware size log

One row per measured build, appended by `scripts/firmware_size.py --record
"<note>"` from the devcontainer after `make daisy` (and `make esp32` when the
ESP32 row is wanted). The point is the trend: every feature lands with a row,
so growth is attributable and a regression shows up as a step, not as a
surprise at flash time.

Columns (Daisy):

- **text / data / bss** - `arm-none-eabi-size` on `wavex-daisy.elf`, bytes.
- **Flash** - the `.bin` that is flashed (text + data + vector padding), and
  its share of the 7936 KiB QSPI region. `.data` counts here because its
  initial image is stored in QSPI and copied to SRAM at boot.
- **RAM** - data + bss, and its share of the 512 KiB AXI SRAM. This counts
  the D2 DMA buffers (`.sram1_bss`, ~22 KiB) that the linker places outside
  AXI SRAM, so it reads ~4 points higher than the linker's own `SRAM:` line;
  the trend is what matters, and it is consistent.
- **Commit** - the tree the build came from; a trailing `+` means the
  working tree was dirty when recorded.

The ESP32 row records only the app `.bin` size (`firmware/esp32/build/`).

Rows before 2026-09-04 15:00 were backfilled by rebuilding each commit in a
worktree with the same toolchain; the Daisy build is deterministic enough
that they match the sizes recorded in the commit messages byte for byte.

## Reading the big steps

- `9e37f65`: `.data` 75 KB -> 126 KB. The sequencer transport is a static
  whose defaults are non-zero, so its whole 50 KB image went to flash and
  was copied to SRAM at boot. `be2d86d` is the fix (`bss_static.hpp`).
- `be2d86d`: flash -117 KB, `.data` 126 KB -> 2.6 KB, `.bss` +120 KB (same
  RAM, no longer duplicated in flash).
- `479db13`: -18 KB text from `-Os` on the six main-loop-only sources. The
  real-time path is still `-O0` (`WAVEX_DAISY_OPT`); see `docs/backlog.md`.

## Log

| Date | Commit | Image | text | data | bss | Flash | RAM | Note |
|---|---|---|---:|---:|---:|---:|---:|---|
| 2026-09-03 | c307ee2 | daisy | 301824 | 75320 | 332532 | 377184 (4.6%) | 407852 (77.8%) | v0.4.0 release |
| 2026-09-04 | 9e37f65 | daisy | 312280 | 126084 | 333620 | 438404 (5.4%) | 459704 (87.7%) | baseline before the image-slimming series (sequencer transport added ~50 KB of .data) |
| 2026-09-04 | 95b2387 | daisy | 308600 | 126084 | 333620 | 434724 (5.3%) | 459704 (87.7%) | region fades prepared per block; float cos table (-4 KB double libm) |
| 2026-09-04 | 4e5ba4b | daisy | 298128 | 125724 | 333620 | 423892 (5.2%) | 459344 (87.6%) | SFZ ParseFloat without strtof (-8 KB strtod/mprec, drops malloc from import) |
| 2026-09-04 | 8e8c1f8 | daisy | 294200 | 125716 | 333620 | 419956 (5.2%) | 459336 (87.6%) | UART_LOGx and stray printf into the log ring (-5 KB buffered stdio) |
| 2026-09-04 | 7fa9eae | daisy | 294152 | 125716 | 333620 | 419908 (5.2%) | 459336 (87.6%) | DaisySP and CMSIS-DSP shim no longer compiled (arm_copy_q15 -> memcpy) |
| 2026-09-04 | 0671bc2 | daisy | 294160 | 125716 | 333620 | 419916 (5.2%) | 459336 (87.6%) | resampler phase split in 32-bit ops (drops float->int64 libgcc call) |
| 2026-09-04 | ca6e6d3 | daisy | 292624 | 124448 | 333588 | 417112 (5.1%) | 458036 (87.4%) | linker script no longer pulls full libc.a ahead of libc_nano |
| 2026-09-04 | a248706 | daisy | 284696 | 124444 | 331488 | 409180 (5.0%) | 455932 (87.0%) | SD volume linked directly; USB host MSC stack gone |
| 2026-09-04 | be2d86d | daisy | 289040 | 2628 | 453344 | 291708 (3.6%) | 455972 (87.0%) | large defaults constructed into .bss instead of shipped as .data |
| 2026-09-04 | 479db13 | daisy | 270392 | 2628 | 453308 | 273060 (3.4%) | 455936 (87.0%) | cold TUs at -Os; SRAM debug layout refitted |
