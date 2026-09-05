# RT1170 Migration Plan — Daisy Seed → PHYTEC phyCORE-i.MX RT1170

**Status**: Draft planning document. Not yet wired into `roadmap.md`; nothing here is authorized for implementation against the current roadmap phase until scope is confirmed and this plan is folded into a tracked phase.
**Activation gate**: This plan remains dormant while the recurring callback-headroom gate in `performance_monitoring.md` stays below 80%. A measured worst callback at or above 80% with callback-resident roadmap features still outstanding changes the gate result to **UPGRADE** and is the trigger to fold this plan into the roadmap; component specifications alone are not.
**Scope decision (recorded)**: **Like-for-like.** The phyCORE-RT1170 replaces the Daisy Seed's role only — real-time audio engine, SD sample streaming, SDRAM sample RAM, CV/Gate output. The ESP32-P4 keeps the UI, display, touch, and MIDI I/O exactly as today. The inter-MCU UART link is ported, not redesigned.
**Software stack decision (recorded)**: **Bare-metal, no RTOS**, built directly against NXP's MCUXpresso SDK drivers (not PHYTEC's Zephyr BSP). This preserves `architecture.md` §4.2's documented rationale — the audio timebase is the DMA clock, not a scheduler — which an RTOS would reintroduce jitter risk against.
**Research basis**: hardware/ecosystem findings below come from a web-research pass (2026-08-30) against NXP and PHYTEC primary docs plus community sources; items marked *(unconfirmed)* should be re-checked against the RT1170 reference manual and PHYTEC schematics before implementation, not treated as settled.

---

## 1. Why this is a bigger change than a part swap

There is **no libDaisy equivalent** for the RT1170 — no focused, audio-callback-first hardware abstraction library. PHYTEC's only published, versioned BSP for this SOM is Zephyr, which this plan deliberately does not adopt. That means the board-support layer libDaisy currently gives us for free (clock tree init, pin mux, `AudioHandle` double-buffered SAI callback, `SdmmcHandler`, DMA buffer placement) has to be **written from scratch** against NXP's MCUXpresso SDK peripheral drivers, using PHYTEC's schematics/pinout for the module and carrier board as the wiring reference. Budget this as the majority of the migration effort — porting `audio_engine.cpp`'s DSP logic is comparatively small.

## 2. Hardware comparison (as-built Daisy Seed vs. phyCORE-RT1170)

| Property | Daisy Seed (STM32H750) | phyCORE-i.MX RT1170 (RT1176) | Notes |
|---|---|---|---|
| Core | Cortex-M7 @ 480 MHz | Cortex-M7 @ up to 1 GHz + Cortex-M4 @ 400 MHz | Plan uses M7 only; M4 is a future option (§8), not part of this migration |
| L1 cache | 16 KB I / 16 KB D | 32 KB I / 32 KB D | Cache line 32 B on both — existing §7 DMA alignment rules carry over unchanged |
| Tightly-coupled RAM | 64 KB ITCM / 128 KB DTCM (fixed) | 512 KB FlexRAM, software-configurable into ITCM/DTCM/OCRAM in 32 KB blocks, plus fixed OCRAM1/OCRAM2 *(sizes unconfirmed — verify against RM)* | RT1170's split is a build-time config decision we own, not a silicon constant |
| External RAM | 64 MB SDRAM (Daisy Seed) | **64 MB SDRAM on the SOM itself** (matches current allocator design almost exactly) | `memory.h`'s 60 MB arena + 4 MB render-scratch split needs no size change, only address/controller retargeting (SEMC instead of STM32 FMC) |
| External flash | 8 MB QSPI (BOOT_QSPI) | 16 MB Octal/Quad SPI NOR on the SOM (FlexSPI) | More headroom than today |
| Audio interface | SAI1 (stereo codec), SAI2 planned TDM-8 | 4× SAI blocks (I2S/TDM/DSP mode); *(unconfirmed)* only SAI1 may support full 8-ch TDM | Confirm before assuming Stage B's PCM1690 TDM-8 plan ports as-is |
| DMA | DMA1/DMA2 + MDMA, cache-coherency rules per §7.1 | eDMA (2 engines service SAI) | Same class of non-cacheable-region-or-explicit-maintenance discipline; NXP's own guidance describes the identical pattern |
| SD/storage | SDMMC 4-bit + FatFs | USDHC (SD/eMMC) + FatFs (need a diskio port) | microSD slot lives on the **carrier board** (phyBOARD-RT1170), not the SOM |
| Inter-MCU link | UART4 DMA (custom `uart4_dma_transport`), SPI1 (compiled out) | LPUART + eDMA (equivalent capability) | Port the transport, not the protocol — `protocol.h`/`uart_protocol.h` are HAL-free and untouched |
| Audio codec | Onboard (Daisy Seed built-in codec) | External, on carrier — **part number not yet identified from public PHYTEC docs**, NXP's own EVK uses WM8960 | Must pull PHYTEC schematics before designing the I2C/SAI init sequence |
| No onboard codec/DAC on the RT1170 itself | — | Confirmed — same "pair with external codec" model as today | Not a new problem class |
| Debug/flash tooling | ST-Link (OpenOCD) + DFU (dfu-util), scripted in `firmware/daisy/Makefile` | Likely J-Link/PyOCD or NXP's serial-downloader (`blhost`)/SD-card boot — **not yet decided** | New Makefile flash targets needed regardless of choice (§6) |

## 3. Non-goals (explicitly out of scope for this plan)

- Replacing the ESP32-P4 UI/display/touch stack. (All-in-one was considered and rejected — see the research findings: it would bolt a second full migration, LVGL/MIPI-DSI on unproven RT1170 tooling, onto the audio-engine port.)
- Adopting Zephyr or any RTOS on the audio-engine core.
- Using the RT1176's M4 core in the first pass.
- Redesigning the inter-MCU wire protocol. `protocol.h` and the UART framing are reused unchanged; only the transport-layer driver underneath is rewritten.
- Changing the 48 kHz / 48-sample-block / 1 kHz control-tick invariant (`timebase.hpp`'s `static_assert`), the SDRAM sample-arena size, or the CV backend architecture (`CvGroupRouter`, Stage A/B split).

## 4. Library changes

| Library | Today (Daisy) | RT1170 plan | Action |
|---|---|---|---|
| `libDaisy` (submodule, `firmware/daisy/libs/libDaisy`) | Board init, `AudioHandle`, `SdmmcHandler`, HAL wrappers | No equivalent exists | **Remove.** Its role is replaced by a new WaveX-owned board-support module written against MCUXpresso SDK drivers (§7). |
| `DaisySP` (submodule) | Portable DSP algorithm library, no board dependency beyond libDaisy's build glue | Mostly reusable as-is — it's generic C++ DSP, not STM32-specific | **Keep if used** — audit actual call sites first; WaveX's own `audio/svf_filter.hpp`/`envelope.hpp` may already have superseded most of what DaisySP provided. Don't assume it's needed without checking. |
| CMSIS-DSP (vendored inside libDaisy's `Drivers/CMSIS-DSP`, pinned commit `3a04f817`, v1.14.4) | Source-only, portable; only `arm_linear_interp_q15` + conversion helpers are actually linked | Fully portable — CMSIS-DSP has no chip dependency beyond `ARM_MATH_CM7` and a correct `core_cm7.h` | **Re-vendor from a standalone source** (ARM's CMSIS-DSP repo directly, same pinned commit/version for continuity) since it's no longer riding along inside libDaisy. Reuse the exact same `CMSIS_DSP_SOURCES` allowlist in CMakeLists — same functions are needed. |
| NXP MCUXpresso SDK | Not present | **New dependency.** Provides device headers, IOMUXC/clock/eDMA/SAI/LPUART/LPI2C/USDHC/SEMC drivers for RT1170. | Add as a submodule/vendored drop, scoped to only the RT1170 device + drivers actually used (mirrors how libDaisy's CMSIS includes were hand-fixed in `CMakeLists.txt:92-113` — expect similar include-path surgery). |
| CMSIS Core / device headers | From libDaisy's bundled CMSIS_5 + ST's STM32H7xx device pack | From NXP's device pack (bundled with MCUXpresso SDK) | Same class of "don't let two device headers collide" problem already solved once for libDaisy (`CMakeLists.txt:92-113`); expect to solve it again for the SDK's own CMSIS layer. |
| FatFs | Bundled via libDaisy, thin diskio glue in `sd_sdio.cpp` | FatFs itself is portable and NXP's SDK ships its own FatFs+SDMMC example glue as a reference | **Keep FatFs, port the diskio layer** (`sd_sdio.cpp`) from libDaisy's `SdmmcHandler` to NXP's USDHC driver. Existing `fs_browse.cpp` (pure FatFs API usage) should need no changes. |
| `firmware/shared/*` (protocol.h, uart_protocol.h, sequence_tracker, wxcf) | HAL-free, C++17 | Unchanged | **No changes.** This is the entire point of the shared/ split — verify host tests stay green as the proof. |

## 5. Build system changes

- **New toolchain file.** NXP's SDK ships `armgcc.cmake` toolchain files for plain CMake + `arm-none-eabi-gcc` builds (same compiler already installed in the devcontainer for the Daisy build — no new apt package needed). Per community reports, the SDK-generated file has historically embedded machine-specific absolute paths *(unconfirmed against the version we'd actually vendor)* — budget time to make it portable the way `firmware/daisy/libs/libDaisy/cmake/toolchains/ArmGNUToolchain.cmake` already is.
- **New CMakeLists.txt** for the target (likely `firmware/rt1170/CMakeLists.txt` or in-place at `firmware/daisy/CMakeLists.txt` if the directory is kept — see §9 naming decision). Mirrors today's structure: read the root `VERSION` file, set `-Wall -Wextra -Wconversion` on first-party sources only (vendor SDK/CMSIS-DSP marked `SYSTEM`, exactly as done today for `daisy`/`DaisySP`/`STM32H7xx_HAL_Driver`), same `-Wl,-T,<linker-script>` pattern.
- **New linker script.** RT1170's memory map (FlexRAM/OCRAM addresses, SEMC SDRAM base, FlexSPI flash base) is entirely different from `wavex_memory_sections.ld`'s STM32H750 addresses. This has to be written from the RT1170 reference manual / NXP SDK example linker scripts, not adapted line-by-line from the existing one.
- **New Makefile wrapper**, structurally parallel to `firmware/daisy/Makefile`: `configure`/`build`/`clean`/`bin` stay conceptually the same; `flash`/`dfu`/`auto-flash` need new tooling (§6 decision) and a new serial/USB VID:PID for board detection (`scripts/serial_ports.py` already has a resolver pattern to extend, not replace).
- **Top-level `Makefile` targets.** `make daisy`/`make daisy-flash`/etc. either get parallel `rt1170` targets (if both boards are kept buildable during transition, recommended — see §9) or are retargeted once the swap is final.
- **Devcontainer (`​.devcontainer/Dockerfile`).** `gcc-arm-none-eabi` is already installed and is reused. New additions needed: NXP SDK dependencies (likely none beyond what's already there, since the SDK itself is vendored as source, not a system package) and whatever flashing tool is chosen (§6) — e.g. `pip install pyocd` or a J-Link SDK package (proprietary EULA — check before baking into a shared image) or NXP's `spsdk`/`blhost` for serial-downloader flashing.
- **CI.** `make test` (host tests, GoogleTest, HAL-free) should need **zero changes** — that's the value of the `firmware/shared` split and of keeping DSP/protocol code HAL-free. Hardware-dependent compile checks (`make daisy` / `make daisy-stageb` in CI today) get an equivalent `make rt1170` compile-only gate.

## 6. Flashing/debug tooling (open decision)

Today's flow is ST-Link + OpenOCD (`option_write` boot-mode dance) or DFU over USB CDC (`dfu-util`), fully scripted including a touchless auto-flash trigger. RT1170 doesn't have an ST-Link-compatible debug probe or a DFU bootloader by default. Realistic options, needing a decision before §7 work can be tested on real hardware:

1. **J-Link/PyOCD + SWD** — standard ARM debug probe workflow, well-documented for i.MX RT parts, but needs a physical J-Link (or CMSIS-DAP-compatible) probe purchased for the bench.
2. **NXP serial downloader (ROM bootloader) via `blhost`/`spsdk`** — no extra probe hardware needed (USB or UART into the boot ROM), closer in spirit to today's DFU flow, but a different toolchain (`spsdk` is Python-based, NXP-maintained).
3. **SD-card boot** — reuses the microSD slot already on the carrier for the dev/flash cycle, useful as a fallback but slow for iteration.

**Recommendation**: J-Link/PyOCD for bring-up (best debugging visibility during the hardest phase — clock/pin/SAI init), evaluate `blhost` for a touchless production-style flash once bring-up is stable. Needs a probe purchase either way; flag as a bench prerequisite before Stage 1 (§8) can start.

## 7. Code changes

### 7.1 New board-support layer (the bulk of the work)

Everything libDaisy currently gives us for free has to be written, scoped tightly to what WaveX actually uses (not a general-purpose HAL):

- **Clock tree init** — PLLs, core/bus clock dividers, targeting the same or better margin than today's 480 MHz H750 config. Use NXP SDK's `clock_config` generator/examples as the starting point, not hand-rolled register writes.
- **Pin mux (IOMUXC)** — replaces STM32 `Pin`/AF-mux setup. This is where PHYTEC schematics become load-bearing: SAI pins, LPI2C (MCP4728 CV DAC), LPUART (ESP32 link), USDHC (SD), GPIO (any discrete I/O) all need the phyCORE/phyBOARD's actual net names, not the RT1170 EVK's.
- **SAI + eDMA audio callback** — the single most important piece of new code. Must reproduce libDaisy's `AudioHandle` double-buffer (ping-pong) semantics: a fixed-size block completes, an interrupt fires, the next block is already streaming via DMA while the callback processes the completed one. This is what the entire `timebase.hpp` invariant (1 block = 1 ms = 1 control tick) depends on — get this exactly right before porting any DSP code on top of it, and DWT-measure it in isolation (RT1170's M7 has the same DWT cycle-counter facility used throughout `docs/performance_monitoring.md`).
- **SEMC SDRAM init** — replaces STM32 FMC config. Targets the same 64 MB the SOM already has, so `memory.h`'s allocator sizing is unchanged; only the controller init and base address differ.
- **USDHC + FatFs diskio port** — replaces `SdmmcHandler`-backed `sd_sdio.cpp`. `fs_browse.cpp`'s FatFs-level API calls should be untouched.
- **LPUART + eDMA transport** — replaces `uart4_dma_transport.cpp`'s STM32-specific DMA1/DMA2 stream wiring, same asynchronous-TX / continuous-RX contract so `daisy_uart_link.cpp`'s message dispatch layer above it doesn't need to change.
- **LPI2C driver** — replaces the I2C1-based MCP4728 CV DAC path (`cv_bus.hpp`/`Mcp4728Backend`).
- **Non-cacheable DMA region / cache maintenance** — RT1170 equivalent of libDaisy's `DMA_BUFFER_MEM_SECTION`. Same rule as §7.1 today (32-byte alignment, non-cacheable placement or explicit clean/invalidate), enforced via MPU region config instead of STM32's D2-SRAM-is-noncacheable trick.

### 7.2 Code expected to port with minimal change

Because the codebase already separates HAL-touching code from DSP/logic (a discipline `AGENTS.md` already enforces), most of the *interesting* code shouldn't need a rewrite, only a recompile against new peripheral handles:

- `firmware/shared/*` — protocol, framing, sequence tracking, WXCF container. Zero expected changes; host tests are the proof.
- `audio/voice_manager.hpp`, `audio/svf_filter.hpp`, `audio/envelope.hpp`, `audio/instrument.hpp` — q15 DSP logic with no direct libDaisy dependency beyond types passed in.
- `sequencer/*` (`pattern.hpp`, `sequencer_scheduler.hpp`, `tempo_follower.hpp`, `sequencer_transport.hpp`) — HAL-free by design already.
- `cv/cv_group_router.hpp`, `cv/cv_cal.hpp` — logic layer above the backend driver; only `Mcp4728Backend`'s I2C calls need porting (§7.1).
- `fs_browse.cpp` — pure FatFs API usage.
- `main.cpp`'s cooperative main-loop *structure* (message dispatch, `PumpWavIO`, metrics) — the loop logic stays; only the init calls at the top change.

### 7.3 Config/pin files (churn expected, structure unchanged)

`pin_config.h` and `hardware_config.h` remain the single source of truth per `AGENTS.md` — that convention doesn't change — but their **contents** for the Daisy side are wholesale STM32 pin/peripheral macros that become RT1170 IOMUXC pin names and peripheral instance numbers. This is mechanical but large; do it once real PHYTEC pinout data is in hand, not speculatively.

## 8. Open risks / unknowns (verify before or during Stage 1)

| Risk | Why it matters | How to resolve |
|---|---|---|
| SAI channel/TDM limits *(unconfirmed)* | Community source suggests only SAI1 does full 8-ch TDM; Stage B's PCM1690 TDM-8 plan assumed a capable SAI block | Confirm against the RT1170 reference manual before assuming Stage B ports unchanged |
| Carrier-board audio codec unidentified | Can't design the I2C init sequence or confirm SAI wiring without knowing the part | Pull PHYTEC phyBOARD-RT1170 schematics/hardware manual |
| RT1170 errata (rev 1.3+) unreviewed | Could affect SAI/eDMA/USDHC specifically | Pull current errata sheet from NXP (requires an NXP account) before committing to a peripheral usage pattern |
| No hobbyist/open-source precedent for audio-synth work on this chip | Higher bring-up risk than the well-trodden STM32/Daisy path — expect to be first through several problems | Budget bring-up time generously; the J-Link/PyOCD debug path (§6) matters more here than it would on a mature platform |
| FlexRAM ITCM/DTCM/OCRAM split is a software decision, not fixed | Wrong allocation could starve either hot-code (ITCM) or stack (DTCM) budgets the way `wavex_memory_sections.ld` currently link-caps DTCM at 64 KB deliberately | Size it from actual measured hot-path code size and stack usage, not by guessing; re-run the same kind of DWT-driven analysis `docs/backlog.md`/roadmap's DTCM item already calls for on the Daisy side |
| ARMGCC CMake toolchain file portability *(unconfirmed for the exact SDK version we'd vendor)* | Could bake in machine-specific paths like `firmware/daisy`'s toolchain file originally didn't (it was already hardened) | Test a from-scratch devcontainer build immediately after vendoring, don't assume it's clean |
| Flashing/debug tooling undecided (§6) | Blocks all hardware bring-up testing | Decide before ordering hardware / starting Stage 1 |

## 9. Naming decision (deferred)

Keep the `firmware/daisy/` directory name during the transition (misleading but avoids a disruptive rename touching `Makefile` targets, the devcontainer, and every doc that says "Daisy"/"STM32H750") **or** rename now to `firmware/rt1170/` for clarity. Recommendation: **keep both trees buildable side by side** during bring-up (mirrors the existing Stage A/B pattern for CV backends) under a new `firmware/rt1170/` directory, and only rename/retire `firmware/daisy/` once RT1170 hardware is validated end-to-end — don't do a big-bang rename before the new platform is proven. `architecture.md`, `roadmap.md`, and `AGENTS.md`'s many "Daisy Seed / STM32H750" references get swept in one dedicated documentation pass at that point, not incrementally.

## 10. Proposed implementation stages

Matches this project's usual one-commit-per-verified-stage workflow. Each stage should be buildable/testable in isolation before the next starts; nothing here jumps ahead without a hardware gate where one is listed.

1. **Toolchain skeleton.** Vendor MCUXpresso SDK (scoped includes), stand up `firmware/rt1170/CMakeLists.txt` + Makefile + toolchain file, get a "hello world" (blink + UART banner) compiling and linking. Gate: compiles clean in the devcontainer; no hardware needed yet if a QEMU/simulator option exists, otherwise defer the gate to Stage 2's hardware bring-up.
2. **Board bring-up.** Clock tree, pin mux, minimal GPIO/UART. Gate: LED blinks and a boot banner prints on real hardware via the chosen debug/flash tool (§6).
3. **Audio timebase proof.** SAI + eDMA silence-passthrough callback (or passthrough of the codec's ADC to its DAC) proving the 1 kHz / 48-sample tick fires at the right rate with no drift. Gate: DWT-measured callback period matches spec; scope-verified if possible.
4. **SDRAM + sample-RAM allocator port.** SEMC init, `memory.h` retargeted. Gate: existing host-tested allocator logic unchanged; on-hardware memtest passes.
5. **SD + FatFs port.** USDHC diskio layer. Gate: `fs_browse`/`sd_sdio` host tests green; hardware WAV read streams correctly.
6. **Inter-MCU UART link port.** LPUART+eDMA transport under the unchanged `daisy_uart_link.cpp` dispatch layer. Gate: round-trip protocol tests pass against real ESP32-P4 hardware; heartbeat/meters visible on the UI.
7. **Audio engine + CV port.** Wire `voice_manager`, `audio_engine`, `cv_group_router`/`Mcp4728Backend` onto the new peripheral bindings. Gate: existing host tests pass unchanged; 8-voice playback audible; CV output verified with a scope/multimeter as today's roadmap already requires for Stage A.
8. **Flashing/debug workflow productionized.** Makefile targets, devcontainer tooling, serial port resolver extension. Gate: a `make rt1170-flash`-equivalent one-command flash works end to end.
9. **Decision point**: retire `firmware/daisy/` (and the doc sweep from §9) once the above is soak-tested, or keep it as a fallback for a deprecation window — user's call at that time, not decided here.

---

This document is a planning artifact, not an approved roadmap phase. Next step is iterating on Stage 1 (or earlier stages of §10) with the user, and — once scope stabilizes — deciding whether this becomes a new `roadmap.md` phase or stays a parallel track.
