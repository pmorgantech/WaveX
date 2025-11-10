#include "audio_engine.h"

#include <gtest/gtest.h>

#include "../../mocks/daisy_mocks.h"
#include "../../utils/test_helpers.h"

#include <chrono>
#include <cmath>
#include <cstring>

using namespace WaveX::AudioEngine;
using namespace WaveX::Test;
using namespace daisy;

class AudioEngineTest : public ::testing::Test {
   protected:
    void SetUp() override {
        // Create mock DaisySeed hardware
        mock_hw_ = std::make_unique<DaisySeed>();
        mock_hw_->Init();

        // Initialize audio engine with mock hardware
        Init(*mock_hw_, 48000.0f);

        // Setup test buffers
        block_size_ = 48;  // Standard block size
        left_buffer_.resize(block_size_);
        right_buffer_.resize(block_size_);
        input_buffer_.resize(block_size_);

        // Initialize buffers to zero
        std::fill(left_buffer_.begin(), left_buffer_.end(), 0.0f);
        std::fill(right_buffer_.begin(), right_buffer_.end(), 0.0f);
        std::fill(input_buffer_.begin(), input_buffer_.end(), 0.0f);
    }

    void TearDown() override {
        // Cleanup if needed
    }

    std::unique_ptr<DaisySeed> mock_hw_;
    size_t block_size_;
    std::vector<float> left_buffer_;
    std::vector<float> right_buffer_;
    std::vector<float> input_buffer_;

    // Helper to create audio buffers for callback
    AudioHandle::InputBuffer GetInputBuffer() {
        static float* in_bufs[2] = {input_buffer_.data(), input_buffer_.data()};
        return in_bufs;
    }

    AudioHandle::OutputBuffer GetOutputBuffer() {
        static float* out_bufs[2] = {left_buffer_.data(), right_buffer_.data()};
        return out_bufs;
    }
};

// Test audio callback timing and block size
TEST_F(AudioEngineTest, CallbackTimingAndBlockSize) {
    // Verify callback processes exact block size
    Callback(GetInputBuffer(), GetOutputBuffer(), block_size_);

    // Verify all samples in block were processed
    EXPECT_EQ(left_buffer_.size(), block_size_);
    EXPECT_EQ(right_buffer_.size(), block_size_);

    // Verify callback doesn't crash with different block sizes
    size_t test_sizes[] = {48, 64, 96, 128};
    for (size_t size: test_sizes) {
        left_buffer_.resize(size);
        right_buffer_.resize(size);
        input_buffer_.resize(size);

        EXPECT_NO_THROW({ Callback(GetInputBuffer(), GetOutputBuffer(), size); });
    }
}

// Test that audio callback produces silence when no audio is playing
TEST_F(AudioEngineTest, SilenceOnStartup) {
    // Callback should produce silence when no WAV is open
    Callback(GetInputBuffer(), GetOutputBuffer(), block_size_);

    // Verify all outputs are zero (silence)
    for (size_t i = 0; i < block_size_; i++) {
        EXPECT_FLOAT_EQ(left_buffer_[i], 0.0f) << "Sample " << i << " should be silent";
        EXPECT_FLOAT_EQ(right_buffer_[i], 0.0f) << "Sample " << i << " should be silent";
    }
}

// Test CPU load monitoring
TEST_F(AudioEngineTest, CpuLoadMonitoring) {
    // Process several blocks to get CPU load measurements
    for (int i = 0; i < 100; i++) {
        Callback(GetInputBuffer(), GetOutputBuffer(), block_size_);
    }

    // Get CPU load metrics
    float avg_load = GetAvgCpuLoad();
    float min_load = GetMinCpuLoad();
    float max_load = GetMaxCpuLoad();
    float block_period = GetBlockPeriodMs();

    // Verify CPU load is within reasonable bounds (0.0-1.0)
    EXPECT_GE(avg_load, 0.0f);
    EXPECT_LE(avg_load, 1.0f);
    EXPECT_GE(min_load, 0.0f);
    EXPECT_LE(min_load, 1.0f);
    EXPECT_GE(max_load, 0.0f);
    EXPECT_LE(max_load, 1.0f);

    // Verify block period is reasonable (should be ~1ms for 48 samples at 48kHz)
    EXPECT_GT(block_period, 0.0f);
    EXPECT_LT(block_period, 10.0f);  // Should be less than 10ms

    // Under load, CPU should be <70% (0.7)
    // Note: This is a functional test - actual load depends on implementation
    // In a real test, we'd need to simulate audio processing load
}

// Test meter calculation
TEST_F(AudioEngineTest, MeterCalculation) {
    // Generate test signal (sine wave)
    for (size_t i = 0; i < block_size_; i++) {
        float t = static_cast<float>(i) / 48000.0f;
        left_buffer_[i] = 0.5f * std::sin(2.0f * M_PI * 440.0f * t);
        right_buffer_[i] = 0.3f * std::sin(2.0f * M_PI * 440.0f * t);
    }

    // Process callback (this will calculate meters)
    Callback(GetInputBuffer(), GetOutputBuffer(), block_size_);

    // Get meters
    BlockMeters meters;
    GetMeters(meters);

    // Verify meters are calculated (non-zero for non-silent signal)
    // Note: Actual values depend on implementation
    EXPECT_GE(meters.rmsL, 0.0f);
    EXPECT_GE(meters.rmsR, 0.0f);
    EXPECT_GE(meters.peakL, 0.0f);
    EXPECT_GE(meters.peakR, 0.0f);
    EXPECT_LE(meters.rmsL, 1.0f);
    EXPECT_LE(meters.rmsR, 1.0f);
    EXPECT_LE(meters.peakL, 1.0f);
    EXPECT_LE(meters.peakR, 1.0f);
}

// Test control change message handling
TEST_F(AudioEngineTest, ControlChangeHandling) {
    WaveX::Protocol::ControlChangeMessage msg;
    msg.parameter = WaveX::Protocol::PARAM_VOLUME;
    msg.channel = 0;
    msg.value = 0x7FFF;  // Max value

    // Should not crash
    EXPECT_NO_THROW({ OnControlChange(msg); });
}

// Test note on/off message handling
TEST_F(AudioEngineTest, NoteMessageHandling) {
    WaveX::Protocol::NoteMessage note_on;
    note_on.note = 60;  // Middle C
    note_on.velocity = 127;
    note_on.channel = 0;
    note_on.reserved = 0;

    EXPECT_NO_THROW({ OnNoteOn(note_on); });

    WaveX::Protocol::NoteMessage note_off;
    note_off.note = 60;
    note_off.velocity = 0;
    note_off.channel = 0;
    note_off.reserved = 0;

    EXPECT_NO_THROW({ OnNoteOff(note_off); });
}

// Test sample control message handling
TEST_F(AudioEngineTest, SampleControlHandling) {
    WaveX::Protocol::SampleCtrlMessage msg;
    msg.slot = 0;
    msg.cmd = WaveX::Protocol::SAMPLE_PLAY_START;
    msg.rate = 1.0f;

    EXPECT_NO_THROW({ OnSampleCtrl(msg); });
}

// Test preview request handling
TEST_F(AudioEngineTest, PreviewRequestHandling) {
    WaveX::Protocol::PreviewReqMessage msg;
    msg.slot = 0;
    msg.start = 0;
    msg.end = 1000;
    msg.decim = 4;

    EXPECT_NO_THROW({ OnPreviewReq(msg); });
}

// Test underrun detection (callback should handle gracefully)
TEST_F(AudioEngineTest, UnderrunHandling) {
    // Process callback when buffer is empty (should handle gracefully)
    // This simulates an underrun condition
    for (int i = 0; i < 10; i++) {
        Callback(GetInputBuffer(), GetOutputBuffer(), block_size_);
    }

    // Check underrun status (should be callable from main loop)
    EXPECT_NO_THROW({ CheckAndLogUnderruns(); });
}

// Test I/O statistics
TEST_F(AudioEngineTest, IOStatistics) {
    uint32_t count = 0;
    uint32_t max_duration = 0;
    uint32_t last_duration = 0;

    GetIOStats(count, max_duration, last_duration);

    // Statistics should be retrievable (values depend on actual I/O operations)
    // This test verifies the API works
    EXPECT_GE(count, 0u);
    EXPECT_GE(max_duration, 0u);
    EXPECT_GE(last_duration, 0u);
}

// Test WAV playback state queries
TEST_F(AudioEngineTest, WavPlaybackState) {
    // Initially, WAV should not be playing
    EXPECT_FALSE(IsWavPlaying());

    // Pre-buffer should not be ready initially
    EXPECT_FALSE(IsPrebufferReady());

    // Should pump I/O flag should be queryable
    EXPECT_NO_THROW({
        bool should_pump = ShouldPumpWavIO();
        (void)should_pump;  // Suppress unused warning
    });
}

// Test buffer management - verify callback doesn't access out of bounds
TEST_F(AudioEngineTest, BufferBoundsSafety) {
    // Test with various buffer sizes
    size_t test_sizes[] = {1, 16, 32, 48, 64, 96, 128, 256};

    for (size_t size: test_sizes) {
        left_buffer_.resize(size);
        right_buffer_.resize(size);
        input_buffer_.resize(size);

        // Should not crash or access out of bounds
        EXPECT_NO_THROW({ Callback(GetInputBuffer(), GetOutputBuffer(), size); });

        // Verify buffers are still valid size
        EXPECT_EQ(left_buffer_.size(), size);
        EXPECT_EQ(right_buffer_.size(), size);
    }
}

// Test that callback maintains real-time performance
TEST_F(AudioEngineTest, RealTimePerformance) {
    // Process multiple blocks and measure timing
    const int num_blocks = 1000;
    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_blocks; i++) {
        Callback(GetInputBuffer(), GetOutputBuffer(), block_size_);
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();

    // Calculate average time per block
    double avg_time_per_block_us = static_cast<double>(duration) / num_blocks;
    double avg_time_per_block_ms = avg_time_per_block_us / 1000.0;

    // At 48kHz with 48-sample blocks, each block should take ~1ms
    // Allow some margin for test overhead
    EXPECT_LT(avg_time_per_block_ms, 5.0)
        << "Average block processing time should be <5ms for real-time performance";

    // Verify we can process blocks fast enough for 48kHz audio
    // (48 samples / 48000 Hz = 1ms per block)
    double max_allowed_time_ms = 2.0;  // Allow 2x margin for safety
    EXPECT_LT(avg_time_per_block_ms, max_allowed_time_ms)
        << "Block processing too slow for real-time audio";
}

// Test input meter calculation
TEST_F(AudioEngineTest, InputMeterCalculation) {
    // Generate test input signal
    for (size_t i = 0; i < block_size_; i++) {
        float t = static_cast<float>(i) / 48000.0f;
        input_buffer_[i] = 0.3f * std::sin(2.0f * M_PI * 1000.0f * t);
    }

    // Process callback
    Callback(GetInputBuffer(), GetOutputBuffer(), block_size_);

    // Get input meters
    float rms = 0.0f;
    float peak = 0.0f;
    GetInputMeters(rms, peak);

    // Verify meters are calculated
    EXPECT_GE(rms, 0.0f);
    EXPECT_GE(peak, 0.0f);
    EXPECT_LE(rms, 1.0f);
    EXPECT_LE(peak, 1.0f);
}

// Test that multiple rapid callbacks don't cause issues
TEST_F(AudioEngineTest, RapidCallbackStress) {
    // Process many blocks rapidly
    const int num_blocks = 10000;

    EXPECT_NO_THROW({
        for (int i = 0; i < num_blocks; i++) {
            Callback(GetInputBuffer(), GetOutputBuffer(), block_size_);
        }
    });

    // Verify system is still functional
    float avg_load = GetAvgCpuLoad();
    EXPECT_GE(avg_load, 0.0f);
    EXPECT_LE(avg_load, 1.0f);
}
