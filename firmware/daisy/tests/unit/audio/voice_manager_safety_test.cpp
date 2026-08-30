#include <gtest/gtest.h>
#include <sys/mman.h>
#include <unistd.h>

#include "audio/voice_manager.hpp"
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

using WaveX::AudioEngine::VoiceManager;
using WaveX::AudioEngine::VoiceTriggerParams;

// Memory-safety properties for VoiceManager::Render().
//
// Why this file exists separately from voice_manager_test.cpp: the August 2026
// audit found two out-of-bounds reads here (228316d release-tail idx1, and
// f09a423 reading past a trimmed end_frame) that the value-asserting tests ran
// straight over. Both were VALUE-INVISIBLE - the read one frame past the
// region returns a plausible sample, interpolates to a plausible float, and
// every EXPECT_NEAR still passed. The suite executed the defect on every run
// for weeks without observing it.
//
// The oracle here is memory protection rather than a value. The sample is
// placed flush against the end of a writable page, with the next page mapped
// PROT_NONE, so a read of even one frame past the end is an immediate SIGSEGV
// rather than a plausible number. That is deterministic, where ASan depends on
// the allocator placing a redzone where the overread lands - so this is worth
// having in addition to `make test-asan`, not instead of it.

namespace {

// A sample buffer whose last frame ends exactly at a PROT_NONE boundary.
class GuardedSample {
   public:
    explicit GuardedSample(uint32_t frames, uint32_t channels = 1) : frames_(frames) {
        const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
        const size_t values = static_cast<size_t>(frames) * channels;
        const size_t bytes = values * sizeof(int16_t);
        // Two pages: the first writable and holding the sample's tail, the
        // second unmapped. Round the data region up so `bytes` always fits.
        pages_ = ((bytes + page - 1) / page) + 1;
        span_ = pages_ * page;

        base_ = static_cast<char*>(
            mmap(nullptr, span_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (base_ == MAP_FAILED) {
            base_ = nullptr;
            return;
        }
        // Poison the final page.
        if (mprotect(base_ + span_ - page, page, PROT_NONE) != 0) {
            munmap(base_, span_);
            base_ = nullptr;
            return;
        }
        // Place the sample so its last value abuts the poisoned page.
        data_ = reinterpret_cast<int16_t*>(base_ + span_ - page - bytes);
        for (size_t i = 0; i < values; ++i) {
            data_[i] = static_cast<int16_t>((i % 512) * 60 - 15000);
        }
    }

    ~GuardedSample() {
        if (base_) {
            munmap(base_, span_);
        }
    }

    GuardedSample(const GuardedSample&) = delete;
    GuardedSample& operator=(const GuardedSample&) = delete;

    bool valid() const { return base_ != nullptr; }
    const int16_t* data() const { return data_; }
    uint32_t frames() const { return frames_; }

   private:
    char* base_ = nullptr;
    int16_t* data_ = nullptr;
    size_t span_ = 0;
    size_t pages_ = 0;
    uint32_t frames_ = 0;
};

}  // namespace

// The specific shape of 228316d: a one-shot voice whose release tail holds at
// the final frame. Pre-fix, `idx1 = idx0 + 1` stepped one frame past the
// region - and when end_frame == sample_frames that is one frame past the
// allocation itself.
TEST(VoiceManagerSafetyTest, ReleaseTailNeverReadsPastTheFinalFrame) {
    GuardedSample sample(64);
    ASSERT_TRUE(sample.valid()) << "mmap/mprotect unavailable";

    VoiceManager vm;
    vm.Init(48000);

    VoiceTriggerParams p;
    p.sample = sample.data();
    p.sample_frames = sample.frames();
    p.note = 60;
    p.root_note = 60;
    p.velocity = 127;
    p.pan = 0.5f;
    p.filter_cutoff_hz = 1.0e6f;
    p.attack_s = 0.0f;
    p.decay_s = 0.0f;
    p.sustain_level = 1.0f;
    p.release_s = 0.2f;  // ~9600 frames parked at the end of the region
    vm.Trigger(p);

    std::vector<float> l(512), r(512);
    vm.Render(l.data(), r.data(), l.size());  // SIGSEGV pre-228316d
}

// The property that generalises the whole class: for ANY combination of
// trigger parameters, Render() must not read outside the sample it was given.
// This subsumes 228316d, f09a423, the loop seam, and the idx0 clamp - and it
// keeps holding for trigger parameters nobody has thought of yet, which is the
// part a case test cannot do.
TEST(VoiceManagerSafetyTest, RenderStaysInsideTheSampleForAnyTriggerParameters) {
    constexpr uint32_t kFrames = 128;
    std::mt19937 rng(20260830);  // fixed seed: reproducible, not a fuzzer

    for (int iter = 0; iter < 2000; ++iter) {
        const uint32_t channels = (rng() % 2) + 1;
        GuardedSample sample(kFrames, channels);
        ASSERT_TRUE(sample.valid()) << "mmap/mprotect unavailable";

        VoiceManager vm;
        vm.Init(48000);

        VoiceTriggerParams p;
        p.sample = sample.data();
        p.sample_frames = kFrames;
        p.channels = static_cast<uint8_t>(channels);
        p.note = static_cast<uint8_t>(24 + (rng() % 72));
        p.root_note = static_cast<uint8_t>(24 + (rng() % 72));
        p.velocity = static_cast<uint8_t>(1 + (rng() % 127));
        p.pan = static_cast<float>(rng() % 101) / 100.0f;
        p.filter_cutoff_hz = 1.0e6f;
        p.attack_s = 0.0f;
        p.decay_s = 0.0f;
        p.sustain_level = 1.0f;
        p.release_s = (rng() % 4) ? 0.05f : 0.0f;

        // Region and loop bounds, deliberately including degenerate and
        // out-of-order combinations - rejecting those is the code's job, and
        // rejecting them without an overread is what is under test.
        p.start_frame = static_cast<uint32_t>(rng() % (kFrames + 8));
        p.end_frame = static_cast<uint32_t>(rng() % (kFrames + 8));
        p.loop = (rng() % 2) == 0;
        p.loop_start = static_cast<uint32_t>(rng() % (kFrames + 8));
        p.loop_end = static_cast<uint32_t>(rng() % (kFrames + 8));
        p.pitch_ratio_mul = 0.25f + static_cast<float>(rng() % 800) / 100.0f;

        vm.Trigger(p);

        std::vector<float> l(96), r(96);
        for (int block = 0; block < 4; ++block) {
            vm.Render(l.data(), r.data(), l.size());
        }
    }
}
