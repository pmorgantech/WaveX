#include "config/logging_config.h"

#include <gtest/gtest.h>

#include <cstring>

using namespace WaveX::Log;

namespace {

// The runtime table is process-global inline state, so every test starts by
// restoring the compiled-in defaults rather than trusting test order.
class LoggingConfigTest : public ::testing::Test {
   protected:
    void SetUp() override {
        size_t i = 0;
#define WAVEX_LOG_X(name, deflvl) g_module_levels[i++] = deflvl;
        WAVEX_LOG_MODULE_LIST(WAVEX_LOG_X)
#undef WAVEX_LOG_X
    }
};

TEST_F(LoggingConfigTest, DefaultsComeFromTheModuleTable) {
    EXPECT_EQ(GetLevel(Module::SYSTEM), WAVEX_LOG_LEVEL_INFO);
    EXPECT_EQ(GetLevel(Module::INTER_MCU_LINK), WAVEX_LOG_LEVEL_INFO);
}

TEST_F(LoggingConfigTest, SetLevelClampsAboveTrace) {
    SetLevel(Module::SYSTEM, 250);
    EXPECT_EQ(GetLevel(Module::SYSTEM), WAVEX_LOG_LEVEL_TRACE);
}

TEST_F(LoggingConfigTest, SetAllLevelsHitsEveryModule) {
    SetAllLevels(WAVEX_LOG_LEVEL_ERROR);
    for (size_t i = 0; i < kModuleCount; ++i) {
        EXPECT_EQ(GetLevel(static_cast<Module>(i)), WAVEX_LOG_LEVEL_ERROR) << kModuleNames[i];
    }
}

TEST_F(LoggingConfigTest, SetLevelByNameIsCaseInsensitive) {
    EXPECT_TRUE(SetLevelByName("inter_mcu_link", WAVEX_LOG_LEVEL_DEBUG));
    EXPECT_EQ(GetLevel(Module::INTER_MCU_LINK), WAVEX_LOG_LEVEL_DEBUG);
}

TEST_F(LoggingConfigTest, SetLevelByNameRejectsUnknownAndPrefixes) {
    EXPECT_FALSE(SetLevelByName("NOPE", WAVEX_LOG_LEVEL_DEBUG));
    // A prefix of a real module must not match it.
    EXPECT_FALSE(SetLevelByName("INTER_MCU", WAVEX_LOG_LEVEL_DEBUG));
    EXPECT_FALSE(SetLevelByName("", WAVEX_LOG_LEVEL_DEBUG));
    EXPECT_FALSE(SetLevelByName(nullptr, WAVEX_LOG_LEVEL_DEBUG));
}

TEST_F(LoggingConfigTest, ParseLevelTokenAcceptsNamesAndDigits) {
    EXPECT_EQ(ParseLevelToken("OFF"), WAVEX_LOG_LEVEL_OFF);
    EXPECT_EQ(ParseLevelToken("error"), WAVEX_LOG_LEVEL_ERROR);
    EXPECT_EQ(ParseLevelToken("Trace"), WAVEX_LOG_LEVEL_TRACE);
    EXPECT_EQ(ParseLevelToken("0"), 0);
    EXPECT_EQ(ParseLevelToken("5"), 5);
}

TEST_F(LoggingConfigTest, ParseLevelTokenRejectsJunk) {
    EXPECT_EQ(ParseLevelToken("6"), -1);
    EXPECT_EQ(ParseLevelToken("55"), -1);
    EXPECT_EQ(ParseLevelToken("LOUD"), -1);
    EXPECT_EQ(ParseLevelToken(""), -1);
    EXPECT_EQ(ParseLevelToken(nullptr), -1);
}

TEST_F(LoggingConfigTest, ApplyLevelCommandSetsOneModule) {
    char reply[96];
    EXPECT_TRUE(ApplyLevelCommand("AUDIO_ENGINE DEBUG", reply, sizeof(reply)));
    EXPECT_EQ(GetLevel(Module::AUDIO_ENGINE), WAVEX_LOG_LEVEL_DEBUG);
    EXPECT_STREQ(reply, "WAVEX-LOG: AUDIO_ENGINE=DEBUG");
    // Other modules untouched.
    EXPECT_EQ(GetLevel(Module::SYSTEM), WAVEX_LOG_LEVEL_INFO);
}

TEST_F(LoggingConfigTest, ApplyLevelCommandWildcardSetsAll) {
    char reply[96];
    EXPECT_TRUE(ApplyLevelCommand("* WARN", reply, sizeof(reply)));
    for (size_t i = 0; i < kModuleCount; ++i) {
        EXPECT_EQ(GetLevel(static_cast<Module>(i)), WAVEX_LOG_LEVEL_WARN) << kModuleNames[i];
    }
    EXPECT_STREQ(reply, "WAVEX-LOG: *=WARN");
}

TEST_F(LoggingConfigTest, ApplyLevelCommandToleratesExtraSpacesAndDigitLevels) {
    EXPECT_TRUE(ApplyLevelCommand("  storage   4  ", nullptr, 0));
    EXPECT_EQ(GetLevel(Module::STORAGE), WAVEX_LOG_LEVEL_DEBUG);
}

TEST_F(LoggingConfigTest, ApplyLevelCommandRejectsBadInputWithoutSideEffects) {
    char reply[128];
    EXPECT_FALSE(ApplyLevelCommand("NOPE DEBUG", reply, sizeof(reply)));
    EXPECT_FALSE(ApplyLevelCommand("SYSTEM LOUD", reply, sizeof(reply)));
    EXPECT_FALSE(ApplyLevelCommand("SYSTEM", reply, sizeof(reply)));
    EXPECT_FALSE(ApplyLevelCommand("", reply, sizeof(reply)));
    EXPECT_FALSE(ApplyLevelCommand(nullptr, reply, sizeof(reply)));
    EXPECT_NE(nullptr, strstr(reply, "usage:"));
    for (size_t i = 0; i < kModuleCount; ++i) {
        EXPECT_EQ(GetLevel(static_cast<Module>(i)), WAVEX_LOG_LEVEL_INFO) << kModuleNames[i];
    }
}

// The gate the WAVEX_LOGx macros compile down to: ceiling is a constant
// fold, runtime level is the byte compared here. Exercise the comparison
// semantics the macros rely on.
TEST_F(LoggingConfigTest, LevelComparisonSemantics) {
    SetLevel(Module::CV, WAVEX_LOG_LEVEL_WARN);
    EXPECT_LE(WAVEX_LOG_LEVEL_ERROR, GetLevel(Module::CV));  // errors pass
    EXPECT_LE(WAVEX_LOG_LEVEL_WARN, GetLevel(Module::CV));   // warns pass
    EXPECT_GT(WAVEX_LOG_LEVEL_INFO, GetLevel(Module::CV));   // info gated
    SetLevel(Module::CV, WAVEX_LOG_LEVEL_OFF);
    EXPECT_GT(WAVEX_LOG_LEVEL_ERROR, GetLevel(Module::CV));  // even errors gated at OFF
}

}  // namespace
