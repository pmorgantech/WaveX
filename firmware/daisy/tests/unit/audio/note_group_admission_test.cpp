#include "audio/note_group_admission.hpp"

#include <gtest/gtest.h>

namespace {
using namespace WaveX::AudioEngine::Allocation;
using Planner = Admission<4, 4>;
constexpr Owner own{1, 0}, other{1, 1};
Group Note(
    uint64_t id, uint64_t slots, Owner owner = own, uint8_t channels = 1, bool releasing = false) {
    return {id, slots, owner, channels, releasing};
}
Request Hit(uint8_t slots = 1,
            uint8_t channels = 1,
            uint8_t limit = 0,
            StealFrom steal = StealFrom::Any) {
    return {own, {PlayMode::Poly, limit, steal}, slots, channels};
}
void Rejected(const Plan& plan, Result result) {
    EXPECT_EQ(plan.result, result);
    EXPECT_EQ(plan.victims, 0u);
    EXPECT_EQ(plan.retire_slots, 0u);
    EXPECT_EQ(plan.new_slots, 0u);
}
unsigned Bits(uint64_t mask) {
    unsigned count = 0;
    for (unsigned i = 0; i < 64; ++i)
        count += (mask >> i) & 1;
    return count;
}

TEST(NoteGroupAdmission, ConfiguredCapacityAcceptsDefaultPolicyWithDeterministicSlots) {
    Admission<>::Snapshot groups{};
    const auto plan = Admission<>::Build(groups, Hit(2, 2));
    ASSERT_EQ(plan.result, Result::Accepted);
    EXPECT_EQ(plan.new_slots, 3u);
    EXPECT_EQ(plan.victims, 0u);
}

TEST(NoteGroupAdmission, LocalCapCountsNotesRatherThanLayersWithSpareCapacity) {
    Planner::Snapshot groups{};
    groups[0] = Note(1, 3, own, 2);  // one musical note, two layers
    auto plan = Planner::Build(groups, Hit(1, 1, 2));
    ASSERT_EQ(plan.result, Result::Accepted);
    EXPECT_EQ(plan.victims, 0u);
    EXPECT_EQ(plan.new_slots, 4u);
    plan = Planner::Build(groups, Hit(1, 1, 1));
    ASSERT_EQ(plan.result, Result::Accepted);
    EXPECT_EQ(plan.victims, 1u);
    EXPECT_EQ(plan.retire_slots, 3u);
    EXPECT_EQ(plan.new_slots, 1u);
}

TEST(NoteGroupAdmission, LoweredCapRetiresEnoughOwnGroupsIncludingReleaseTails) {
    Planner::Snapshot groups{Note(3, 1), Note(2, 2, own, 1, true), Note(1, 4), {}};
    auto request = Hit(1, 1, 8, StealFrom::OwnOnly);
    request.policy.mode = PlayMode::Mono;
    const auto plan = Planner::Build(groups, request);
    ASSERT_EQ(plan.result, Result::Accepted);
    EXPECT_EQ(plan.victims, 7u);
    EXPECT_EQ(plan.retire_slots, 7u);
    EXPECT_EQ(plan.new_slots, 1u);
}

TEST(NoteGroupAdmission, OwnOnlyRejectionDiscardsTentativeLocalVictims) {
    const Planner::Snapshot groups{
        Note(1, 1), Note(2, 2, other), Note(3, 4, other), Note(4, 8, other)};
    Rejected(Planner::Build(groups, Hit(1, 2, 1, StealFrom::OwnOnly)), Result::NoCapacity);
    // The caller's group remains active even after a cap victim was considered.
    EXPECT_EQ(groups[0].slots, 1u);
    EXPECT_EQ(groups[0].id, 1u);
    auto newcomer = Hit(1, 1, 0, StealFrom::OwnOnly);
    newcomer.owner = {2, 0};  // same Track, a different loaded Instrument instance
    Rejected(Planner::Build(groups, newcomer), Result::NoCapacity);
}

TEST(NoteGroupAdmission, OwnFirstPrefersItsPlayingGroupBeforeForeignRelease) {
    const Planner::Snapshot groups{
        Note(4, 1), Note(1, 2, other, 1, true), Note(2, 4, other), Note(3, 8, other)};
    const auto local = Planner::Build(groups, Hit(1, 1, 0, StealFrom::OwnFirst));
    ASSERT_EQ(local.result, Result::Accepted);
    EXPECT_EQ(local.victims, 1u);
    const auto global = Planner::Build(groups, Hit());
    ASSERT_EQ(global.result, Result::Accepted);
    EXPECT_EQ(global.victims, 2u);
}

TEST(NoteGroupAdmission, StereoAdmissionRetiresWholeGroupsAndRanksReleaseThenOnset) {
    Planner::Snapshot groups{
        Note(4, 1), Note(2, 2, other, 1, true), Note(1, 4, other), Note(3, 8, other, 1, true)};
    auto plan = Planner::Build(groups, Hit(1, 2));
    ASSERT_EQ(plan.result, Result::Accepted);
    EXPECT_EQ(plan.victims, 10u);  // both release tails before older held note
    EXPECT_EQ(plan.new_slots, 2u);
    groups = {Note(1, 9, other, 3), Note(2, 2), {}, {}};
    plan = Planner::Build(groups, Hit(2, 3));
    ASSERT_EQ(plan.result, Result::Accepted);
    EXPECT_EQ(plan.victims, 1u);
    EXPECT_EQ(plan.retire_slots, 9u);  // retire both nonadjacent layers
    EXPECT_EQ(plan.new_slots, 5u);     // no collision with retained slot 1
}

TEST(NoteGroupAdmission, RenderSlotAndChannelBudgetsAreIndependent) {
    Admission<4, 8>::Snapshot groups{
        Note(1, 1), Note(2, 2, other), Note(3, 4, other), Note(4, 8, other)};
    Rejected(Admission<4, 8>::Build(groups, Hit(2, 2, 0, StealFrom::OwnOnly)), Result::NoCapacity);
    auto plan = Admission<4, 8>::Build(groups, Hit(2, 2));
    ASSERT_EQ(plan.result, Result::Accepted);
    EXPECT_EQ(plan.victims, 3u);
    EXPECT_EQ(plan.new_slots, 3u);
    Planner::Snapshot stereo{Note(1, 1, own, 2), Note(2, 2, other, 2), {}, {}};
    plan = Planner::Build(stereo, Hit());
    ASSERT_EQ(plan.result, Result::Accepted);
    EXPECT_EQ(plan.victims, 1u);  // free slots do not provide free channels
}

TEST(NoteGroupAdmission, RejectsInvalidRequestsAndInconsistentSnapshots) {
    Planner::Snapshot groups{};
    for (const auto request: {Hit(0, 0), Hit(2, 1), Hit(1, 3), Hit(1, 1, 9)})
        Rejected(Planner::Build(groups, request), Result::InvalidRequest);
    Rejected(Planner::Build(groups, Hit(3, 6)), Result::NoCapacity);
    auto request = Hit();
    request.owner.binding = 0;
    Rejected(Planner::Build(groups, request), Result::InvalidRequest);
    request = Hit();
    request.owner.track = 16;
    Rejected(Planner::Build(groups, request), Result::InvalidRequest);
    request = Hit();
    request.policy.steal = static_cast<StealFrom>(3);
    Rejected(Planner::Build(groups, request), Result::InvalidRequest);
    request = Hit();
    request.policy.mode = static_cast<PlayMode>(2);
    Rejected(Planner::Build(groups, request), Result::InvalidRequest);
    for (const auto invalid: {Note(0, 1),
                              Note(1, 16),
                              Note(1, 1, {0, 0}),
                              Note(1, 1, {1, 16}),
                              Note(1, 3, own, 1),
                              Note(1, 1, own, 3)}) {
        groups[0] = invalid;
        Rejected(Planner::Build(groups, Hit()), Result::InvalidSnapshot);
    }
    groups = {Note(1, 1), Note(1, 2), {}, {}};
    Rejected(Planner::Build(groups, Hit()), Result::InvalidSnapshot);  // duplicate identity
    groups = {Note(1, 1), Note(2, 1), {}, {}};
    Rejected(Planner::Build(groups, Hit()), Result::InvalidSnapshot);  // shared render slot
    groups = {Note(1, 1, own, 2), Note(2, 2, own, 2), Note(3, 4), {}};
    Rejected(Planner::Build(groups, Hit()), Result::InvalidSnapshot);  // over budget
}

TEST(NoteGroupAdmission, SupportsHighestSlotAndGroupBitsWithoutShiftOverflow) {
    Admission<64, 64>::Snapshot groups{};
    for (unsigned i = 0; i < 64; ++i)
        groups[i] = Note(i + 1, uint64_t{1} << i, other);
    groups[63].owner = own;
    const auto plan = Admission<64, 64>::Build(groups, Hit(1, 1, 1, StealFrom::OwnOnly));
    ASSERT_EQ(plan.result, Result::Accepted);
    EXPECT_EQ(plan.victims, uint64_t{1} << 63);
    EXPECT_EQ(plan.retire_slots, uint64_t{1} << 63);
    EXPECT_EQ(plan.new_slots, uint64_t{1} << 63);
}

// Independent brute-force feasibility oracle: enumerate all victim subsets,
// rather than duplicating the planner's ranking/selection algorithm.
TEST(NoteGroupAdmission, ExhaustiveSmallSnapshotsMatchFeasibilityAndConserveResources) {
    unsigned cases = 0;
    for (unsigned encoded = 0; encoded < 625; ++encoded) {
        Planner::Snapshot groups{};
        unsigned value = encoded, total = 0;
        uint64_t occupied = 0;
        for (unsigned i = 0; i < 4; ++i, value /= 5) {
            const unsigned state = value % 5;
            if (state) {
                const auto channels = static_cast<uint8_t>(state > 2 ? 2 : 1);
                groups[i] = Note(i + 1, 1u << i, state % 2 ? own : other, channels, i % 2);
                total += channels;
                occupied |= 1u << i;
            }
        }
        if (total > 4)
            continue;
        for (auto steal: {StealFrom::Any, StealFrom::OwnFirst, StealFrom::OwnOnly})
            for (uint8_t cap: {0, 1, 2})
                for (uint8_t slots = 1; slots <= 4; ++slots)
                    for (uint8_t channels = slots; channels <= 2 * slots; ++channels) {
                        bool possible = false;
                        for (unsigned victims = 0; victims < 16; ++victims) {
                            unsigned remaining_slots = 0, remaining_channels = 0, own_count = 0;
                            bool allowed = true;
                            for (unsigned i = 0; i < 4; ++i) {
                                const auto& g = groups[i];
                                if (victims & (1u << i)) {
                                    allowed &=
                                        g.slots && (steal != StealFrom::OwnOnly || g.owner == own);
                                } else if (g.slots) {
                                    ++remaining_slots;
                                    remaining_channels += g.channels;
                                    own_count += g.owner == own;
                                }
                            }
                            possible |= allowed && remaining_slots + slots <= 4 &&
                                        remaining_channels + channels <= 4 &&
                                        (!cap || own_count < cap);
                        }
                        const auto plan = Planner::Build(groups, Hit(slots, channels, cap, steal));
                        ASSERT_EQ(plan.result == Result::Accepted, possible) << encoded;
                        if (!possible) {
                            Rejected(plan, Result::NoCapacity);
                            continue;
                        }
                        uint64_t retired = 0;
                        unsigned freed_channels = 0, own_count = 0;
                        for (unsigned i = 0; i < 4; ++i) {
                            const auto& g = groups[i];
                            if (plan.victims & (1u << i)) {
                                ASSERT_NE(g.slots, 0u);
                                if (steal == StealFrom::OwnOnly)
                                    ASSERT_TRUE(g.owner == own);
                                retired |= g.slots;
                                freed_channels += g.channels;
                            } else if (g.slots)
                                own_count += g.owner == own;
                        }
                        EXPECT_EQ(plan.retire_slots, retired);
                        EXPECT_EQ(plan.new_slots & (occupied & ~retired), 0u);
                        EXPECT_EQ(plan.new_slots & ~uint64_t{15}, 0u);
                        EXPECT_EQ(Bits(plan.new_slots), slots);
                        EXPECT_LE(total - freed_channels + channels, 4u);
                        EXPECT_TRUE(!cap || own_count < cap);
                        ++cases;
                    }
    }
    EXPECT_GT(cases, 1000u);
}
}  // namespace
