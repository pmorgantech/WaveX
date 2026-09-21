#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace WaveX::Recording {

// Caller owns all buffers for the entire session. Configure runs only while
// detached from the callback. Arm/Start/Stop/Process are producer-only;
// Drain/Complete are consumer-only. No DMA accesses these CPU-owned buffers.
class Capture {
   public:
    enum class Phase : uint32_t { Idle, Armed, Capturing, Stopped };
    enum class End : uint8_t { None, Manual, Limit, Overflow };
    struct Config {
        uint32_t max_frames = 0;
        uint32_t preroll_frames = 0;
        uint16_t threshold = 0;  // 0 = manual; magnitude, including -32768.
        uint8_t channels = 2;
    };
    struct Buffers {
        int16_t* preroll = nullptr;
        int16_t* ring = nullptr;
        int16_t* take = nullptr;
        uint32_t ring_frames = 0;
        uint32_t take_frames = 0;
        uint32_t preroll_frames = 0;
    };

    bool Configure(const Config& config, const Buffers& buffers) {
        if ((config.channels != 1 && config.channels != 2) || !config.max_frames ||
            config.max_frames > 48000u * 120u || config.threshold > 32768 ||
            config.preroll_frames >= config.max_frames ||
            config.preroll_frames > buffers.preroll_frames ||
            config.max_frames > buffers.take_frames || !buffers.take || !buffers.ring ||
            !buffers.ring_frames || (config.preroll_frames && !buffers.preroll))
            return false;
        config_ = config;
        buffers_ = buffers;
        history_head_ = history_count_ = frozen_first_ = frozen_count_ = 0;
        history_drained_ = drained_ = produced_ = 0;
        end_ = End::None;
        read_.store(0, std::memory_order_relaxed);
        write_.store(0, std::memory_order_relaxed);
        phase_.store(static_cast<uint32_t>(Phase::Idle), std::memory_order_relaxed);
        return true;
    }
    void Arm() { Publish(Phase::Armed); }
    void Start() {
        if (State() != Phase::Armed)
            return;
        frozen_count_ = history_count_;
        frozen_first_ = history_count_ == config_.preroll_frames ? history_head_ : 0;
        produced_ = frozen_count_;
        Publish(Phase::Capturing);
    }
    void Stop() {
        if (State() == Phase::Armed || State() == Phase::Capturing)
            Finish(End::Manual);
    }

    // Interleaved q15 frames from the selected source. Caller bounds each block
    // to the audio block size and does float conversion with CMSIS-DSP upstream.
    void Process(const int16_t* frames, uint32_t count) {
        Phase phase = State();
        if (!frames || (phase != Phase::Armed && phase != Phase::Capturing))
            return;
        for (uint32_t i = 0; i < count; ++i) {
            const auto* frame = frames + i * config_.channels;
            if (phase == Phase::Armed) {
                bool trigger = false;
                if (config_.threshold) {
                    for (uint8_t c = 0; c < config_.channels; ++c) {
                        const int32_t value = frame[c];
                        const int32_t magnitude = value < 0 ? -value : value;
                        trigger = trigger || magnitude >= config_.threshold;
                    }
                }
                if (!trigger) {
                    if (config_.preroll_frames) {
                        Copy(buffers_.preroll, history_head_, frame, 1);
                        history_head_ = (history_head_ + 1) % config_.preroll_frames;
                        history_count_ = std::min(history_count_ + 1, config_.preroll_frames);
                    }
                    continue;
                }
                Start();
                phase = Phase::Capturing;
            }
            const uint32_t write = write_.load(std::memory_order_relaxed);
            if (write - read_.load(std::memory_order_acquire) >= buffers_.ring_frames) {
                Finish(End::Overflow);  // Preserve the contiguous prefix, never drop oldest.
                return;
            }
            Copy(buffers_.ring, write % buffers_.ring_frames, frame, 1);
            write_.store(write + 1, std::memory_order_release);
            if (++produced_ == config_.max_frames) {
                Finish(End::Limit);
                return;
            }
        }
    }

    uint32_t Drain(uint32_t budget_frames) {
        const auto phase = State();
        if (phase != Phase::Capturing && phase != Phase::Stopped)
            return 0;
        const uint32_t before = drained_;
        while (budget_frames && history_drained_ < frozen_count_) {
            const uint32_t pos = (frozen_first_ + history_drained_) % config_.preroll_frames;
            const uint32_t n = std::min(
                {budget_frames, frozen_count_ - history_drained_, config_.preroll_frames - pos});
            Copy(buffers_.take, drained_, buffers_.preroll + pos * config_.channels, n);
            history_drained_ += n;
            drained_ += n;
            budget_frames -= n;
        }
        uint32_t read = read_.load(std::memory_order_relaxed);
        const uint32_t available = write_.load(std::memory_order_acquire) - read;
        uint32_t remaining = std::min(budget_frames, available);
        while (remaining) {
            const uint32_t pos = read % buffers_.ring_frames;
            const uint32_t n = std::min(remaining, buffers_.ring_frames - pos);
            Copy(buffers_.take, drained_, buffers_.ring + pos * config_.channels, n);
            drained_ += n;
            read += n;
            remaining -= n;
        }
        read_.store(read, std::memory_order_release);
        return drained_ - before;
    }
    Phase State() const { return static_cast<Phase>(phase_.load(std::memory_order_acquire)); }
    bool Complete() const {
        return State() == Phase::Stopped && history_drained_ == frozen_count_ &&
               read_.load(std::memory_order_relaxed) == write_.load(std::memory_order_acquire);
    }
    uint32_t Frames() const { return drained_; }  // Consumer only.
    End Reason() const { return State() == Phase::Stopped ? end_ : End::None; }

   private:
    static_assert(std::atomic<uint32_t>::is_always_lock_free);
    void Publish(Phase phase) {
        phase_.store(static_cast<uint32_t>(phase), std::memory_order_release);
    }
    void Finish(End end) {
        end_ = end;
        Publish(Phase::Stopped);
    }
    void Copy(int16_t* dst, uint32_t offset, const int16_t* src, uint32_t frames) const {
        std::memcpy(dst + offset * config_.channels,
                    src,
                    static_cast<size_t>(frames) * config_.channels * sizeof(int16_t));
    }
    Config config_{};
    Buffers buffers_{};
    std::atomic<uint32_t> phase_{0}, write_{0}, read_{0};
    uint32_t history_head_ = 0, history_count_ = 0, frozen_first_ = 0, frozen_count_ = 0;
    uint32_t produced_ = 0;
    End end_ = End::None;
    uint32_t history_drained_ = 0, drained_ = 0;
};
}  // namespace WaveX::Recording
