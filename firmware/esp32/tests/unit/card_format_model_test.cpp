#include "ui/card_format_model.h"

#include <gtest/gtest.h>
using namespace WaveX::Protocol;
TEST(CardFormatModel, RequiresExplicitPrepareAndMatchingReplyBeforeConfirm) {
    wavex_ui::CardFormatModel model;
    EXPECT_FALSE(IsValidCardOp(model.Request(CARD_CONFIRM_FORMAT, 1)));
    model.Request(CARD_GET, 2);
    EXPECT_TRUE(model.Accept({2, 99, 0, CARD_CONFIRMATION, CARD_OK, 1, 0}));
    EXPECT_FALSE(model.CanConfirm());
    model.Request(CARD_PREPARE_FORMAT, 3);
    EXPECT_FALSE(model.Accept({2, 3, 0, CARD_CONFIRMATION, CARD_OK, 1, 0}));
    EXPECT_TRUE(model.Accept({3, 3, 0, CARD_CONFIRMATION, CARD_OK, 1, 0}));
    EXPECT_TRUE(model.CanConfirm());
    const auto request = model.Request(CARD_CONFIRM_FORMAT, 4);
    EXPECT_EQ(request.token, 3u);
    EXPECT_FALSE(model.CanConfirm());
    EXPECT_FALSE(IsValidCardOp(model.Request(CARD_CONFIRM_FORMAT, 5)));
}
TEST(CardFormatModel, CancelStaleCacheAndRebootCannotAuthorizeOrReportSuccess) {
    wavex_ui::CardFormatModel model;
    model.Request(CARD_PREPARE_FORMAT, 1);
    CardStateMessage state{1, 1, 0, CARD_CONFIRMATION, CARD_OK, 1, 0};
    ASSERT_TRUE(model.Accept(state));
    EXPECT_FALSE(model.Accept(state));  // cached state cannot refresh link liveness
    model.Request(CARD_CANCEL, 2);
    EXPECT_FALSE(model.CanConfirm());
    EXPECT_FALSE(model.Accept(state));
    model.Request(CARD_PREPARE_FORMAT, 3);
    ASSERT_TRUE(model.Accept({3, 3, 0, CARD_CONFIRMATION, CARD_OK, 1, 0}));
    model.Request(CARD_CONFIRM_FORMAT, 4);
    model.Request(CARD_GET, 5);
    ASSERT_TRUE(model.Accept({5, 0, 0, CARD_IDLE, CARD_OK, 1, 0}));
    EXPECT_TRUE(model.UnknownResult());
    EXPECT_FALSE(model.CanConfirm());
}
