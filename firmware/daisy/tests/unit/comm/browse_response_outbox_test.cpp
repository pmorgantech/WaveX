#include "comm/browse_response_outbox.hpp"

#include <gtest/gtest.h>

#include <cstring>

using WaveX::Comm::BrowseResponseOutbox;

TEST(BrowseResponseOutbox, RetainsCompleteListingUntilTransportAcceptsIt) {
    BrowseResponseOutbox box;
    std::memset(box.Begin(), 0x5a, BrowseResponseOutbox::kCapacity);
    box.Commit(BrowseResponseOutbox::kCapacity);
    unsigned attempts = 0;
    auto send = [&](const uint8_t* payload, uint16_t size) {
        EXPECT_EQ(size, BrowseResponseOutbox::kCapacity);
        for (size_t i = 0; i < size; ++i)
            EXPECT_EQ(payload[i], 0x5a);
        return ++attempts < 3 ? -1 : 0;
    };
    box.Pump(send);
    box.Pump(send);
    box.Pump(send);
    box.Pump(send);
    EXPECT_EQ(attempts, 3u);
}

TEST(BrowseResponseOutbox, NewListingAndStorageLossSupersedeUnsentPage) {
    BrowseResponseOutbox box;
    box.Begin()[0] = 42;
    box.Commit(1);
    box.Pump([](const uint8_t*, uint16_t) { return -1; });
    // Storage loss must not allow the old directory to reappear afterward.
    std::memset(box.Begin(), 0, 5);
    box.Commit(5);
    unsigned sent = 0;
    box.Pump([&](const uint8_t* payload, uint16_t size) {
        EXPECT_EQ(size, 5u);
        for (size_t i = 0; i < size; ++i)
            EXPECT_EQ(payload[i], 0);
        ++sent;
        return 0;
    });
    EXPECT_EQ(sent, 1u);
}

TEST(BrowseResponseOutbox, FailedReplacementCannotSendOldOrPartialListing) {
    BrowseResponseOutbox box;
    box.Begin()[0] = 42;
    box.Commit(1);
    box.Begin()[0] = 7;
    auto reject = [](const uint8_t*, uint16_t) {
        ADD_FAILURE() << "uncommitted or invalid listing was sent";
        return 0;
    };
    box.Pump(reject);
    box.Commit(BrowseResponseOutbox::kCapacity + 1);
    box.Pump(reject);
}
