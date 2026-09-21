#include "audio/global_lfo.hpp"

#include <gtest/gtest.h>
using namespace WaveX::AudioEngine;
using namespace WaveX::Protocol;
TEST(GlobalLfo, RevisionAndDuplicateResetProtectSessionState) {
    GlobalLfo lfo;
    lfo.Init();
    GlobalLfoOpMessage m;
    m.request_id = 1;
    m.revision = 1;
    m.op = GLOBAL_LFO_SET;
    m.value.wave = 2;
    m.value.rate_hz = 2;
    lfo.Request(m);
    EXPECT_EQ(lfo.State().revision, 2u);
    EXPECT_NEAR(lfo.Tick(120, false, 0, false), -.996f, .0001f);
    m.request_id = 2;
    m.op = GLOBAL_LFO_RESET;
    lfo.Request(m);
    EXPECT_EQ(lfo.State().error, 1);  // stale cannot reset phase
    lfo.Tick(120, false, 0, false);
    EXPECT_NEAR(lfo.Phase(), .004f, .0001f);
    m.request_id = 3;
    m.revision = 2;
    lfo.Request(m);
    lfo.Tick(120, false, 0, false);
    EXPECT_NEAR(lfo.Phase(), .002f, .0001f);
    lfo.Request(m);
    lfo.Tick(120, false, 0, false);
    EXPECT_NEAR(lfo.Phase(), .004f, .0001f);
}
TEST(GlobalLfo, SyncedRateAndRestartModesUseTransportAndNotes) {
    GlobalLfo lfo;
    lfo.Init();
    GlobalLfoOpMessage m;
    m.request_id = 1;
    m.revision = 1;
    m.op = GLOBAL_LFO_SET;
    m.value.sync_div = 3;
    m.value.restart = 1;
    lfo.Request(m);
    for (int i = 0; i < 100; ++i)
        lfo.Tick(120, false, 0, false);
    EXPECT_NEAR(lfo.Phase(), .2f, .0001f);
    lfo.Tick(120, true, 1, false);
    EXPECT_NEAR(lfo.Phase(), .002f, .0001f);
    lfo.Tick(120, true, 1, true);
    EXPECT_NEAR(lfo.Phase(), .004f, .0001f);
    m.request_id = 2;
    m.revision = 2;
    m.value.restart = 2;
    lfo.Request(m);
    lfo.Tick(60, true, 1, true);
    EXPECT_NEAR(lfo.Phase(), .001f, .0001f);
}
