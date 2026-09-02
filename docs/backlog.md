# Backlog

Work that is worth doing but is not scheduled into a roadmap phase. Items here
are deliberately *not* in [`roadmap.md`](roadmap.md): that file tracks the
phased build-out of the instrument, and mixing opportunistic engineering work
into it makes the phase gates harder to read.

Each entry should record **why it is not urgent**, so a future reader can tell
whether the reasoning still holds. An item whose justification has expired is
more useful than an item with no justification at all.

---

## A host pixel test for LVGL widgets is possible today

**Recorded 2026-09-01, correcting a claim made the same day.** The stacked
L/R waveform work was justified as having no host coverage because "there is
no LVGL on the host". **That is wrong**, and the mistaken version briefly went
into `roadmap.md` § Outstanding hardware verification. The real situation is
narrower and worth stating, because the wrong version argues for reaching for
hardware when a host test would do.

**Real LVGL already builds and renders on the host.** `tools/ui_preview`
compiles the *same* vendored LVGL the firmware uses
(`firmware/esp32/managed_components/lvgl__lvgl`), creates a real
`lv_display_create(1280, 720)`, calls `lv_refr_now()`, and writes the resulting
framebuffer to BMP (`preview.c:80`, `:376`). No hardware, no SDL, no display
driver — LVGL's software renderer into plain memory.

**There are two host LVGL setups and widgets are in neither:**

| Setup | LVGL | Why a widget is not covered |
|---|---|---|
| `firmware/esp32/tests` | 96-line **stub** `mocks/lvgl.h`, written for `file_browser.cpp` | No `lv_area_t`, no draw API, so anything calling `lv_draw_*` will not compile |
| `tools/ui_preview` | **Real** vendored LVGL, renders to BMP | C-only CMake project (`project(... C)`); it *reimplements* a waveform in C at `preview.c:750` instead of linking the C++ widget. Not wired into `make` or CI. |

**Built, same day.** `firmware/esp32/tests/widget/` renders widgets with the
real vendored LVGL and asserts on pixels; `waveform_view_test.cpp` covers the
stacked L/R traces, including the channel-swap case that was the only real
correctness risk in that change. Adds ~12 s to `make test-esp32`. The how, the
`mocks/` shadowing trap, and the pixels-not-rows lesson are written up in
`docs/testing_guide.md` § *Testing LVGL widgets by pixel*.

**What remains open** is coverage, not capability:

- Only `WaveformView` is wired in. Other leaf widgets (`ui_softkey_bar`,
  `ui_tab_group`, `ui_busy_overlay`) could follow the same pattern for the
  cost of adding their `.cpp` to the target.
- **The pages still cannot be reached this way**, and that is the
  `components/ui` ⇄ `main` cycle below, not an LVGL problem. `WaveformView`
  was the cheap place to start precisely because it has no `main` dependency.

**Why the remainder is not urgent:** the widgets left are simpler than the one
now covered, and none has a defect on record. The item that would genuinely
pay is breaking the `main` cycle so pages become testable, which is already
recorded separately and is a much larger job.

---

## Page-entry render cost: the waveform chart and the Play keyboard

**Measured 2026-08-30.** The pages reported as "slow to render" - Sample Edit,
Sample Record and Play/Keys - are not slow to *run*. Steady-state render is
0-5 ms on every page in the product. The entire cost is the **first frame after
entering the page**, and it is large:

| Page | First-frame render | Steady state |
|---|---|---|
| Sample -> Edit | 35-**205** ms (typically 66-102) | 0-5 ms |
| Sample -> Record | 50-77 ms | 0-5 ms |
| Play | 40-45 ms | 0-2 ms |
| Diagnostics | 30-47 ms | 0-6 ms |
| Sample -> Browse | 11-28 ms | 1-3 ms |
| Main Menu | 11-18 ms | 0-5 ms |

Method: `scripts/sysmon_stats.py` over a serial capture, reading the samples
immediately after each `UI_NAVIGATOR: Entering page` marker rather than the
steady-state medians. **This is why it went unnoticed for so long** - every
aggregate view of this data, including the tool's own default, drops warmup
samples precisely because a page-entry redraw is "not the steady state". The
thing being discarded as noise *was* the complaint.

Confirmed not the cause: invalidation scope. With `CONFIG_LV_USE_REFR_DEBUG=y`
only the touched key redraws on the Play page, so redraw regions are already
tight. Also not the cause: the PPA draw unit - the same first-frame costs
appear with it on and off.

**Prime suspect for the two worst pages is `WaveformView`.** It is used by
exactly Sample Edit and Sample Record (`ui_sample_edit_page.cpp:206`,
`ui_sample_record_page.cpp:47`) and nowhere else, which matches the ranking
precisely. It builds an `lv_chart` of `LV_CHART_TYPE_LINE` with
`kPointCount = 512` (`waveform_view.h:48`) and **two** series - the upper and
lower halves of the envelope silhouette - so LVGL software-renders roughly 1022
anti-aliased line segments on the first draw. Sample Edit is worse than Record
because it draws that chart plus the rest of the edit chrome.

Play has no chart; its ~40 ms is a different shape of the same problem, roughly
82 objects (41 keys, each a button plus a label) laid out and drawn in one go.

**Ideas, cheapest first, none yet measured:**

1. Drop `kPointCount`. 512 columns across a panel narrower than 512 px buys
   nothing that survives rasterisation.
2. Stop drawing the envelope as a two-series AA polyline. A waveform silhouette
   is a filled shape; vertical spans on a canvas would replace ~1022 line
   segments with a fill and would not need anti-aliasing at all.
3. Build the page incrementally - show chrome first, populate the waveform from
   an `lv_timer` - so the cost is spread rather than concentrated in the frame
   the user is waiting on. This hides it rather than fixing it.

**Why it is not urgent:** it is a one-off cost on page entry, not a sustained
frame-rate problem, and the UI is otherwise comfortably inside budget at
0-5 ms steady state. It is worth doing because ~200 ms is perceptible as lag
when opening a page, not because anything is at risk.

---

## Build speed and size: LTO is unavailable, and the obvious LVGL trim does not work

**Investigated 2026-08-30.** The ask was to use LTO and to exclude cruft so the
ESP32 firmware builds faster and smaller. Both of the expected levers turned out
to be closed, and the reasons are worth recording so nobody re-derives them.

**LTO is not available on ESP-IDF.** `grep -rn flto` across the entire IDF tree
(`--include='*.cmake' --include='Kconfig*' --include='*.txt'`) returns nothing:
there is no `COMPILER_OPTIMIZATION_LTO`, no IPO option, no plumbing. This is an
upstream position, not a missing setting on our side — IDF's linker fragments,
IRAM/section attributes and `--gc-sections` placement do not survive
whole-program optimization. There is nothing to enable. We build
`CONFIG_COMPILER_OPTIMIZATION_PERF` (`-O2`), which is the right choice already.

**Trimming LVGL's Kconfig cannot speed up the build.** This is the
counterintuitive one. `env_support/cmake/esp.cmake:3` does
`file(GLOB_RECURSE SOURCES ${LVGL_ROOT_DIR}/src/*.c ...)` — *every* LVGL source
file is compiled unconditionally, and the Kconfig flags only gate content with
`#if` blocks inside those files. Turning a widget off yields an empty object
file that still cost a full compile. LVGL is **781 of the 1760 objects** in a
full build (44%), so it dominates build time, and none of that time is
reachable through configuration. Only the demo and example globs are genuinely
conditional, and we already build none of them.

**ccache is already on**, so the cheap win is taken: the devcontainer has
ccache 4.9.1 with `IDF_CCACHE_ENABLE=1` in the environment, currently running a
40.8% hit rate over 7174 cacheable calls. Incremental rebuilds are already
served by it; the misses are the genuinely-changed translation units.

**Why this is not urgent:** what remains would be real work for a modest
return. Vendoring LVGL to replace the glob with an explicit source list would
cut build time meaningfully, but it means maintaining a fork of a managed
component — the `managed_components/` tree is regenerated by the component
manager, so the edit would be silently reverted on any dependency change, and
`docs/roadmap.md` § 0.1 already warns about exactly that failure mode with
submodule overrides. Not worth it while a full build is minutes and ccache
covers the inner loop.

**Where the size actually is,** if size ever becomes the constraint (today the
app is 957 KB in a 4 MB partition, 77% free, so it is not): LVGL is 584 KB of
the 950 KB image — 199 KB `.rodata` (largely the five compiled-in Montserrat
faces, `CONFIG_LV_FONT_MONTSERRAT_18/22/24/32/36`), 254 KB `.text`, and 128 KB
of DIRAM `.bss` that is the `LV_MEM_SIZE` pool. Dropping an unused font size is
the single largest one-line saving available. `libesp_app_format.a` also
carries 64 KB of `.rodata` that nobody has explained.

---

## LTO on the Daisy image

**Investigated 2026-08-30 only far enough to say it is not blocked.** Unlike
ESP-IDF — where LTO does not exist at all, see the entry above — the Daisy is
built with `arm-none-eabi-gcc`, which supports `-flto` normally. So the
question is live here in a way it is not on the ESP32.

**It is second in line, not first.** The Daisy firmware compiles at `-O0`
(`firmware/daisy/CMakeLists.txt:30`, `WAVEX_DAISY_OPT`), and LTO at `-O0` buys
essentially nothing: the inter-procedural passes it enables have no
optimization pipeline to feed. Raising the optimization level is both the
larger win and a prerequisite; it is recorded separately as [its own
entry](#the-daisy-image-is-compiled--o0-the--o2-decision-is-unmade), which
explains why `-O0` is the default (it is what the firmware has always been
built with, unintentionally). Measure `-O2` with DWT first. Only then does `-flto` become an interesting
follow-on question.

**Two hazards specific to this firmware,** worth knowing before anyone spends a
day on it. Both are the same class of problem that keeps LTO out of ESP-IDF:

- **Section placements.** This codebase deliberately places state in DTCM
  (`s_voice_manager`, `s_para_env`, per-block DSP/stat state — see roadmap
  § Outstanding hardware verification) and has ITCM placement queued in this
  backlog. LTO is free to merge, clone and move symbols, which is exactly what
  `__attribute__((section(...)))` placement assumes will not happen.
- **The HAL's weak symbols.** ST's HAL relies on weak definitions overridden by
  strong ones, and interrupt vectors are referenced only from the vector table.
  LTO's whole-program view is where those get miscompiled or discarded, and the
  failure mode is a hard fault at run time rather than a link error.

**Why it is not urgent:** the Daisy is not currently short of flash, and the
real-time budget question in front of us is `-O0` → `-O2`, which is a bigger,
simpler and far safer lever. Revisit only after that has been measured and
banked.

---

## Self-implemented bidirectional SPI link (replacing UART4)

**Want:** a faster, DMA-driven, non-blocking link between the MCUs, ideally
with the ESP32 as master since most control actions originate there.

See [`spi-notes.md`](spi-notes.md) for the libDaisy 8.1 slave/DMA constraints,
the minimal fixed-frame bring-up gate, the dormant link diagnosis, and the
planned Stage B CV SPI interaction.

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

## Streaming CRC recovery can outrun the audio ring

**Found in the 2026-08-31 Daisy real-time review.** After three streaming read
errors, `audio_engine.cpp` calls `SdSdio::DowngradeSpeed()` for an SDMMC data
CRC failure. That enters `TrySpeed()`, which unmounts, deinitializes,
reinitializes, remounts, and may perform five 50 ms `FR_NOT_READY` retries.
This all runs synchronously in the main loop. One attempt can therefore take
roughly 250 ms, while the 2,048-frame audio ring holds only about 42.7 ms at
48 kHz. The audio callback remains non-blocking, but the ring can empty and
the same stall also delays UART and CV servicing.

**Why it is not being changed blind:** the failure path was added in response
to a real marginal-card CRC problem, and removing it would restore permanent
playback failure. Choosing between an audible pause with automatic recovery,
an immediate abort, or a larger prebuffer is product behavior; proving a
replacement also needs the problem card or an equivalent fault-injection
bench setup. Host tests cannot model HAL/FatFS timing or SDMMC electrical
failures faithfully.

**Fix if picked up:** make recovery an explicit main-loop state machine. Stop
or mark the stream as recovering, close the stale `FIL`, perform at most one
bounded negotiation step per loop pass, refill to a defined high-water mark,
then resume or report a terminal error. On hardware, inject/reproduce CRC
failures and capture maximum main-loop latency, ring low-water, UART/CV
service gaps, and the user-visible result. Acceptance requires no stale audio,
no silent permanent failure, and a documented choice about whether playback
may pause and resume.

---

## GT911 touch range mismatch

The vendored BSP's `bsp_touch_new()` (`esp32_p4_nano.c` in
`managed_components/waveshare__esp32_p4_nano/`) configures the GT911 with
`x_max = 720, y_max = 1280` for our panel
(`CONFIG_BSP_LCD_TYPE_720_1280_5_INCH_A` falls through to the native-orientation
branch), but LVGL draws to the software-rotated 1280×720 landscape canvas
(`LV_DISPLAY_ROTATION_90`, `display_manager.cpp`). Touch is configured in the
panel's native portrait orientation while the display it's reporting against
is rotated to landscape.

**Why it is not urgent:** touch has been working well enough that nobody
noticed — the GT911 reports its own coordinates and the softkey targets are
large. But if touch ever feels offset, swapped, or dead near two edges, this
is the first suspect (verify by tapping all four corners). Do not fix blind:
change it with the panel attached and corner-tap before/after, since the
correct values depend on how the driver interacts with the panel's own
configuration, and the fix likely lives in how `esp_lvgl_port`'s indev is set
up to consume BSP touch coordinates, not in this vendored file directly.

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

## SPI-link revival is gated on six recorded defects

**Want:** when the SPI link is re-enabled (`WAVEX_SPI_LINK_ENABLED`,
`link_config.h` — decision of 2026-07-05 made UART the transport of record),
the quarantined `esp_spi_link.cpp` must first be fixed.

**Current state:** the 2026-08-29 ESP32 review recorded five blockers as
SPI-1..SPI-5 (that review has since been deleted; the blockers are restated
here in full): driver-owned
transaction descriptors/RX buffers reused on result timeout, no sequence
gating on the live SPI RX path, an uninitialized in/out capacity that can
overflow a 220-byte stack buffer, an 8-bit TX sequence wrapping through the
reserved value 0, and configured-vs-actual transfer length confusion. A later
2026-08-30 ESP32 coding-guide pass (`roadmap.md` §0.2 item 3) found a sixth,
**SPI-6**: `spi_post_trans_cb` is registered as the SPI slave driver's ISR
callback and calls `gpio_set_level()` and `AttnWatchdog::MarkCleared()`,
neither audited for IRAM-safety (guide §4) — the driver instance doesn't
request `ESP_INTR_FLAG_IRAM` today, so it likely works by accident rather than
by audit.

**Why it is not urgent:** the code is compiled out of every image today, so
none of it is reachable. It becomes urgent the moment anyone flips the flag —
which is why it is recorded here rather than fixed opportunistically: fixing
dead code cannot be verified on hardware, and the project's standard is not to
claim fixes without a way to observe them.

**When to revisit:** at SPI revival planning. Copy SPI-1..SPI-6 into that
roadmap item's gate before any bring-up work starts. `roadmap.md` §0.2 item 3
should point back at this list rather than restate a subset of it.

---

## Break the `components/ui` ⇄ `main` dependency cycle

**Want:** the UI component to stop depending on `main`, so it can be built and
host-tested on its own.

**Current state:** `components/ui/CMakeLists.txt` declares `REQUIRES ... main`,
and 18 include sites across ten UI files reach into `inter_mcu.h`, `ui_task.h`
and `comm/i_comm_interface.h`. Pages call `inter_mcu_*` free functions directly.
`docs/ui-architecture.md` already names the intended fix — a `UISharedContext`
injected into pages instead of global reach-through — and lists it as future
work; the cycle is stronger than that doc admits. Recorded as E-ARCH1 by the
2026-08-29 ESP32 review, and deferred there by explicit decision rather than
stacked on top of twenty unverified behavioural changes.

**Why it is not urgent:** nothing is broken by it. The cost is paid in
testability rather than behaviour: the UI component cannot be compiled without
the whole frontend, so pages have no host tests, which is why the LVGL-threading
and listener-lifetime defects fixed in August were found by reading rather than
by a failing test.

**Why it is explicitly deferred right now:** it touches every page, and the
August remediation pass changed input decoding, the LVGL tick, touch ownership,
refresh cadence and link timing without any of it running on hardware. Layering
a wide refactor on top of that would make a bench failure impossible to
attribute — the first question would be "is this the new architecture or one of
the twenty behavioural fixes?" Do the hardware pass first.

**If picked up, the shape that seems right:**

- Define `UISharedContext` with the handful of operations pages actually use
  (send note, request envelope, request browse, read meters) rather than
  exposing `inter_mcu.h` wholesale — the narrow interface is the point.
- Inject it at page construction via the existing `UINavigator` factory
  functions, so no page reaches for a global.
- Move `ICommInterface` into the UI component or a third shared component; it is
  currently in `main` only by accident of where it was written.
- Drop `main` from the UI component's `REQUIRES` last: that is the check that
  the job is finished, not a step along the way.

---

## Sample status can only describe 8 of up to 32 loaded samples

**What:** the Daisy holds up to `kLoadedSampleCapacity` (= `kMaxZones`, 32)
loaded samples, but `SampleMemStatusMessage` carries
`WAVEX_SAMPLE_STATUS_MAX_ENTRIES` (8) and `GetSampleMemStatus()` clamps to it.
The frontend's metadata cache is also 8 (`kMetaCacheSize`), and the Sample
Manager builds its list by probing that cache.

**Why it matters beyond a short list.** The status message is the only one that
carries a count plus the full resident set, so it is what the frontend uses to
work out that a sample has been *removed* — a metadata push can only describe
samples that exist, and unloading the last one pushes nothing at all. That makes
status the authority on deletion, and a truncated authority cannot prove absence.

`prune_sample_meta_to()` therefore refuses to prune when
`sample_count >= WAVEX_SAMPLE_STATUS_MAX_ENTRIES`, because the truncation keeps
the *first* 8 loaded while the cache holds the 8 most recently *pushed*, and
those sets need not overlap — pruning against a truncated list would drop live
entries. The consequence: **with 8 or more samples loaded, an unloaded sample
can linger in the frontend's list until the count drops below 8 again.**

**Why it is not urgent:** the cache is 8 entries, so the frontend cannot track
more than 8 regardless, and loading 8+ samples is not a normal workflow yet
(there is no kit or multisample UI to drive it). The guard makes the failure
conservative — a stale row — rather than the alternative, which is silently
dropping samples that are genuinely loaded.

**When to revisit:** when kits or multisampled instruments start loading more
than a handful of samples at once — Phase 2 item 4 or Phase 2.5 item 1. Fixing
it properly means either paging the status (a `first_index` field and repeated
responses) or a dedicated D→E "sample N unloaded" message, which expresses the
deletion directly instead of inferring it from a set difference. Prefer the
explicit message: it also covers eviction, which today notifies nobody at all.

---

## `ControlParameter`'s target id space contradicts what is already live

**What:** `docs/features/param-locks-and-modulation.md` §1 specifies the future
`ControlParameter` layout and assigns `PARAM_PAN = 0x0C` and
`PARAM_PITCH_OFFSET = 0x0B`. But `PARAM_PAN` is already live at `0x08` and
`PARAM_PITCH` at `0x09`: the Voice page sends both and the Daisy's parameter
switch handles both. The design doc's preamble — "0x01..0x0A existing (…
LFO_RATE, LFO_DEPTH, MODULATION_MATRIX)" — describes the enum as it was before
the Voice page landed, when `0x08`/`0x09` carried only the LFO labels.

**Found:** stage 8 of `ui-information-architecture.md`, while fixing a genuine
duplicate-value defect in the same enum (`PARAM_LFO_RATE` and `PARAM_PAN` both
`0x08`; `PARAM_LFO_DEPTH` and `PARAM_PITCH` both `0x09`). That fix moved the two
dead LFO ids to `0x16`/`0x17`, deliberately *clear* of the `0x0B`–`0x15` block
the design doc reserves, so the reserved block is still intact and this item is
purely about reconciling the doc with the code.

**Why it is not urgent:** nothing is broken. `PAN`/`PITCH` work at their live
values on both sides, the reserved block is unoccupied, and no p-lock or
mod-matrix code exists yet to be confused by the discrepancy. The cost of
getting it wrong is paid only when someone implements §1 and either renumbers
two live ids (a wire break for no benefit) or leaves `0x0C` documented as `PAN`
while `0x08` is the one on the wire.

**When to revisit:** when param locks or the modulation matrix are implemented
(Phase 2 / 2.5). The likely resolution is to leave `PAN`/`PITCH` where they are
and edit §1 to match, since a target design should not renumber ids that are
already shipping. Whoever does it should also decide what `0x0B`/`0x0C` become
once `PAN` no longer needs `0x0C`.

---

## Browse listings cap at 256 entries; the 500-entry target needs paging

**Surfaced by the 2026-08-30 test-quality audit.** (The audit's other
production findings — loop-window truncation, `ValidateWaveXPacket`
underflow, the play-request `strlen` overrun, `ListDir` error swallowing,
`SampleMemMgr::ptr()` on released handles, the file browser's `//name` root
paths — were all fixed the same day; see `CHANGELOG.md`.)

`fs_browse.cpp`'s internal static 256-entry scratch array bounds both
enumeration and `total_count`: a directory with more qualifying entries lists
exactly 256 and reports 256. This is deliberately self-consistent — reporting
the true count while only being able to serve 256 would hand the frontend
pages it can request but never receive. `ListDir` also stops scanning once
the array fills, because each further `f_readdir` is real SD I/O on the main
loop for entries that would only be discarded. Pinned in `fs_browse_test.cpp`
(`ListingIsCappedAt256Entries`).

**Why it is not urgent:** a directory needs more than 256 qualifying entries
(subdirs + WAVs) before it truncates, and the failure mode is a
truncated-but-usable listing, not corruption.

**When to revisit:** with the roadmap's 500-entry browse target. The fix is a
paging redesign, not a bigger array: enumerate per requested page (re-scan
with a start offset — FatFS has no seekdir) or cache the directory scan
outside the request path, and decide then whether `total_count` should count
beyond what one response can serve.

---

## Softkey press heap-allocates a `std::function` per event

**Found in the 2026-08-30 ESP32 coding-guide review.** Every softkey press
(`SoftkeyBar::pressFocused()` and `event_cb()`, `ui_softkey_bar.cpp`) does
`new std::function<void()>(cb)` to carry the callback across `lv_async_call`'s
deferral, freed inside the deferred call. `docs/esp32p4_coding_guide.md` §8
flags `std::function` heap churn as worth suspicion in long-running embedded
code.

**Why it is not urgent:** the deferral exists specifically because handling a
softkey press synchronously inside LVGL event processing is unsafe (it may
push/pop a page while LVGL is mid-draw), and the allocation rate is bounded by
human button-press cadence — a few Hz at most, nothing like a per-frame or
per-sample path. No fragmentation symptom has been observed or reported.

**Fix if picked up:** a small fixed-capacity ring of pending closures (sized
to the softkey count) would remove the allocation without changing the
deferral semantics. Not worth disturbing this code for its own sake; do it if
something else already touches `ui_softkey_bar.cpp`.

---

## The output sink object exists but nothing drives it

**Found by the 2026-08-30 firmware warning sweep** (`-Wall` flagged it as
unused). `audio_engine.cpp` constructs `s_output_sink` — the
`StereoMixSink`/`TdmVoiceSink` selected by `WAVEX_VOICE_OUTPUT_BACKEND` — but
no code path calls into it. Audio reaches the codec without going through the
sink, so the Stage A/B *output* backend flag today selects only which sink
type must keep compiling, not which one runs. (The CV backend flag, by
contrast, is live through `s_cv_router`.) The object is kept, marked
`__attribute__((unused))`, so both sink types stay compiled under CI's two
flag sets.

**Why it is not urgent:** Stage A audio output works without the sink, and
Stage B (TDM8 voice board) is not the active phase. The risk is only that
someone reads the flag as routing audio when it does not yet.

**Fix if picked up:** route the audio callback's output through
`s_output_sink` when Stage B wiring lands, and drop the unused attribute.

---

## `WAVEX_ESP_SPI2_*` pins collide with `WAVEX_ESP_PCNT1_*`

**Found in the 2026-08-30 comment audit of `firmware/shared`.**
`pin_config.h` assigns `WAVEX_ESP_SPI2_SCLK/MOSI` to GPIO 46/47, the same
pins as `WAVEX_ESP_PCNT1_A/B` — consistent with that header's own UNVERIFIED
warning that the ESP32 assignments were written for an S3 DevKit and never
re-checked against the P4 board.

**Why it is not urgent:** the SPI link is compiled out
(`WAVEX_SPI_LINK_ENABLED` is 0), so only the PCNT assignment is live; the
collision cannot bite until the SPI link is revived. Resolving it now would
mean guessing new pins without the hardware in hand, which is exactly what
the UNVERIFIED warning exists to prevent.

**Fix if picked up:** assign non-conflicting SPI2 pins as part of the pin
re-verification pass `pin_config.h` already calls for, before re-enabling
`WAVEX_SPI_LINK_ENABLED`.

---

## Logging: fold `UART_LOGx` into the module table; level the legacy Daisy call sites

**Deferred from the 2026-08-31 logging rework** (see `docs/logging.md`).
Two remainders:

- The 88 `UART_LOGE/W/I/V` call sites (`uart_debug_config.h`) still use
  their own compile-time-only level (`WAVEX_UART_DEBUG_LEVEL`, currently
  errors-only). Folding them into the UART_PROTOCOL module would make link
  protocol detail runtime-tunable like everything else.
- The ~100 legacy `WAVEX_LOG_DAISY(MODULE, ...)` call sites are all INFO via
  the compatibility alias. Each deserves a real level (boot landmarks stay
  INFO; per-message chatter becomes DEBUG/TRACE), after which the alias can
  go and chatty modules could default to WARN.

**Why it is not urgent:** the new gates already deliver the goal — default
output is quiet where it was migrated, and any module can be deep-dived at
runtime. These remainders only widen coverage; they change no behavior
until each call site is judged, which is exactly why they shouldn't be
rushed inside the infrastructure commit.

**Fix if picked up:** mechanical per-file passes, one module at a time, with
the level-choice guidance in `docs/logging.md`.

---

## The Daisy image is compiled `-O0`; the `-O2` decision is unmade

**Found at the 2026-08-29 bench session.** The whole Daisy image, including
all DSP, builds at `-O0`. Nobody chose that: `firmware/daisy/CMakeLists.txt`
set no compile flags of its own, and libDaisy's toolchain file forces
`CMAKE_{C,CXX}_FLAGS_{DEBUG,RELEASE}` to `""` as `CACHE INTERNAL`
(`ArmGNUToolchain.cmake:50-58`), so `-DCMAKE_BUILD_TYPE=Release` contributes
nothing and a command-line `-DCMAKE_CXX_FLAGS_RELEASE=...` is overwritten on
every configure.

A knob that does work now exists, defaulting to `-O0` so nothing changes
silently:

```
cd firmware/daisy && make BUILD_DIR=build-O2 CMAKE_EXTRA_ARGS="-DWAVEX_DAISY_OPT=-O2"
```

A separate `BUILD_DIR` keeps the `-O0` image intact for A/B. First results
from that build: `rb_pop_stereo` inlines into `Callback()` entirely and the
image drops from 694 KB to 562 KB (-19%).
`docs/daisy_rt_audio_coding_guide.md` §8 asks for `-O3`.

**Why it is not urgent:** raising the optimization level on a real-time audio
target alters timing everywhere and can expose latent UB that `-O0` was
masking. It needs a deliberate decision and a bench pass with DWT numbers,
not a flag flip — and the audio path is currently fast enough at `-O0` after
the 2026-08-29 regression fix.

The 2026-08-31 review found a second reason this needs measurement rather than
a paper decision. A live digital control update currently performs one
`pow()` plus up to two SVF coefficient recomputations (`tan()`) for each of
eight playing voices inside the callback. The 1 kHz analog control tick - now compiled out by default, see
`roadmap.md` § Analog CV is deferred, so this no longer applies to a stock
build - also evaluates `CvShapeCutoff()`, which contains two `expf()` calls. These paths are
bounded and run only on control updates/ticks, but their worst-case callback
cost at `-O0` has not been measured. If it is material, the likely code fix is
to calculate immutable filter coefficients once per shared update and publish
them, not merely to rely on optimization flags.

**The baseline moved on 2026-09-01.** The Daisy now boots at 480 MHz rather
than 400 (`hw.Init(true)`, see `roadmap.md` § Outstanding hardware
verification), which is a ~20% core speed-up this entry's reasoning predates.
Measure the boosted `-O0` image FIRST: if the callback now has comfortable
headroom, the case for `-O2` weakens considerably, and the argument above for
publishing precomputed filter coefficients weakens with it. Do not compare a
new `-O2` image against an old 400 MHz `-O0` number — that conflates two
changes, which is exactly the mistake the ITCM entry warns about.

**Fix if picked up:** flash separate `-O0` and `-O2` images and use the DWT
counter (`docs/performance_monitoring.md` Part 1) while eight voices sound and
cutoff/resonance controls sweep continuously. Record callback maximum, budget
headroom, and underruns, then run at least a one-hour audio/SD soak on each
image. Decide the default from those measurements. If coefficient publication
is needed, add host tests for numerical equivalence and block-boundary update
semantics before changing the callback. This is also the prerequisite for
[LTO on the Daisy image](#lto-on-the-daisy-image) — LTO at `-O0` buys
essentially nothing.

---

## No protocol message can act on a loaded sample

**Observed at the 2026-08-29 bench session.** There is no way to unload,
delete, rename or reorder samples in RAM. The Daisy owns `s_loaded_samples`
plus a memory manager, and `SampleMetadata` already carries `name`,
`sample_id`, `generation` and a resident flag; the frontend has a read-only
Sample Memory page. What is missing is any *message to act on* a loaded
sample.

**Why it is not urgent:** this is a feature with a protocol design in front
of it, not a defect — nothing misbehaves today, the capability is simply
absent. It also wants the Voice/Preset entity below to be settled first, so
that "unload" has a defined effect on anything referencing the sample.

**Fix if picked up:** design the ops as a single `MSG_SAMPLE_OP` verb rather
than one message per action, per the reservation table in
`features/feature-expansion-ideas.md`; round-trip test in the same commit
(AGENTS.md rule 4).

---

## Voice / Preset does not exist as an entity

**Observed at the 2026-08-29 bench session.** Envelopes and filter settings
are global page state on the Play page, not properties of anything nameable
or saveable. The pieces exist but unowned: `VoiceLiveParams` /
`VoiceTriggerParams` carry filter and envelope values on the Daisy, the Play
page edits them globally via `MSG_CONTROL_CHANGE`, and nothing associates
them with a sample or a name.

**Target shape** (set at the bench): a **Voice/Preset** is a named entity,
saveable to and loadable from the card, consisting of a sample with
key-tracking settings, Env → Gain, Env → Filter, filter settings, and
modulation settings and wirings.

**Why it is not urgent:** it is the largest item recorded here and needs a
protocol and an on-disk format decided before any UI is built — and
`features/instrument-model.md` already owns most of that design space
(presets, zones, velocity layers, the WXCF container). Building a second
preset entity beside it is the expensive outcome.

**Fix if picked up:** resolve it *as* the instrument model rather than
alongside it — decide whether a Voice/Preset is an `Instrument` with one
zone, or a distinct lighter entity, before writing either.

---

## Busy overlay presents errors as if they were work in flight

**Observed at the 2026-08-29 bench session.** "Sample will not fit" spins for
several seconds and then becomes "No response from backend — Tap to
dismiss"; "Backend timeout" also lingers. Errors are shown with the same call
used for operations genuinely in flight — `BusyOverlay::show(caption, detail,
timeout_ms)` — so they get a spinner and a timeout, and when it expires
`onTimeout()` (`ui_busy_overlay.cpp:37`) rewrites the caption into a
different, wrong error. The header exposes no error entry point at all.

**Why it is not urgent:** it misreports a failure that has already happened
rather than causing one, and the overlay itself is correct for its intended
case (see roadmap § 1.5.4).

**Fix if picked up:** add a `BusyOverlay::showError()` with no spinner, no
timeout mutation, and a ~2 s auto-dismiss; leave `show()` for in-flight work
only.

---

## Encoder read-then-clear window is narrowed, not closed

**From the 2026-08-29 ESP32-P4 review (E-ENC1), partly fixed the same day.**
`__atomic_fetch_add` / `__atomic_exchange_n` replaced the cross-core-unsafe
`portSET_INTERRUPT_MASK_FROM_ISR()`, both `esp_err_t` returns are now
checked, and a failed clear no longer zeroes the baseline. What remains is
the hardware read-then-clear race in `pcnt_task.cpp`: counts arriving between
`pcnt_unit_get_count` and `pcnt_unit_clear_count` are lost.

The review's suggested fix — free-run and never clear — is **not safe as
written**: the unit is configured `high_limit = INT16_MAX` / `low_limit =
INT16_MIN`, and the `pulse_cnt` driver resets the count to zero on reaching
either limit, so a free-running counter yields one large bogus delta per
±32767 counts. The counter is instead re-centred only past ±8000, so the
lossy window went from every poll during movement to roughly one per 8000
counts (~85 revolutions).

**Why it is not urgent:** losing a fraction of a detent once per ~85
revolutions is imperceptible. Closing it properly means the driver's
watch-point callbacks — an ISR, needing an IRAM-safety audit
(`docs/esp32p4_coding_guide.md` §4) and bench time.

**Fix if picked up:** PCNT watch-point callbacks, or fold the 500 Hz poll
into the UI task's own loop since it is the sole consumer at 31 Hz.

---

## Caller-less API surface on the ESP32, and `window_manager.cpp`

**From the 2026-08-29 ESP32-P4 review (E-DEAD1), partly cleared the same
day.** `parse_browse_response`, the `shared_packet_handler` fossil and the
demo page trio were deleted. Still present and grep-verified caller-less:

- `inter_mcu.h:156` declares `inter_mcu_toggle_inversion`, defined nowhere
  (an undefined-reference trap); `inter_mcu_toggle_debug` is defined but
  never declared or called; `inter_mcu_send_test_messages`,
  `inter_mcu_process_packet_data` (books every byte as type 0xFF),
  `inter_mcu_set_suspended` and `uart_link_stop` have no callers.
- `pcnt_task.cpp`: `pcnt_get_reading` / `pcnt_get_raw_count` /
  `pcnt_reset_counter` are caller-less, and the `prev_count`/`count`
  bookkeeping is immediately zeroed, so `pcnt_get_reading` can only ever
  return `{0,0,…}`.
- `common/window_manager.cpp` — no external callers, and carries a real
  `lv_pct` arithmetic bug at :60 and :187 for whoever revives it.
- `SoftkeyBar::focusNext` / `pressFocused` — the encoder-drives-softkey-focus
  model in `docs/ui-architecture.md` was never wired.
- `ui_sample_detail.cpp` shows a hard-coded "44.1 kHz / 2:34" and appears
  unreachable.

**Why it is not urgent:** none of it executes. The risk is misleading a
future reader, not misbehaviour — and `inter_mcu_toggle_inversion` fails at
link time rather than silently if anyone does call it.

**Fix if picked up:** delete in one pass rather than opportunistically, so
the grep verification is done once against a known tree state.

---

## The keypad matrix is configured 8x10 because that is what the code passes

**From the 2026-08-29 ESP32-P4 review (E-CFG1).** `WAVEX_TCA8418_COLUMNS`
now records the configured value instead of leaving the code to hardcode its
own, but **nobody has checked how many columns are actually wired.** Getting
this wrong silently stops a column being scanned — there is no error, just
keys that never report.

Related and also unresolved from that pass: the board-availability comment in
`pin_config.h` excludes assigned pins 6, 14, 15, 34 and 40.

**Why it is not urgent:** it needs a schematic or a continuity check rather
than a guess, and guessing is what the header's own UNVERIFIED warning
exists to prevent. The keypad has never been confirmed working on hardware
at all (roadmap § Outstanding hardware verification), so this is one input to
that bench pass, not a separate task.

**Fix if picked up:** settle it during the keypad bench pass — press a key in
each physical column and confirm every one reports.

---

## Backend link counters are gated behind `WAVEX_DAISY_UART_PERF_DEBUG`

**From the August 2026 hardware pass over the ported UI.**
`DiagPushMessage::link_*` is populated only when that flag is on, because
timing every `UartLinkProcess` call is the overhead the flag exists to gate.
The Diagnostics Link tab therefore shows the frontend's view only.

**Why it is not urgent:** the frontend's own view of the link is the one that
matters for the common "is the link alive" question, and the tab is not
currently claiming to show backend figures.

**Fix if picked up:** decide one of — leave as-is and label the gap on the
tab; split the flag so frame/byte counts (cheap) are always on and only the
microsecond timing is gated; or accept the timing cost permanently, which
needs a measured number first.

---

## Diagnostics stats that are still uninstrumented

**Residual from the diagnostics page build-out** (spec retired 2026-08-31;
the page itself shipped). Not measured anywhere today: round-trip link
latency, active voice count, per-CC and per-note detail, and dropped/late
MIDI event counts. The MIDI tab's wire fields exist and are parsed, but the
Daisy has no sequencer or tempo follower to fill them, so they read zero and
the tab says so.

**Why it is not urgent:** the MIDI counters should follow the Phase 2
sequencer work rather than lead it — there is nothing to count until the
sequencer runs. Round-trip latency and active voices are genuinely useful
now, but neither has blocked a diagnosis yet.

**Fix if picked up:** active voices is nearly free (`VoiceManager` already
knows); round-trip latency needs an echo message with an ingest timestamp,
which is a protocol addition and wants the same round-trip test as any
other.
