# RT1170 Migration Plan — Daisy Seed → PHYTEC phyCORE-i.MX RT1170

**Status**: Activated planning checkpoint, linked from Phase 2's callback-capacity section. No implementation, board port, or purchase is authorized by this planning update.
**Activation gate**: The 2026-09-07 DaisySP comparison measured 89.6635% worst callback utilization with callback-resident work still outstanding, so the recurring gate is **UPGRADE**. The WaveX 24 dB path measured 65.8029% and remains the fallback. The measured workload and limits are recorded in `docs/callback-performance-log.md`.
**Scope decision (recorded)**: **Like-for-like.** The phyCORE-RT1170 replaces the Daisy Seed's role only — real-time audio engine, SD sample streaming, SDRAM sample RAM, CV/Gate output. The ESP32-P4 keeps the UI, display, touch, and MIDI I/O exactly as today. The inter-MCU UART link is ported, not redesigned.
**Core split direction (2026-09-17)**: The user wants the M4 to own the ESP32 link and SD-card I/O where feasible, leaving audio processing on the M7. This supersedes the earlier M7-only end-state assumption; retain a simple M7-only bring-up baseline, then implement the bounded I/O service proposed in §11. No dual-core implementation or performance result exists yet. The board/module and user-reported external memory capacities still need reconciliation with the older PHYTEC assumptions below before a board port.
**Software stack decision (recorded)**: **Bare-metal, no RTOS**, built directly against NXP's MCUXpresso SDK drivers (not PHYTEC's Zephyr BSP). This preserves `architecture.md` §4.2's documented rationale — the audio timebase is the DMA clock, not a scheduler — which an RTOS would reintroduce jitter risk against.
**Research basis**: hardware/ecosystem findings below come from a web-research pass (2026-08-30) against NXP and PHYTEC primary docs plus community sources; items marked *(unconfirmed)* should be re-checked against the RT1170 reference manual and PHYTEC schematics before implementation, not treated as settled.

---

## 1. Why this is a bigger change than a part swap

There is **no libDaisy equivalent** for the RT1170 — no focused, audio-callback-first hardware abstraction library. PHYTEC's only published, versioned BSP for this SOM is Zephyr, which this plan deliberately does not adopt. That means the board-support layer libDaisy currently gives us for free (clock tree init, pin mux, `AudioHandle` double-buffered SAI callback, `SdmmcHandler`, DMA buffer placement) has to be **written from scratch** against NXP's MCUXpresso SDK peripheral drivers, using PHYTEC's schematics/pinout for the module and carrier board as the wiring reference. Budget this as the majority of the migration effort — porting `audio_engine.cpp`'s DSP logic is comparatively small.

## 2. Hardware comparison (as-built Daisy Seed vs. phyCORE-RT1170)

| Property | Daisy Seed (STM32H750) | phyCORE-i.MX RT1170 (RT1176) | Notes |
|---|---|---|---|
| Core | Cortex-M7 @ 480 MHz | Cortex-M7 @ up to 1 GHz + Cortex-M4 @ 400 MHz | M7 audio with proposed M4 link/storage service (§11), following an M7-only bring-up baseline |
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
- Using the RT1176's M4 core during the initial audio timebase proof. The requested subsequent I/O split is covered by §11.
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

The H750 and RT1170 M7 share the relevant Cortex-M7/FPv5 instruction target,
including hardware single- and double-precision floating point. The existing
`-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16` flags apply to both
M7 builds ([NXP RT1170 datasheet](https://www.nxp.com/docs/en/data-sheet/IMXRT1170IEC.pdf),
[ST H750 specification](https://www.st.com/en/microcontrollers-microprocessors/stm32h750-value-line.html)).
This supports DSP source reuse; it does not make startup, peripheral drivers,
linker placement or binaries interchangeable. Measure the real memory/interrupt
workload rather than assuming callback time scales exactly with clock speed.

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
| FlexRAM ITCM/DTCM/OCRAM split is a software decision, not fixed | Wrong allocation could starve either hot-code (ITCM) or stack (DTCM) budgets the way `wavex_memory_sections.ld` currently link-caps DTCM at 64 KB deliberately | Size it from actual measured hot-path code size and stack usage, not by guessing; re-run the same kind of DWT-driven analysis `docs/roadmap.md`/roadmap's DTCM item already calls for on the Daisy side |
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

This document is a planning artifact linked from the Phase 2 callback-capacity
checkpoint. The user's requested M4 I/O direction is recorded below; board
bring-up and a dual-core firmware implementation have not been performed.

## 11. Proposed M4 link and storage service

**Requested 2026-09-17; design direction, not implemented.** Keep the M7's
48 kHz audio callback, sequencer clock, modulation, voice state and Sample Pool
authority together. Move link framing, storage work and their peripheral
interrupts to the M4. This removes CPU/interrupt work from the M7; it does not
give either core exclusive bandwidth to shared external RAM.

### Access and resource ownership

The M4 can use shared OCRAM, system-mapped external memory and permitted system
peripherals. Core-local addresses/aliases are not a portable shared-pointer
contract. Both images must agree on the shared region and buffer-offset ABI;
verify each peripheral, DMA master and memory region against the selected
device's access map. Keep each core's TCM private by design.

| Resource | Owner and boundary |
|---|---|
| Audio SAI, audio DMA, voices, sequencer and control timebase | M7; M4 forwards commands but never mutates live engine state |
| ESP32 UART, or subsequently validated SPI transport, and assigned DMA channels | M4; existing external framing/payload definitions remain authoritative |
| USDHC, SD card, FatFs volume and open file handles | M4 only; M7 submits bounded asynchronous requests |
| Sample Pool allocation and published sample identity | M7; grants M4 bounded writable leases on unowned buffers/assets |
| Shared command/completion queues | One producer and one consumer per direction; fixed capacity and explicit overflow policy |
| Shared clocks, pin mux and memory-controller initialization | One boot owner; no independent runtime reconfiguration by both cores |

NXP's [M4 USB/SD example port](https://mcuxpresso.nxp.com/mcuxsdk/latest/html/middleware/usb/docs/AN13690/topics/mcuxpresso_ide.html)
explicitly moves the USDHC and SDMMC drivers to an RT1170 M4 project, providing
evidence that the storage assignment is feasible. This does not verify our
board wiring, controller instance, DMA destinations or sustained throughput.

### Contention, notification and cache visibility

Hardware bus arbitration serializes competing memory transactions. Resource
domain controllers enforce access permissions; they are not a bandwidth
reservation or a guarantee that M7 requests always win. Interrupt priority on
one core does not prioritize that core's external-memory traffic. Use bounded
bulk transfers, prefetch, admission control and hot TCM state to limit exposure;
verify any QoS/arbitration tuning against the reference manual before using it.

M7 may play immutable samples in SDRAM while M4 fills a different SDRAM region.
No whole-SDRAM mutex is needed: the controller interleaves transactions. Distinct
regions prevent logical overwrite, but do not provide separate bandwidth or
eliminate row/bus contention. Keep loading extents and published assets disjoint
and aligned to cache lines. Complete cache visibility for just the new extent
before publishing it; unrelated live samples do not need global invalidation.
Reusing a formerly audible extent still requires released voice references and
the complete cache/DMA handoff, even when its numeric address is unchanged.

Use the Messaging Unit (MU) for notifications and shared memory for payloads.
Evaluate NXP's bare-metal [RPMsg-Lite example](https://docs.mcuxpresso.nxp.com/mcuxsdk/latest/html/examples/multicore_examples/rpmsg_lite_pingpong/readme.html)
before inventing an inter-core transport. The MU ISR should acknowledge/latch
work, not perform bulk cache maintenance or application mutation. Software
queues remain the authority when notifications coalesce. SEMA4 may protect
rare setup operations; never spin or wait on it in the audio callback.

There is no automatic coherent shared-data contract between the M7 cache, M4
LMEM cache and DMA. Reserve a small shared non-cacheable OCRAM region using each
core's appropriate cache/memory configuration for queue metadata and initial
I/O staging. Use properly ordered publication and cache-line-isolated records;
`volatile` alone is insufficient. Verify the distinct M7 and M4 cache APIs.

Keep large sample data cacheable only with a defined handoff: the producer
completes CPU/DMA writes and required cache operations before publishing; the
consumer invalidates stale data before accepting ownership. Prepare destinations
before DMA as required so dirty lines cannot later overwrite incoming data.
Perform bulk maintenance in M7 foreground work before a buffer becomes audible,
not in the callback. Do not concurrently access an active DMA destination.
[NXP's cache guidance](https://www.nxp.com/docs/en/application-note/AN12042.pdf)
explains DMA visibility and the non-cacheable-buffer alternative; exact M4
LMEM configuration remains a port-specific verification item.

### Buffer and request lifecycle

Represent storage work with request ID, service epoch, asset/stream generation,
file offset, buffer ID/offset, capacity and requested length. Completion carries
actual length and status. M4 file handles and C++ object pointers stay local.

```text
Free → M4 filling / DMA active → completed → M7 prepared and reading → Free
```

M4 cannot recycle a buffer until M7 releases it. Loaded assets publish only
after complete preparation; cancellation or either-core restart rejects stale
generations and prevents reuse until DMA is stopped and ownership reconciled.
Sample unload must wait for all voice references to be released.

For streaming, fill a multi-block read-ahead pool before playback and replenish
ahead of a low-water mark. The callback consumes ready data only, never waits
for an RPC/SD completion. Define bounded fade/silence behavior and underrun
telemetry when storage fails. Resident samples may continue during an I/O-core
failure. SD-card latency and shared-bus traffic remain even with M4 ownership.

M4 scheduling must preserve prompt note/control forwarding while SD is busy.
A long blocking FatFs/SD call can otherwise move the responsiveness problem
onto the M4. Use bounded driver waits/state machines or a separately bounded
forwarding path, prioritize stream refills over bulk browsing/preview work,
and coalesce low-priority meters. DMA alone does not establish this guarantee.

### Audition workload and comparison with Daisy

The 400 MHz M4 is not a compute-equivalent replacement for the 480 MHz M7.
Matching I/O service is nevertheless plausible because peripheral clocks/DMA
and card latency determine much of transfer performance, and the M4 no longer
shares its execution time with the audio renderer. No RT1176 throughput or
latency result has been measured for WaveX.

The current `PumpWavIO()` in
[`audio_engine.cpp`](../firmware/daisy/src/audio/audio_engine.cpp) performs
prebuffering, SD-slot refill, PCM format conversion, gain/fades and optional
streaming linear sample-rate conversion before publishing to the audio ring.
The callback consumes prepared samples. Preserve this producer/consumer
boundary; do not move filesystem operations into an M7 callback or make an
inter-core request for each output block.

For one browser audition, evaluate an M4-owned bounded producer that converts
the supported WAV formats to the fixed 48 kHz interleaved PCM16 preview stream.
The M7 consumes ready blocks and owns final mixing, live preview level,
start/stop fades and the audio timebase. Keep Instrument playback pitch and
voice processing on M7. Moving the existing simple audition SRC to the M4 is a
candidate to benchmark, not evidence that high-quality SRC, timestretch or
arbitrary codecs fit that service budget.

| Example workload | Sustained payload, excluding framing/filesystem overhead |
|---|---:|
| Stereo PCM16 at 48 kHz from SD | 192,000 bytes/s |
| Stereo PCM24 at 48 kHz from SD | 288,000 bytes/s |
| Prepared stereo PCM16 at 48 kHz between cores | 192,000 bytes/s |
| Current 2 Mbaud UART, assuming 8N1 | At most 200,000 bytes/s per direction before protocol overhead |

Sample audio stays in shared RAM; it does not pass over the ESP32 link. Notify
per published chunk or queue transition, not per sample. The table describes
one preview at normal speed, not admission for multiple streamed voices or a
future faster SPI link.

At 192,000 bytes/s, a fully occupied 64 KiB PCM ring represents about 341 ms
of audio and 128 KiB about 683 ms. These are candidate capacities, not chosen
defaults or an SD-stall guarantee. Available protection is current occupancy
divided by consumption rate. Set the refill threshold so remaining audio
exceeds the measured service latency plus scheduling margin; enough average
throughput alone does not avoid underruns. Separate startup prefill from full
ring capacity so browsing need not wait for the entire ring to fill.

Rapid preview changes need a stream generation on every command and block.
M7 can fade/stop the old generation immediately and reject late blocks while
M4 finishes/cancels its read safely. Do not drain hundreds of milliseconds of
old queued audio before honoring Stop. The next preview becomes audible only
after its own prefill and visibility handoff. Explicitly acknowledge applied
Start/Stop on M7 rather than treating a completed SD read as audible playback.

MCUXpresso provides an asynchronous
[`USDHC_TransferNonBlocking` API](https://mcuxpresso.nxp.com/api_doc/dev/4784/a00094.html)
with ADMA configuration. This is a driver building block: FatFs and higher
SDMMC calls can still wait synchronously. Audit that entire wait chain so SD
busy time cannot block urgent link forwarding. Refill, urgent controls and
bulk browse/preview analysis need separate service priorities; a waveform scan
must not consume the preview's read-ahead margin.

Compare the actual migration against Daisy using time to first sound, applied
Stop latency, MIDI/control latency during forced SD delays, lowest ring fill,
SD read latency distribution and M7 maximum callback cycles. Include rapid
file switching, sample-rate conversion, SD writes and active resident voices.
The existing storage policy stops streamed audition for some save operations;
preserve that policy initially. Supporting simultaneous save and audition is a
separate measured expansion, not an automatic benefit of dual-core execution.

### Incremental validation

After the M7-only audio baseline, prove shared-memory visibility and epoch
recovery; move the link first, then grant the M4 exclusive SD ownership.
Compare M7 worst-case callback cycles, command latency, stream low-water marks
and underruns against the baseline while loading/saving/browsing concurrently.
Exercise SD stalls/removal, queue exhaustion and either-core reset. Add the
specific bench procedures to `hardware-validation.md` when implementation
reaches these gates; no gate is claimed passed by this proposal.

The [NXP RT1170 data sheet](https://www.nxp.com/docs/en/data-sheet/IMXRT1170BCEC.pdf)
describes MU, SEMA4 and resource-domain protection. Exact memory aliases,
peripheral grants, DMA routing and arbitration registers must be verified
against the board and reference manual during port design.
