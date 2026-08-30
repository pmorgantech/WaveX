# Backlog

Work that is worth doing but is not scheduled into a roadmap phase. Items here
are deliberately *not* in [`roadmap.md`](roadmap.md): that file tracks the
phased build-out of the instrument, and mixing opportunistic engineering work
into it makes the phase gates harder to read.

Each entry should record **why it is not urgent**, so a future reader can tell
whether the reasoning still holds. An item whose justification has expired is
more useful than an item with no justification at all.

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
larger win and a prerequisite, and it is already recorded — that file explains
why `-O0` is the default (it is what the firmware has always been built with,
unintentionally) and `docs/daisy_rt_audio_coding_guide.md` §8 asks for `-O3`.
Measure `-O2` with DWT first. Only then does `-flto` become an interesting
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
SPI-1..SPI-5 in
[`code_review_esp32_20260829.md` §7](code_review_esp32_20260829.md): driver-owned
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
work; the cycle is stronger than that doc admits. Tracked as E-ARCH1 in
[`code_review_esp32_20260829.md`](code_review_esp32_20260829.md).

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
