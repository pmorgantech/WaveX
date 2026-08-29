#include "diag_push.h"

#include "audio/audio_engine.h"
#include "comm/daisy_uart_link.h"
#include "config/hardware_config.h"
#include "spi_protocol/protocol.h"
#include "storage/sd_sdio.h"

namespace WaveX {
namespace Comm {

namespace {

bool s_subscribed = false;
uint32_t s_interval_ms = 500;  // 2 Hz default
uint32_t s_last_push_ms = 0;

// Clamped so a malformed or hostile subscription cannot ask the backend to
// spend main-loop time it needs for the audio ring.
constexpr uint8_t kMinHz = 1;
constexpr uint8_t kMaxHz = 10;

template <typename T>
uint16_t Sat16(T v) {
    return v > static_cast<T>(0xFFFF) ? 0xFFFFu : static_cast<uint16_t>(v);
}

}  // namespace

void DiagSubscribe(bool enable, uint8_t interval_hz) {
    s_subscribed = enable;
    if (interval_hz < kMinHz) {
        interval_hz = kMinHz;
    } else if (interval_hz > kMaxHz) {
        interval_hz = kMaxHz;
    }
    s_interval_ms = 1000u / interval_hz;

    // Drain the counters on subscribe. Otherwise the first push reports
    // everything accumulated since boot as if it happened in one interval -
    // an alarming, entirely fictional spike on the first frame the user sees.
    if (enable) {
        uint32_t pushes = 0, discards = 0, underruns = 0;
        WaveX::AudioEngine::TakeStreamCounters(pushes, discards, underruns);
        WaveX::AudioEngine::TakeRingLowWater();
        uint32_t b = 0, r = 0, a = 0, mn = 0, mx = 0;
        WaveX::AudioEngine::TakeIOThroughput(b, r, a, mn, mx);
#if WAVEX_DAISY_UART_PERF_DEBUG
        UartPerfSample perf;
        TakeUartPerf(perf);
#endif
        s_last_push_ms = 0;  // push immediately rather than after an interval
    }
}

bool DiagIsSubscribed() {
    return s_subscribed;
}

void DiagPushTick(uint32_t now_ms) {
    if (!s_subscribed) {
        return;
    }
    if (s_last_push_ms != 0 && (now_ms - s_last_push_ms) < s_interval_ms) {
        return;
    }
    const uint32_t interval_ms = s_last_push_ms ? (now_ms - s_last_push_ms) : s_interval_ms;
    s_last_push_ms = now_ms;

    WaveX::Protocol::DiagPushMessage m;
    m.interval_ms = interval_ms;

    // --- audio ---------------------------------------------------------
    // Callback rate is derived from the block counter rather than assumed
    // from the sample rate: that is the whole point of the figure. It
    // separates "the engine stopped" from "the ring starved", and only the
    // measured rate can tell them apart - if the callback stops, the ring
    // stays full and no underrun is ever detected or logged.
    static uint32_t s_last_blocks = 0;
    const uint32_t blocks = WaveX::AudioEngine::GetCallbackBlocks();
    const uint32_t block_delta = blocks - s_last_blocks;
    s_last_blocks = blocks;
    if (interval_ms > 0) {
        m.callback_hz_x10 = Sat16((static_cast<uint64_t>(block_delta) * 10000ull) / interval_ms);
    }

    m.ring_low_water = Sat16(WaveX::AudioEngine::TakeRingLowWater());

    uint32_t pushes = 0, discards = 0, underruns = 0;
    WaveX::AudioEngine::TakeStreamCounters(pushes, discards, underruns);
    m.ring_pushes = pushes;
    m.ring_discards = discards;
    m.underruns = Sat16(underruns);

    m.engine_cpu_x10 = Sat16(static_cast<uint32_t>(WaveX::AudioEngine::GetAvgCpuLoad() * 1000.0f));
    m.engine_cpu_max_x10 =
        Sat16(static_cast<uint32_t>(WaveX::AudioEngine::GetMaxCpuLoad() * 1000.0f));

    uint32_t prebuf_filled = 0, prebuf_target = 0, wav_rate = 0;
    uint8_t wav_ch = 0, wav_bits = 0;
    WaveX::AudioEngine::GetStreamDebug(prebuf_filled, prebuf_target, wav_rate, wav_ch, wav_bits);
    m.prebuffer_filled = Sat16(prebuf_filled);
    m.wav_sample_rate = wav_rate;
    m.wav_channels = wav_ch;
    m.wav_bits = wav_bits;
    m.playing = WaveX::AudioEngine::IsWavPlaying() ? 1 : 0;
    m.resampling = (wav_rate != 0 && wav_rate != static_cast<uint32_t>(WAVEX_AUDIO_SAMPLE_RATE));

    // --- storage -------------------------------------------------------
    m.sd_mounted = WaveX::Storage::SdSdio::IsMounted() ? 1 : 0;
    m.sd_speed_index = static_cast<uint8_t>(WaveX::Storage::SdSdio::CurrentSpeedIndex());

    uint32_t bytes = 0, reads = 0, avg_us = 0, min_us = 0, max_us = 0;
    WaveX::AudioEngine::TakeIOThroughput(bytes, reads, avg_us, min_us, max_us);
    m.sd_bytes = bytes;
    m.sd_reads = Sat16(reads);
    m.sd_lat_avg_us = Sat16(avg_us);
    m.sd_lat_max_us = Sat16(max_us);

    uint32_t io_errors = 0, last_result = 0;
    WaveX::AudioEngine::GetIOErrors(io_errors, last_result);
    m.sd_errors = Sat16(io_errors);
    m.sd_last_fatfs = static_cast<uint8_t>(last_result);

    WaveX::Protocol::SampleMemStatusMessage mem;
    WaveX::AudioEngine::GetSampleMemStatus(mem);
    m.sample_ram_free = mem.large_free_bytes + mem.small_free_bytes;
    m.sample_ram_largest = mem.largest_free_bytes;
    m.sample_failed_allocs = Sat16(mem.failed_allocs);
    m.sample_count = mem.sample_count;

    // --- link ----------------------------------------------------------
#if WAVEX_DAISY_UART_PERF_DEBUG
    UartPerfSample perf;
    TakeUartPerf(perf);
    m.link_total_us = perf.total_us;
    m.link_max_us = Sat16(perf.max_us);
    m.link_rx_frames = Sat16(perf.rx_frames);
    m.link_tx_frames = Sat16(perf.tx_frames);
    m.link_errors = Sat16(perf.errors);
    m.link_seq_drops = Sat16(perf.seq_drops);
    m.link_queue_overflows = Sat16(perf.queue_overflows);
#endif
    // Without WAVEX_DAISY_UART_PERF_DEBUG these stay zero. Timing every
    // UartLinkProcess call is exactly the overhead that flag gates, and the
    // frontend already shows its own view of the link, so the fields are left
    // unfilled rather than paid for on every build.

    // MIDI and transport counters are left zero: the sequencer and tempo
    // follower are Phase 2 work and none of those counters exist yet. Sending
    // zeros is honest - the frontend labels the tab accordingly - where
    // inventing plausible values would not be.

    UartLinkSend(WaveX::Protocol::MSG_DIAG_PUSH, &m, sizeof(m));
}

}  // namespace Comm
}  // namespace WaveX
