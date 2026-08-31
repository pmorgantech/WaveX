#include <gtest/gtest.h>

static_assert(__cplusplus >= 201703L,
              "Daisy first-party code uses C++17 inline variables and must compile as C++17");

TEST(CxxStandardTest, DaisyFirstPartyCodeUsesCxx17OrNewer) {
    EXPECT_GE(__cplusplus, 201703L);
}
