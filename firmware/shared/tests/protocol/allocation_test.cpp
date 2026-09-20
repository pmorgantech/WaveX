#include <gtest/gtest.h>

#include "spi_protocol/protocol.h"

#include <array>
#include <cstring>
using namespace WaveX::Protocol;
namespace A = WaveX::Allocation;
TEST(AllocationProtocol, AllOperationsAndScopesRoundTrip) {
    std::array<uint8_t, 64> packet{};
    for (uint8_t scope = 0; scope < 2; ++scope)
        for (uint8_t op = 0; op <= ALLOC_REVERT; ++op) {
            AllocationOpMessage in;
            in.request_id = 42;
            in.revision = 123;
            in.track = 15;
            in.scope = scope;
            in.op = op;
            in.inherit = scope;
            in.policy = {A::PlayMode::Mono, 8, A::StealFrom::OwnFirst};
            ASSERT_TRUE(IsValidAllocationOp(in));
            ASSERT_GT(ProtocolHandler::CreatePacket(
                          packet.data(), packet.size(), MSG_ALLOC_OP, &in, sizeof(in)),
                      0);
            AllocationOpMessage out;
            ASSERT_TRUE(
                ProtocolHandler::ParseMessage(packet.data(), MSG_ALLOC_OP, &out, sizeof(out)));
            EXPECT_EQ(std::memcmp(&in, &out, sizeof(in)), 0);
        }
    AllocationSyncMessage in;
    in.request_id = 4;
    in.completed_request_id = 3;
    in.revision = 18;
    in.track = 15;
    in.valid = 1;
    in.loaded = 1;
    in.scope = ALLOC_TRACK;
    in.dirty = 1;
    in.sound = {A::PlayMode::Mono, 2, A::StealFrom::OwnOnly};
    ASSERT_TRUE(IsValidAllocationSync(in));
    ASSERT_GT(ProtocolHandler::CreatePacket(
                  packet.data(), packet.size(), MSG_ALLOC_SYNC, &in, sizeof(in)),
              0);
    AllocationSyncMessage out;
    ASSERT_TRUE(ProtocolHandler::ParseMessage(packet.data(), MSG_ALLOC_SYNC, &out, sizeof(out)));
    EXPECT_EQ(std::memcmp(&in, &out, sizeof(in)), 0);
}
TEST(AllocationProtocol, RejectsMalformedEditsBeforeMutation) {
    AllocationOpMessage good;
    good.request_id = 1;
    good.revision = 2;
    good.op = ALLOC_SET;
    ASSERT_TRUE(IsValidAllocationOp(good));
    auto bad = good;
    bad.policy.mode = static_cast<A::PlayMode>(2);
    EXPECT_FALSE(IsValidAllocationOp(bad));
    bad = good;
    bad.policy.limit = 9;
    EXPECT_FALSE(IsValidAllocationOp(bad));
    bad = good;
    bad.policy.steal = static_cast<A::StealFrom>(3);
    EXPECT_FALSE(IsValidAllocationOp(bad));
    bad = good;
    bad.track = 16;
    EXPECT_FALSE(IsValidAllocationOp(bad));
    bad = good;
    bad.revision = 0;
    EXPECT_FALSE(IsValidAllocationOp(bad));
    bad = good;
    bad.inherit = 1;
    EXPECT_FALSE(IsValidAllocationOp(bad));
    bad = good;
    bad.reserved = 1;
    EXPECT_FALSE(IsValidAllocationOp(bad));
    bad = good;
    bad.scope = 2;
    EXPECT_FALSE(IsValidAllocationOp(bad));
    bad = good;
    bad.op = 4;
    EXPECT_FALSE(IsValidAllocationOp(bad));
    bad = good;
    bad.request_id = 0;
    EXPECT_FALSE(IsValidAllocationOp(bad));
}
