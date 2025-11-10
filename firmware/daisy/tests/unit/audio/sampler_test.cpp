#include "../src/sampler.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

class SamplerTest : public ::testing::Test {
   protected:
    void SetUp() override {
        sampler_.Init(48000);  // 48kHz sample rate
    }

    void TearDown() override {}

    Sampler sampler_;
};

// Test sampler initialization
TEST_F(SamplerTest, Initialization) {
    Sampler s;
    s.Init(44100);

    // After initialization, sampler should be stopped
    sampler_.StopPlay();
    sampler_.StopRec();
}

// Test recording functionality
TEST_F(SamplerTest, Recording) {
    // Start recording
    sampler_.StartRec();

    // Feed input block
    std::vector<float> input_block(48, 0.5f);  // 48 samples at 0.5 amplitude
    sampler_.FeedInputBlock(input_block.data(), input_block.size());

    // Stop recording
    sampler_.StopRec();

    // Verify recording stopped
    // (Can't directly check internal state, but StopRec should not crash)
    EXPECT_NO_THROW({ sampler_.StopRec(); });
}

// Test playback functionality
TEST_F(SamplerTest, Playback) {
    // Record some samples first
    sampler_.StartRec();
    std::vector<float> input_block(100, 0.3f);
    sampler_.FeedInputBlock(input_block.data(), input_block.size());
    sampler_.StopRec();

    // Start playback at normal rate
    sampler_.StartPlay(1.0f);

    // Generate samples
    float sample = sampler_.Next();

    // Sample should be in valid range [-1.0, 1.0]
    EXPECT_GE(sample, -1.0f);
    EXPECT_LE(sample, 1.0f);

    // Stop playback
    sampler_.StopPlay();

    // After stopping, Next() should return 0.0
    EXPECT_FLOAT_EQ(sampler_.Next(), 0.0f);
}

// Test playback rate variation
TEST_F(SamplerTest, PlaybackRate) {
    // Record samples
    sampler_.StartRec();
    std::vector<float> input_block(100, 0.5f);
    sampler_.FeedInputBlock(input_block.data(), input_block.size());
    sampler_.StopRec();

    // Test different playback rates
    float rates[] = {0.5f, 1.0f, 1.5f, 2.0f};

    for (float rate: rates) {
        sampler_.StartPlay(rate);

        // Generate a few samples
        for (int i = 0; i < 10; i++) {
            float sample = sampler_.Next();
            EXPECT_GE(sample, -1.0f);
            EXPECT_LE(sample, 1.0f);
        }

        sampler_.StopPlay();
    }
}

// Test empty buffer playback
TEST_F(SamplerTest, EmptyBufferPlayback) {
    // Start playback without recording anything
    sampler_.StartPlay(1.0f);

    // Next() should return 0.0 for empty buffer
    EXPECT_FLOAT_EQ(sampler_.Next(), 0.0f);
    EXPECT_FLOAT_EQ(sampler_.Next(), 0.0f);

    sampler_.StopPlay();
}

// Test preview generation
TEST_F(SamplerTest, PreviewGeneration) {
    // Record samples
    sampler_.StartRec();
    std::vector<float> input_block(1000, 0.4f);
    sampler_.FeedInputBlock(input_block.data(), input_block.size());
    sampler_.StopRec();

    // Generate preview with decimation
    std::vector<int16_t> preview;
    sampler_.MakePreview(0, 1000, 4, preview);

    // Preview should have approximately 250 samples (1000 / 4)
    EXPECT_GE(preview.size(), 200u);
    EXPECT_LE(preview.size(), 300u);

    // All preview samples should be valid int16_t values
    for (int16_t sample: preview) {
        EXPECT_GE(sample, -32768);
        EXPECT_LE(sample, 32767);
    }
}

// Test preview with different decimation factors
TEST_F(SamplerTest, PreviewDecimation) {
    // Record samples
    sampler_.StartRec();
    std::vector<float> input_block(1000, 0.3f);
    sampler_.FeedInputBlock(input_block.data(), input_block.size());
    sampler_.StopRec();

    // Test different decimation factors
    size_t decimations[] = {1, 2, 4, 8, 16};

    for (size_t decim: decimations) {
        std::vector<int16_t> preview;
        sampler_.MakePreview(0, 1000, decim, preview);

        // Preview size should be approximately original_size / decim
        size_t expected_size = (1000 / decim) + 1;      // +1 for rounding
        EXPECT_GE(preview.size(), expected_size - 10);  // Allow some margin
        EXPECT_LE(preview.size(), expected_size + 10);
    }
}

// Test preview boundary conditions
TEST_F(SamplerTest, PreviewBoundaries) {
    // Record samples
    sampler_.StartRec();
    std::vector<float> input_block(500, 0.2f);
    sampler_.FeedInputBlock(input_block.data(), input_block.size());
    sampler_.StopRec();

    // Test with start > end (should handle gracefully)
    std::vector<int16_t> preview;
    sampler_.MakePreview(600, 500, 1, preview);
    EXPECT_EQ(preview.size(), 0u);

    // Test with end > buffer size
    sampler_.MakePreview(0, 1000, 1, preview);
    EXPECT_LE(preview.size(), 500u);  // Should be limited to actual buffer size

    // Test with start at end
    sampler_.MakePreview(500, 500, 1, preview);
    EXPECT_EQ(preview.size(), 0u);
}

// Test preview with zero decimation (should default to 1)
TEST_F(SamplerTest, PreviewZeroDecimation) {
    // Record samples
    sampler_.StartRec();
    std::vector<float> input_block(100, 0.1f);
    sampler_.FeedInputBlock(input_block.data(), input_block.size());
    sampler_.StopRec();

    std::vector<int16_t> preview;
    sampler_.MakePreview(0, 100, 0, preview);

    // Should treat decim=0 as decim=1
    EXPECT_GE(preview.size(), 90u);
    EXPECT_LE(preview.size(), 110u);
}

// Test input clipping during recording
TEST_F(SamplerTest, InputClipping) {
    sampler_.StartRec();

    // Feed samples that exceed [-1.0, 1.0] range
    std::vector<float> input_block(10);
    for (size_t i = 0; i < 10; i++) {
        input_block[i] = (i % 2 == 0) ? 2.0f : -2.0f;  // Values outside valid range
    }

    sampler_.FeedInputBlock(input_block.data(), input_block.size());
    sampler_.StopRec();

    // Start playback and verify samples are clipped
    sampler_.StartPlay(1.0f);

    for (int i = 0; i < 10; i++) {
        float sample = sampler_.Next();
        // Samples should be clipped to approximately [-1.0, 1.0]
        // Allow small floating-point precision errors
        EXPECT_GE(sample, -1.01f);
        EXPECT_LE(sample, 1.01f);
    }

    sampler_.StopPlay();
}

// Test BlockMeters static function
TEST_F(SamplerTest, BlockMeters) {
    // Create test signal
    std::vector<float> signal(48);
    for (size_t i = 0; i < 48; i++) {
        float t = static_cast<float>(i) / 48000.0f;
        signal[i] = 0.5f * std::sin(2.0f * M_PI * 440.0f * t);
    }

    float rms = 0.0f;
    float peak = 0.0f;

    Sampler::BlockMeters(signal.data(), signal.size(), rms, peak);

    // RMS should be positive for non-zero signal
    EXPECT_GT(rms, 0.0f);
    EXPECT_LE(rms, 1.0f);

    // Peak should be positive and >= RMS
    EXPECT_GT(peak, 0.0f);
    EXPECT_LE(peak, 1.0f);
    EXPECT_GE(peak, rms);
}

// Test BlockMeters with silence
TEST_F(SamplerTest, BlockMetersSilence) {
    std::vector<float> silence(48, 0.0f);

    float rms = 1.0f;  // Initialize to non-zero
    float peak = 1.0f;

    Sampler::BlockMeters(silence.data(), silence.size(), rms, peak);

    // RMS and peak should be 0.0 for silence
    EXPECT_FLOAT_EQ(rms, 0.0f);
    EXPECT_FLOAT_EQ(peak, 0.0f);
}

// Test BlockMeters with empty buffer
TEST_F(SamplerTest, BlockMetersEmpty) {
    float rms = 1.0f;
    float peak = 1.0f;

    Sampler::BlockMeters(nullptr, 0, rms, peak);

    // Should handle empty buffer gracefully
    EXPECT_FLOAT_EQ(rms, 0.0f);
    EXPECT_FLOAT_EQ(peak, 0.0f);
}

// Test playback completion
TEST_F(SamplerTest, PlaybackCompletion) {
    // Record a small number of samples
    sampler_.StartRec();
    std::vector<float> input_block(10, 0.5f);
    sampler_.FeedInputBlock(input_block.data(), input_block.size());
    sampler_.StopRec();

    // Start playback
    sampler_.StartPlay(1.0f);

    // Play through all samples
    int samples_played = 0;
    for (int i = 0; i < 20; i++) {  // More iterations than samples
        float sample = sampler_.Next();
        if (sample != 0.0f) {
            samples_played++;
        }
    }

    // Should have played approximately 10 samples
    EXPECT_GE(samples_played, 8);
    EXPECT_LE(samples_played, 12);

    // After completion, Next() should return 0.0
    EXPECT_FLOAT_EQ(sampler_.Next(), 0.0f);
}

// Test rapid start/stop cycles
TEST_F(SamplerTest, RapidStartStop) {
    // Record samples
    sampler_.StartRec();
    std::vector<float> input_block(100, 0.3f);
    sampler_.FeedInputBlock(input_block.data(), input_block.size());
    sampler_.StopRec();

    // Rapidly start and stop playback
    for (int i = 0; i < 10; i++) {
        sampler_.StartPlay(1.0f);
        float sample = sampler_.Next();
        EXPECT_GE(sample, -1.0f);
        EXPECT_LE(sample, 1.0f);
        sampler_.StopPlay();
        EXPECT_FLOAT_EQ(sampler_.Next(), 0.0f);
    }
}

// Test recording while playing (should be independent)
TEST_F(SamplerTest, RecordWhilePlaying) {
    // Record initial samples
    sampler_.StartRec();
    std::vector<float> input_block1(50, 0.2f);
    sampler_.FeedInputBlock(input_block1.data(), input_block1.size());
    sampler_.StopRec();

    // Start playback
    sampler_.StartPlay(1.0f);

    // Start recording again (should be independent)
    sampler_.StartRec();
    std::vector<float> input_block2(50, 0.4f);
    sampler_.FeedInputBlock(input_block2.data(), input_block2.size());

    // Playback should continue
    float sample = sampler_.Next();
    EXPECT_GE(sample, -1.0f);
    EXPECT_LE(sample, 1.0f);

    sampler_.StopRec();
    sampler_.StopPlay();
}
