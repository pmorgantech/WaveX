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
- **Regions** - the RAM split by STM32H750 memory region, from the ELF's
  allocated sections (VMA): DTCM (128 KiB, stacks and callback state),
  SRAM (512 KiB AXI), D2DMA (32 KiB, DMA buffers), D2/D3, ITCM (code),
  BKP. This is the linker's `--print-memory-usage` accounting, so it is the
  column to read when a change moves state between regions rather than
  growing it. Empty on rows backfilled before the column existed.
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

| Date | Commit | Image | text | data | bss | Flash | RAM | Regions | Note |
|---|---|---|---:|---:|---:|---:|---:|---|---|
| 2026-09-03 | c307ee2 | daisy | 301824 | 75320 | 332532 | 377184 (4.6%) | 407852 (77.8%) |  | v0.4.0 release |
| 2026-09-04 | 9e37f65 | daisy | 312280 | 126084 | 333620 | 438404 (5.4%) | 459704 (87.7%) |  | baseline before the image-slimming series (sequencer transport added ~50 KB of .data) |
| 2026-09-04 | 95b2387 | daisy | 308600 | 126084 | 333620 | 434724 (5.3%) | 459704 (87.7%) |  | region fades prepared per block; float cos table (-4 KB double libm) |
| 2026-09-04 | 4e5ba4b | daisy | 298128 | 125724 | 333620 | 423892 (5.2%) | 459344 (87.6%) |  | SFZ ParseFloat without strtof (-8 KB strtod/mprec, drops malloc from import) |
| 2026-09-04 | 8e8c1f8 | daisy | 294200 | 125716 | 333620 | 419956 (5.2%) | 459336 (87.6%) |  | UART_LOGx and stray printf into the log ring (-5 KB buffered stdio) |
| 2026-09-04 | 7fa9eae | daisy | 294152 | 125716 | 333620 | 419908 (5.2%) | 459336 (87.6%) |  | DaisySP and CMSIS-DSP shim no longer compiled (arm_copy_q15 -> memcpy) |
| 2026-09-04 | 0671bc2 | daisy | 294160 | 125716 | 333620 | 419916 (5.2%) | 459336 (87.6%) |  | resampler phase split in 32-bit ops (drops float->int64 libgcc call) |
| 2026-09-04 | ca6e6d3 | daisy | 292624 | 124448 | 333588 | 417112 (5.1%) | 458036 (87.4%) |  | linker script no longer pulls full libc.a ahead of libc_nano |
| 2026-09-04 | a248706 | daisy | 284696 | 124444 | 331488 | 409180 (5.0%) | 455932 (87.0%) |  | SD volume linked directly; USB host MSC stack gone |
| 2026-09-04 | be2d86d | daisy | 289040 | 2628 | 453344 | 291708 (3.6%) | 455972 (87.0%) |  | large defaults constructed into .bss instead of shipped as .data |
| 2026-09-04 | 479db13 | daisy | 270392 | 2628 | 453308 | 273060 (3.4%) | 455936 (87.0%) |  | cold TUs at -Os; SRAM debug layout refitted |
| 2026-09-04 | 9f8f453 | daisy | 270392 | 2628 | 453308 | 273060 (3.4%) | 455936 (87.0%) |  | size log added (no firmware change) |
| 2026-09-04 | 343054e | daisy | 270664 | 2628 | 453500 | 273332 (3.4%) | 456128 (87.0%) |  | SVF 12/24 dB slope + soft-clip drive (+272 B; both default off) |
| 2026-09-04 | 88fa2e7 | daisy | 275480 | 2660 | 454316 | 278180 (3.4%) | 456976 (87.2%) |  | VoiceFilter A/B switch, daisysp::Svf linked (svf.cpp only, +1808 B) + WAVEX-FILTER console verb |
| 2026-09-04 | b571443 | esp32 app |  |  |  | 1007392 |  |  | ESP32 baseline before disabling the LVGL sysmon |
| 2026-09-04 | 84453a0 | esp32 app |  |  |  | 1004944 |  |  | LVGL sysmon/perf/mem monitors off |
| 2026-09-04 | 88fa2e7 | daisy | 275480 | 2660 | 454316 | 278180 (3.4%) | 456976 (87.2%) | ITCM 528B / DTCM 2.9K (2%) / SRAM 422.6K (83%) / D2DMA 20.0K (63%) / BKP 12B | same image as the row above, re-measured to fill in Regions; baseline for the -O2 series |
| 2026-09-04 | 75145f3 | daisy | 273072 | 2660 | 460748 | 275772 (3.4%) | 463408 (88.4%) | ITCM 528B / DTCM 2.9K (2%) / SRAM 428.9K (84%) / D2DMA 20.0K (63%) / BKP 12B | last obj = T{} temporaries reconstructed in place; usage reply bounded |
| 2026-09-04 | 75145f3 | daisy -O3 (measured, not adopted) | 216212 | 2564 | 460792 | 218816 (2.7%) | 463356 (88.4%) | DTCM 2.9K (2%) / SRAM 428.9K (84%) / D2DMA 20.0K (63%) / BKP 12B | same sources as 75145f3 built -O3, for comparison with the -O2 row below |
| 2026-09-04 | 6ac460e | daisy | 187032 | 2564 | 460788 | 189636 (2.3%) | 463352 (88.4%) | ITCM 184B / DTCM 2.9K (2%) / SRAM 428.9K (84%) / D2DMA 20.0K (63%) / BKP 12B | WAVEX_DAISY_OPT -O0 -> -O2 (real-time path); text -86 KB, RAM unchanged |
| 2026-09-04 | 6ac460e | daisy stage B | 186376 | 2884 | 459612 | 189260 (2.3%) | 462496 (88.2%) | ITCM 184B / DTCM 2.9K (2%) / SRAM 428.0K (84%) / D2DMA 20.0K (63%) / BKP 12B | Stage B flag set at -O2 |
