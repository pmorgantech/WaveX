// audio_engine_test.cpp - NOT BUILT ON THE HOST.
//
// STATUS (read before "fixing" this file): tests/CMakeLists.txt filters this
// file out of the host build (`list(FILTER ... EXCLUDE ... audio_engine_test)`)
// because audio_engine.cpp drags in the STM32 HAL, SDRAM placement macros,
// FatFS streaming and the SAI/DMA callback plumbing - none of which the
// mocks in tests/mocks/ model. Nothing here compiles or runs in CI. (Before
// this rewrite the file did not even compile: one test referenced variables
// of an API that had been removed, and nobody could notice because the file
// is never built.)
//
// What ACTUALLY covers the engine's logic on the host: every piece of
// audio_engine.cpp that could be extracted HAL-free has been, and each has a
// real suite in this directory - voice_manager_test, instrument_test,
// linear_resampler_test, svf_filter_test, paraphonic_envelope_test,
// fade_test, output_sink_test - plus message_dispatch_test for the wire ->
// engine routing. That extraction is the intended pattern for anything else
// that needs coverage here: pull the logic out of the callback into a
// HAL-free header, test that.
//
// The checklist below records what an audio_engine-level host suite would
// still add on top (callback-context wiring, not DSP): keep it short and do
// not add "EXPECT_GE(x, 0u)"-style assertions back - a test that cannot
// fail is worse than no test.

#include <gtest/gtest.h>

class AudioEngineTest : public ::testing::Test {};

// Needs a DaisySeed/AudioHandle mock deep enough to run Init()+Callback():
// silence before any WAV/note, exact block clearing at every block size.
TEST_F(AudioEngineTest, DISABLED_CallbackProducesSilenceWhenIdle) {}

// Note queue drain: OnNoteOn from the "main loop" must be audible in the
// next Callback() block and OnNoteOff must release it (SPSC queue wiring,
// not VoiceManager logic - that part is already covered).
TEST_F(AudioEngineTest, DISABLED_NoteEventsCrossTheSpscQueueIntoTheCallback) {}

// Meters must reflect the block actually rendered (RMS/peak of a known
// test signal to exact values, not just "in [0,1]").
TEST_F(AudioEngineTest, DISABLED_MetersMatchRenderedBlockExactly) {}
