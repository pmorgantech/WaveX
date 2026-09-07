#include "comm/listener_slot.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

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

TEST_F(ListenerSlotTest, AllocationFailureNeverInvokesAnUnprotectedCallback) {
    mockFailNextRecursiveMutexCreation();
    ListenerSlot<TestCb> slot;
    slot.set(recordingCb, &g_obs);
    EXPECT_FALSE(slot.registered());
    slot.invoke(42);
    slot.set(nullptr, nullptr);
    EXPECT_EQ(g_obs.calls, 0);
}

TEST_F(ListenerSlotTest, PageTeardownWaitsForReceiverBeforeDestroyingOwner) {
    struct Page {
        std::promise<void> entered;
        std::shared_future<void> release;
        int calls = 0;
    };
    ListenerSlot<TestCb> slot;
    std::promise<void> release;
    auto page = std::make_unique<Page>();
    page->release = release.get_future().share();
    auto entered = page->entered.get_future();
    slot.set(
        [](int, void* data) {
            auto& owner = *static_cast<Page*>(data);
            owner.entered.set_value();
            owner.release.wait();
            ++owner.calls;
        },
        page.get());
    std::thread receiver([&] { slot.invoke(1); });
    entered.wait();
    std::promise<void> exiting;
    auto teardown = std::async(std::launch::async, [&] {
        exiting.set_value();
        slot.set(nullptr, nullptr);
        const int calls = page->calls;
        page.reset();
        return calls;
    });
    exiting.get_future().wait();
    EXPECT_EQ(teardown.wait_for(std::chrono::milliseconds(30)), std::future_status::timeout);
    release.set_value();
    receiver.join();
    EXPECT_EQ(teardown.get(), 1);
    slot.invoke(2);
    EXPECT_FALSE(slot.registered());
}

TEST_F(ListenerSlotTest, RegistrationAndObservationStayConsistentDuringTraffic) {
    struct Owner {
        int tag;
        std::atomic<int> calls{0};
    };
    Owner first{1}, second{2};
    std::atomic<int> mismatches{0};
    struct Registration {
        Owner* owner;
        std::atomic<int>* mismatches;
    } a{&first, &mismatches}, b{&second, &mismatches};
    ListenerSlot<TestCb> slot;
    auto first_cb = +[](int, void* data) {
        auto& r = *static_cast<Registration*>(data);
        if (r.owner->tag != 1) {
            ++*r.mismatches;
        }
        ++r.owner->calls;
    };
    auto second_cb = +[](int, void* data) {
        auto& r = *static_cast<Registration*>(data);
        if (r.owner->tag != 2) {
            ++*r.mismatches;
        }
        ++r.owner->calls;
    };
    std::promise<void> started;
    std::atomic<bool> stop{false};
    slot.set(first_cb, &a);
    std::thread receiver([&] {
        slot.invoke(0);
        started.set_value();
        while (!stop.load()) {
            slot.invoke(0);
            (void)slot.registered();
        }
    });
    started.get_future().wait();
    for (int i = 0; i < 2000; ++i) {
        slot.set(second_cb, &b);
        slot.set(nullptr, nullptr);
        slot.set(first_cb, &a);
    }
    slot.set(nullptr, nullptr);
    stop.store(true);
    receiver.join();
    EXPECT_EQ(mismatches.load(), 0);
    EXPECT_GT(first.calls.load() + second.calls.load(), 0);
}

}  // namespace
