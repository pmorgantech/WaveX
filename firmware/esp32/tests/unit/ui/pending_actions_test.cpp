#include "ui/pending_actions.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using wavex_ui::PendingActions;

TEST(PendingActions, NavigationCancelsActionsBeforePageDestruction) {
    PendingActions pending;
    auto page = std::make_unique<int>(0);
    int calls = 0;
    ASSERT_TRUE(pending.push([&] {
        pending.clear();  // navigator/tab exits the owner before deleting it
        page.reset();
        ++calls;
    }));
    ASSERT_TRUE(pending.push([owner = page.get()] { ++*owner; }));
    auto action = pending.pop();
    ASSERT_TRUE(action);
    action();
    EXPECT_FALSE(page);
    EXPECT_FALSE(pending.pop());
    EXPECT_EQ(calls, 1);
}

TEST(PendingActions, CancelledActionsReleaseCapturesAndDoNotSurviveReentry) {
    PendingActions pending;
    auto owner = std::make_shared<int>(0);
    std::weak_ptr<int> lifetime = owner;
    ASSERT_TRUE(pending.push([owner] { ++*owner; }));
    owner.reset();
    EXPECT_FALSE(lifetime.expired());
    pending.clear();
    EXPECT_TRUE(lifetime.expired());
    int next_page_calls = 0;
    ASSERT_TRUE(pending.push([&] { ++next_page_calls; }));
    pending.pop()();
    EXPECT_EQ(next_page_calls, 1);
    EXPECT_FALSE(pending.pop());
}

TEST(PendingActions, BoundedQueuePreservesOrderAcrossWraparound) {
    PendingActions pending;
    std::vector<size_t> calls;
    for (size_t cycle = 0; cycle < 3; ++cycle) {
        for (size_t i = 0; i < PendingActions::kCapacity; ++i) {
            ASSERT_TRUE(pending.push([&, i] { calls.push_back(i); }));
        }
        EXPECT_FALSE(pending.push([] {}));
        for (size_t i = 0; i < PendingActions::kCapacity; ++i) {
            auto action = pending.pop();
            ASSERT_TRUE(action);
            action();
            EXPECT_EQ(calls.back(), i);
        }
    }
}
