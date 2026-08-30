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

Great—here’s a drop-in way to measure timing with the **DWT cycle counter** (and a light SysTick fallback). It gives you per-block duration, average/max over a window, and a computed “CPU load” vs. the audio block period.

## 1) DWT + CPU-load helper (single header)

Create `src/util/CpuLoadMeter.h`:

```cpp
#pragma once
#include <stdint.h>
#include <string.h>

// Daisy / STM32H7 notes:
// - DWT->CYCCNT counts CPU cycles at core clock (Seed default: 480 MHz).
// - Make sure TRCENA is set (we do that in Init()).

#ifndef DAISY_SYSCLK_HZ
#define DAISY_SYSCLK_HZ 480000000u // adjust if you changed clocks
#endif

class DwtTimer
{
public:
    static void Init()
    {
        // Enable DWT CYCCNT
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DWT->LAR = 0xC5ACCE55; // unlock on some chips; harmless if not required
        DWT->CYCCNT = 0;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    }

    static inline void Reset() { DWT->CYCCNT = 0; }
    static inline uint32_t Now() { return DWT->CYCCNT; }

    static inline float CyclesToUs(uint32_t cyc)
    {
        return (float)cyc * (1e6f / (float)DAISY_SYSCLK_HZ);
    }
    static inline float CyclesToMs(uint32_t cyc)
    {
        return (float)cyc * (1e3f / (float)DAISY_SYSCLK_HZ);
    }
};

class CpuLoadMeter
{
public:
    // window_count: how many blocks to average over
    void Init(float sample_rate_hz, int block_size, int window_count = 100)
    {
        sr_ = sample_rate_hz;
        bs_ = block_size;
        win_ = window_count > 0 ? window_count : 1;
        Reset();
        DwtTimer::Init();
    }

    // Call at the start/end of your audio callback's DSP section.
    inline void BeginBlock()
    {
        start_cycles_ = DwtTimer::Now();
    }
    inline void EndBlock()
    {
        const uint32_t end = DwtTimer::Now();
        const uint32_t cyc = end - start_cycles_;

        // Update stats
        accum_cycles_ += cyc;
        if(cyc > max_cycles_) max_cycles_ = cyc;
        if(++count_ >= (uint32_t)win_)
        {
            avg_cycles_ = accum_cycles_ / (float)count_;
            accum_cycles_ = 0;
            count_ = 0;
        }
        last_cycles_ = cyc;
    }

    // Metrics
    inline float BlockPeriodMs() const { return 1000.0f * (float)bs_ / sr_; }
    inline float LastMs()        const { return DwtTimer::CyclesToMs(last_cycles_); }
    inline float AvgMs()         const { return DwtTimer::CyclesToMs((uint32_t)avg_cycles_); }
    inline float MaxMs()         const { return DwtTimer::CyclesToMs(max_cycles_); }

    inline float LastLoad() const { return LastMs() / BlockPeriodMs(); }
    inline float AvgLoad()  const { return AvgMs()  / BlockPeriodMs(); }
    inline float MaxLoad()  const { return MaxMs()  / BlockPeriodMs(); }

    void Reset()
    {
        start_cycles_ = last_cycles_ = max_cycles_ = 0;
        accum_cycles_ = 0;
        avg_cycles_   = 0.0f;
        count_        = 0;
    }

private:
    float      sr_ = 48000.0f;
    int        bs_ = 48;
    int        win_ = 100;

    uint32_t   start_cycles_ = 0;
    uint32_t   last_cycles_  = 0;
    uint32_t   max_cycles_   = 0;
    uint64_t   accum_cycles_ = 0; // avoid overflow in long windows
    float      avg_cycles_   = 0.0f;
    uint32_t   count_        = 0;
};
```

> If you change the Seed’s core clock, set `DAISY_SYSCLK_HZ` accordingly (or make it a CMake `-D`).

## 2) Use it in your libDaisy app

```cpp
#include "daisy_seed.h"
#include "CpuLoadMeter.h"
#include "daisysp.h"
using namespace daisy;

DaisySeed     hw;
CpuLoadMeter  meter;

static void AudioCb(AudioHandle::InputBuffer  in,
                    AudioHandle::OutputBuffer out,
                    size_t size)
{
    meter.BeginBlock();

    // --- your DSP here ---
    for(size_t i = 0; i < size; ++i)
    {
        float sig = in[0][i]; // example pass-through
        out[0][i] = sig;
        out[1][i] = sig;
    }
    // ----------------------

    meter.EndBlock();
}

int main(void)
{
    hw.Configure();
    hw.Init();

    // Audio config
    auto cfg = hw.audio_handle.GetConfig();
    cfg.blocksize  = 48; // example
    cfg.samplerate = SaiHandle::Config::SampleRate::SAI_48KHZ;
    hw.audio_handle.Init(cfg, AudioCb);

    // Init meter
    meter.Init(hw.AudioSampleRate(), hw.AudioBlockSize(), 200);

    hw.StartAudio(AudioCb);

    // Simple text telemetry over Serial (optional)
    // hw.StartLog(true); // USB CDC
    for(;;)
    {
        System::Delay(500);
        // hw.PrintLine("Blk %.2f ms | last %.3f ms (%.0f%%) avg %.3f ms (%.0f%%) max %.3f ms (%.0f%%)",
        //   meter.BlockPeriodMs(),
        //   meter.LastMs(), meter.LastLoad()*100.0f,
        //   meter.AvgMs(),  meter.AvgLoad()*100.0f,
        //   meter.MaxMs(),  meter.MaxLoad()*100.0f);
    }
}
```

That’s all you need. You’ll get stable, microsecond-accurate timing per audio block, plus average and peak “CPU load” numbers.

Here’s a drop-in header that adds **P95/P99** over a rolling window using the DWT cycle counter. It’s a fixed-capacity ring buffer (default 256 samples), so there’s no heap use. Percentiles are computed with `std::nth_element` on a stack copy (fast and lightweight at this size).

### `CpuLoadMeter.h` (with P95/P99)

```cpp
#pragma once
#include <stdint.h>
#include <string.h>
#include <algorithm> // nth_element

#ifndef DAISY_SYSCLK_HZ
#define DAISY_SYSCLK_HZ 480000000u // adjust if you changed the core clock
#endif

class DwtTimer
{
public:
    static void Init()
    {
        // Enable DWT->CYCCNT
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DWT->LAR = 0xC5ACCE55;                    // harmless if not needed
        DWT->CYCCNT = 0;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    }
    static inline uint32_t Now() { return DWT->CYCCNT; }

    static inline float CyclesToUs(uint32_t cyc) { return cyc * (1e6f / (float)DAISY_SYSCLK_HZ); }
    static inline float CyclesToMs(uint32_t cyc) { return cyc * (1e3f / (float)DAISY_SYSCLK_HZ); }
};

// Fixed-capacity, no-heap meter. BUFFER_CAP should be a power-of-two (e.g., 128/256).
template <size_t BUFFER_CAP = 256>
class CpuLoadMeter
{
public:
    static_assert(BUFFER_CAP >= 16, "BUFFER_CAP too small");
    static_assert((BUFFER_CAP & (BUFFER_CAP - 1)) == 0, "BUFFER_CAP must be power-of-two");

    void Init(float sample_rate_hz, int block_size, int avg_window = 100)
    {
        sr_   = sample_rate_hz;
        bs_   = block_size;
        win_  = avg_window > 0 ? avg_window : 1;
        Reset();
        DwtTimer::Init();
    }

    inline void BeginBlock() { start_cycles_ = DwtTimer::Now(); }

    inline void EndBlock()
    {
        const uint32_t cyc = DwtTimer::Now() - start_cycles_;
        last_cycles_ = cyc;

        // rolling stats
        accum_cycles_ += cyc;
        if(cyc > max_cycles_) max_cycles_ = cyc;
        if(++count_ >= (uint32_t)win_)
        {
            avg_cycles_   = (float)accum_cycles_ / (float)count_;
            accum_cycles_ = 0;
            count_        = 0;
        }

        // ring buffer for percentiles
        buf_[idx_] = cyc;
        idx_ = (idx_ + 1) & (BUFFER_CAP - 1);
        if(filled_ < BUFFER_CAP) ++filled_;
    }

    // Time metrics (ms)
    inline float BlockPeriodMs() const { return 1000.0f * (float)bs_ / sr_; }
    inline float LastMs()        const { return DwtTimer::CyclesToMs(last_cycles_); }
    inline float AvgMs()         const { return DwtTimer::CyclesToMs((uint32_t)avg_cycles_); }
    inline float MaxMs()         const { return DwtTimer::CyclesToMs(max_cycles_); }

    // Load (0.0 .. 1.0+)
    inline float LastLoad() const { return LastMs() / BlockPeriodMs(); }
    inline float AvgLoad()  const { return AvgMs()  / BlockPeriodMs(); }
    inline float MaxLoad()  const { return MaxMs()  / BlockPeriodMs(); }

    // Percentiles over the rolling buffer
    float P95Ms() const { return PercentileMs(0.95f); }
    float P99Ms() const { return PercentileMs(0.99f); }
    float P95Load() const { return P95Ms() / BlockPeriodMs(); }
    float P99Load() const { return P99Ms() / BlockPeriodMs(); }

    void Reset()
    {
        start_cycles_ = last_cycles_ = max_cycles_ = 0;
        accum_cycles_ = 0;
        avg_cycles_   = 0.0f;
        count_        = 0;
        idx_          = 0;
        filled_       = 0;
        for(size_t i = 0; i < BUFFER_CAP; ++i) buf_[i] = 0;
    }

private:
    float PercentileMs(float p) const
    {
        const size_t n = filled_;
        if(n == 0) return 0.0f;

        // Copy to stack and select the k-th element (0-based).
        uint32_t tmp[BUFFER_CAP];
        for(size_t i = 0; i < n; ++i) tmp[i] = buf_[i];

        // Clamp p to [0,1]
        if(p < 0.0f) p = 0.0f; else if(p > 1.0f) p = 1.0f;

        const size_t k = (size_t)((p * (float)(n - 1)) + 0.5f); // nearest-rank
        std::nth_element(tmp, tmp + k, tmp + n);
        return DwtTimer::CyclesToMs(tmp[k]);
    }

    // config
    float    sr_  = 48000.0f;
    int      bs_  = 48;
    int      win_ = 100;

    // running stats
    uint32_t start_cycles_ = 0;
    uint32_t last_cycles_  = 0;
    uint32_t max_cycles_   = 0;
    uint64_t accum_cycles_ = 0;
    float    avg_cycles_   = 0.0f;
    uint32_t count_        = 0;

    // percentile buffer (ring)
    uint32_t buf_[BUFFER_CAP];
    size_t   idx_    = 0;
    size_t   filled_ = 0;
};
```

### Usage (unchanged, now with P95/P99)

```cpp
// Init as before…
meter.Init(hw.AudioSampleRate(), hw.AudioBlockSize(), 200);

// In a telemetry loop (USB CDC or UART)
System::Delay(500);
// hw.PrintLine("blk %.2f ms  last %.3f (%.0f%%)  avg %.3f (%.0f%%)  max %.3f (%.0f%%)  p95 %.3f (%.0f%%)  p99 %.3f (%.0f%%)",
//   meter.BlockPeriodMs(),
//   meter.LastMs(), meter.LastLoad()*100.0f,
//   meter.AvgMs(),  meter.AvgLoad()*100.0f,
//   meter.MaxMs(),  meter.MaxLoad()*100.0f,
//   meter.P95Ms(),  meter.P95Load()*100.0f,
//   meter.P99Ms(),  meter.P99Load()*100.0f);
```

### Notes

* Default buffer is **256 blocks** (\~256 ms if your block is 1 ms). Make it larger if you want a longer statistical horizon; keep it a power of two for cheap wrap-around.
* `std::nth_element` is O(n) average and avoids a full sort. For 256 items, it’s trivial on the H7.
* Percentiles are **rolling** over the last `BUFFER_CAP` blocks, which is what you want to catch recent “hiccups.”
* If you later enable DMA and caches, remember to align any DMA buffers to 32-byte cache lines; this meter is unaffected.

If you prefer **no `<algorithm>`**, I can swap in a tiny quickselect implementation.


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
