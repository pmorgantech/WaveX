// Tests for the comm listener slot.
//
// The bugs this class exists to prevent - a torn {callback, user_data} pair,
// and a page destroyed while its callback is in flight - are concurrency bugs,
// and the host mocks are single-threaded no-op semaphores. So these tests do
// not prove the mutual exclusion; they pin the observable contract that the
// mutual exclusion is wrapped around, which is where the real defects actually
// showed up: a slot that read user_data separately from the callback, and one
// that kept invoking after it had been cleared.

#include "comm/listener_slot.h"

#include <gtest/gtest.h>

namespace {

using WaveX::Comm::ListenerSlot;

// Mirrors the shape of every WaveX comm callback: payload args first,
// user_data last.
using TestCb = void (*)(int value, void* user_data);

struct Observation {
    int calls = 0;
    int last_value = 0;
    void* last_user_data = nullptr;
};

Observation g_obs;

void recordingCb(int value, void* user_data) {
    g_obs.calls++;
    g_obs.last_value = value;
    g_obs.last_user_data = user_data;
}

void otherCb(int value, void* user_data) {
    (void)value;
    (void)user_data;
    g_obs.calls += 100;
}

class ListenerSlotTest : public ::testing::Test {
   protected:
    void SetUp() override { g_obs = Observation{}; }
};

TEST_F(ListenerSlotTest, UnregisteredSlotIsSafeToInvoke) {
    ListenerSlot<TestCb> slot;
    EXPECT_FALSE(slot.registered());
    slot.invoke(7);  // must not crash or call anything
    EXPECT_EQ(g_obs.calls, 0);
}

TEST_F(ListenerSlotTest, InvokePassesUserDataAsFinalArgument) {
    ListenerSlot<TestCb> slot;
    int owner = 0;
    slot.set(recordingCb, &owner);

    slot.invoke(42);

    EXPECT_EQ(g_obs.calls, 1);
    EXPECT_EQ(g_obs.last_value, 42);
    EXPECT_EQ(g_obs.last_user_data, &owner);
}

// The torn-pair defect in the shape it actually took: re-registering swapped
// the callback and the user_data as two separate writes, so an invocation
// could pair one page's handler with another page's `this`. Whatever a caller
// observes, the two must belong to the same registration.
TEST_F(ListenerSlotTest, ReRegistrationSwapsCallbackAndUserDataTogether) {
    ListenerSlot<TestCb> slot;
    int first_owner = 0;
    int second_owner = 0;

    slot.set(recordingCb, &first_owner);
    slot.set(recordingCb, &second_owner);
    slot.invoke(1);

    EXPECT_EQ(g_obs.calls, 1);
    EXPECT_EQ(g_obs.last_user_data, &second_owner)
        << "invocation used the previous registration's user_data";
}

// The use-after-free defect: a page clears its listener in onExit and is then
// destroyed. After the clear returns, nothing may reach the old callback.
TEST_F(ListenerSlotTest, ClearingStopsFurtherInvocations) {
    ListenerSlot<TestCb> slot;
    int owner = 0;
    slot.set(recordingCb, &owner);
    slot.invoke(1);
    ASSERT_EQ(g_obs.calls, 1);

    slot.set(nullptr, nullptr);
    EXPECT_FALSE(slot.registered());

    slot.invoke(2);
    EXPECT_EQ(g_obs.calls, 1) << "callback ran after the slot was cleared";
}

TEST_F(ListenerSlotTest, LatestRegistrationWins) {
    ListenerSlot<TestCb> slot;
    int owner = 0;
    slot.set(recordingCb, &owner);
    slot.set(otherCb, &owner);

    slot.invoke(1);

    EXPECT_EQ(g_obs.calls, 100) << "the superseded callback was invoked";
}

// Registering from inside a callback is legitimate - a page can swap its own
// handler - and must not deadlock against the mutex the invocation holds.
// That is why the slot's mutex is recursive; with a plain mutex this test
// would hang rather than fail.
TEST_F(ListenerSlotTest, ReRegisteringFromInsideACallbackDoesNotDeadlock) {
    static ListenerSlot<TestCb>* s_slot = nullptr;
    ListenerSlot<TestCb> slot;
    s_slot = &slot;

    slot.set(
        [](int value, void* user_data) {
            (void)value;
            (void)user_data;
            g_obs.calls++;
            s_slot->set(nullptr, nullptr);
        },
        nullptr);

    slot.invoke(1);

    EXPECT_EQ(g_obs.calls, 1);
    EXPECT_FALSE(slot.registered());
}

}  // namespace
