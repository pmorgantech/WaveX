# Performance Monitoring

Two unrelated measurement problems live here, because `docs/roadmap.md` cites
this file for both.

- **[Part 1 - Daisy audio callback](#part-1---daisy-audio-callback)**: DWT cycle
  counting for per-block DSP cost. This is what the roadmap's DWT items mean.
- **[Part 2 - ESP32 UI rendering](#part-2---esp32-ui-rendering)**: LVGL FPS and
  render cost. This is what the roadmap's "FPS + UI-task CPU" items mean, and
  it was missing until 2026-08-30 - the roadmap had been pointing at a document
  that only covered the Daisy.

---

## Part 1 - Daisy audio callback

Two facilities already exist and are wired up in production code. **Do not
write a new DWT/CPU-load header** — a hand-rolled scaffold like that sat in
`main.cpp` until Review M11 deleted it because nothing read its output; see
the comment at `main.cpp:48-51`. Use one of these two instead.

### Overall callback CPU load: libDaisy's `CpuLoadMeter`

Vendored at `firmware/daisy/libs/libDaisy/src/util/CpuLoadMeter.h` (a
submodule — do not edit it locally) and already instantiated as
`s_cpu_load_meter` in `audio_engine.cpp:279` (`WAVEX_DTCM_DATA`-placed). The
real API:

```cpp
s_cpu_load_meter.Init(sample_rate, block_size);  // audio_engine.cpp:~1581

// In the audio callback:
s_cpu_load_meter.OnBlockStart();  // audio_engine.cpp:1592
// ... DSP ...
s_cpu_load_meter.OnBlockEnd();    // audio_engine.cpp:1699

// Read out anywhere (main loop, diagnostics push):
float avg = s_cpu_load_meter.GetAvgCpuLoad();  // 0.0-1.0, smoothed
float min = s_cpu_load_meter.GetMinCpuLoad();
float max = s_cpu_load_meter.GetMaxCpuLoad();
```

`WaveX::AudioEngine::GetAvgCpuLoad()` (`audio_engine.h:155`,
`audio_engine.cpp:2809`) exposes the average to the rest of the firmware —
it feeds both `MSG_HEARTBEAT`'s CPU fields and `MSG_DIAG_PUSH`'s
`engine_cpu_x10` (`diag_push.cpp:116`). There is a smoothing-constant
gotcha documented right above the `OnBlockStart()` call site
(`audio_engine.cpp:~1581`): libDaisy's `Init()` defaults its window count to
`1.0f` for a reason specific to this project's block size — read that
comment before changing the window.

### Per-section cycle cost: `WaveX::Profiling::Profiler`

A separate, zone-based cycle counter (`firmware/daisy/src/profiling/profiler.h`/`.cpp`),
gated by `WAVEX_PROFILING_ENABLED` (compiles to nothing when off, so it costs
nothing in a release image). This is what to reach for when you need to know
*which section* of the callback is expensive, not just the total.

```cpp
// At file scope, once per zone:
PROFILE_DEFINE_ZONE(my_section);

// Once at init:
PROFILE_REGISTER_ZONE(my_section);

// Around the code to measure - either form:
PROFILE_SCOPE(my_section);                 // RAII, ends at scope exit
// ...or...
PROFILE_BEGIN(my_section);
/* ... */
PROFILE_END(my_section);
```

Real zones already registered in `audio_engine.cpp`: `audio_callback`,
`wav_pump_io`, `format_conversion`, `ring_buffer_push`, `prebuffer_audio`,
`sd_refill` — grep `PROFILE_SCOPE`/`PROFILE_REGISTER_ZONE` there for live
examples.

Read results via `Profiler::GetZoneCount()` / `Profiler::GetZone(i)`
(`main.cpp:121-123` dumps every zone this way), which returns a
`ProfileZone` carrying `total_cycles`, `min_cycles`, `max_cycles`,
`entry_count`, `last_cycles`, plus `GetAvgCycles()` and
`GetStats(avg_us, min_us, max_us)` for the converted form.
`Profiler::ResetAll()` clears every zone's counters — it runs from the main
loop while `End()` runs from the audio ISR, so it takes a
`ScopedIrqBlocker` internally to avoid tearing the non-atomic 64-bit
`total_cycles` accumulator (`profiler.cpp`, fixed in the same session this
doc was corrected).

`WaveX::Profiling::GetCycles()` / `CyclesToMicroseconds()` are the raw
DWT-cycle-counter primitives underneath both facilities, if you need a
one-off timestamp rather than either wrapper.

---

## Part 2 - ESP32 UI rendering

### What is instrumented

`CONFIG_LV_USE_SYSMON=y` with `CONFIG_LV_USE_PERF_MONITOR=y` and
`CONFIG_LV_USE_MEM_MONITOR=y` (both in `firmware/esp32/sdkconfig` and
`sdkconfig.defaults`) draw two small overlays that LVGL maintains itself:

- **Top left - performance.** Frames per second and LVGL's CPU figure.
- **Top right - memory.** Used bytes and fragmentation of the LVGL pool
  (`CONFIG_LV_MEM_SIZE_KILOBYTES=128`). This one is not decoration: the drop
  shadow experiment (roadmap § 0.3 item 2) hung the UI purely by asking for a
  layer the pool could not satisfy, and that overlay is where such a thing
  becomes visible before it becomes a freeze.

They are aligned top-left and top-right specifically to stay clear of the
softkey bar along the bottom.

**Read the CPU number with care.** LVGL derives it from its own idle time, not
from the scheduler, so it describes how busy LVGL's refresh loop is, not core
load. It is a valid *relative* measure for A/B comparisons like the one below
and a poor absolute one. For true core load, use FreeRTOS run-time stats.

**This is not a shipping configuration.** The overlays draw on top of the
product UI. Turn both off before any build that is not a measurement build.

### Getting numbers instead of impressions

The on-screen overlay is fine for a glance and useless for a comparison — the
values move, and "it felt slower" is not a measurement. For anything you intend
to act on, use log mode:

```
CONFIG_LV_USE_LOG=y
CONFIG_LV_LOG_LEVEL_USER=y        # only the sysmon line; suppresses LVGL's own chatter
CONFIG_LV_LOG_PRINTF=y            # route to stdout, i.e. the console UART
CONFIG_LV_USE_PERF_MONITOR_LOG_MODE=y
```

LVGL then prints one line roughly every 300 ms and hides the perf overlay
(`lv_sysmon.c:125`), which also removes the overlay's own draw cost from what
you are measuring:

```
sysmon: 42 FPS (refr_cnt: 13 | redraw_cnt: 13), refr 21ms (render 18ms | flush 3ms), CPU 61%
```

**The `render` / `flush` split is the most useful thing here**, and the overlay
does not show it. `render` is LVGL drawing into the buffer — the part a draw
unit like the PPA can accelerate. `flush` is getting those pixels onto the
panel, which it cannot. A page dominated by `flush` will not be helped by any
amount of draw acceleration, and that single distinction decides whether a slow
page is worth optimising in LVGL at all.

Capture and summarise:

```
python3 scripts/serial_log.py --vid 303a --out logs/ppa-off.log
# ... drive the UI, one page at a time ...
python3 scripts/sysmon_stats.py logs/ppa-off.log logs/ppa-on.log
```

`sysmon_stats.py` prints mean, median, p95, min and max per field, and for two
captures a median-to-median delta plus each run's interquartile range. Use the
IQR: **if the two runs' middle-50% ranges overlap, a moving median is not yet
evidence.** It drops the first few samples by default (`--warmup`), because a
freshly opened page redraws everything once and those samples describe the
transition rather than the steady state.

Capture one page per file. FPS is not comparable across pages, so a single log
covering three screens averages away the thing you wanted to see.

### A/B procedure: is `CONFIG_LV_USE_PPA` actually faster?

The roadmap's outstanding item. The PPA draw unit accelerates unrounded,
fully-opaque rectangle fills, but it also replaces LVGL's global
cache-invalidation callback (previously a free no-op) with a whole-buffer
`esp_cache_msync` called twice per draw task - so it can plausibly lose. The
only way to know is to measure both ways.

1. Build and flash with `CONFIG_LV_USE_PPA=y` (current state).
2. Open a page and let the FPS reading settle. Use the same page, the same
   content and the same interaction each time - FPS is meaningless across
   different pages. Good choices: the diagnostics tab (many cards, live
   updating) and a page the panel visibly struggles with.
3. Record FPS and the CPU figure.
4. Flip **only** `CONFIG_LV_USE_PPA` to `n` in `sdkconfig`. Leave
   `CONFIG_LVGL_PORT_ENABLE_PPA` alone - it is a different PPA consumer, and
   changing both at once makes the result unattributable. Note that
   `CONFIG_LV_DRAW_BUF_ALIGN` must stay 128 while PPA is on and may go back to
   4 when it is off; changing it also changes allocation behaviour, so for a
   clean comparison leave it at 128 for both runs.
5. Rebuild, reflash, repeat step 2 on the same page.

A difference smaller than the run-to-run spread is not a difference. If PPA
comes out slower, the cache-invalidation callback is the first suspect, not the
PPA hardware - see roadmap § 0.3 item 1.

### Attributing a slow page

FPS tells you *that* a page is slow, not *why*. Two LVGL settings turn the
question into an answerable one, both off by default and both worth enabling
temporarily rather than shipping:

- `CONFIG_LV_USE_REFR_DEBUG=y` tints each redrawn area a random colour. If a
  page is slow because it redraws far more than it changed, this shows it
  immediately - large flashing regions on a page where only one label updated
  means the invalidation is too coarse, which is a layout problem, not a
  rendering one.
- `CONFIG_LV_USE_PROFILER` with `CONFIG_LV_USE_PROFILER_BUILTIN` gives
  per-draw-task timings, which is what distinguishes "one expensive widget"
  from "a thousand cheap ones".

Check `LV_USE_REFR_DEBUG` first. It is cheaper to interpret and, on this
codebase, coarse invalidation is the more likely cause: a full-width redraw
costs the same whether the PPA is helping or not.
