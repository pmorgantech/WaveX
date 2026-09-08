#include "ui/sequencer_grid_model.h"

#include <gtest/gtest.h>

using wavex_ui::SequencerGridModel;

namespace {
SequencerGridModel::Page Reply(const SequencerGridModel::Request& request) {
    SequencerGridModel::Page page;
    page.request_id = request.request_id;
    page.track = request.track;
    page.first_step = request.first_step;
    page.valid = 1;
    page.length = 64;
    page.enabled = 1;
    page.scale = 1;
    return page;
}
}  // namespace
TEST(SequencerGridModelTest, StalePagesCannotOverwriteAnotherWindow) {
    SequencerGridModel model;
    const auto old = Reply(model.BeginRead(1, 0));
    ASSERT_TRUE(model.SetWindow(12, 48));
    const auto current = Reply(model.BeginRead(2, 3));
    EXPECT_FALSE(model.Accept(old));
    EXPECT_FALSE(model.Ready(3));
    ASSERT_TRUE(model.Accept(current));
    EXPECT_EQ(model.Row(3).track, 15);
    EXPECT_EQ(model.Row(3).first_step, 48);
    EXPECT_FALSE(model.Waiting());
    EXPECT_FALSE(model.Accept(current));  // duplicate is not another completed read
}
TEST(SequencerGridModelTest, InvalidDataCannotMakeCellsEditable) {
    SequencerGridModel model;
    auto page = Reply(model.BeginRead(42, 2));
    page.steps[15].note = 128;
    EXPECT_FALSE(model.Accept(page));
    page.steps[15].note = 75;
    page.steps[15].velocity = 128;
    EXPECT_FALSE(model.Accept(page));
    EXPECT_FALSE(model.Ready(2));
    page.steps[15].velocity = 127;
    page.steps[15].probability = 101;
    EXPECT_FALSE(model.Accept(page));
    page.steps[15].probability = 100;
    page.valid = 0;
    EXPECT_FALSE(model.Accept(page));
    page.valid = 1;
    page.first_step = 16;
    EXPECT_FALSE(model.Accept(page));
    page.first_step = 0;
    EXPECT_TRUE(model.Accept(page));
}
TEST(SequencerGridModelTest, FourRowsBecomeReadyIndependentlyAndEditsRevalidateTheirRow) {
    SequencerGridModel model;
    for (uint8_t row = 0; row < 4; ++row)
        ASSERT_TRUE(model.Accept(Reply(model.BeginRead(row + 1, row))));
    EXPECT_TRUE(model.AllReady());
    model.InvalidateRow(2);
    EXPECT_FALSE(model.AllReady());
    EXPECT_TRUE(model.Ready(0));
    EXPECT_FALSE(model.Ready(2));
    EXPECT_TRUE(model.Ready(3));
    EXPECT_TRUE(model.Accept(Reply(model.BeginRead(5, 2))));
    EXPECT_TRUE(model.AllReady());
    model.Invalidate();
    EXPECT_FALSE(model.AllReady());
    EXPECT_FALSE(model.Ready(0));
}
TEST(SequencerGridModelTest, WindowAndRequestBoundsAreRejectedWithoutChangingSelection) {
    SequencerGridModel model;
    ASSERT_TRUE(model.SetWindow(12, 48));
    EXPECT_FALSE(model.SetWindow(16, 48));
    EXPECT_FALSE(model.SetWindow(3, 0));
    EXPECT_FALSE(model.SetWindow(0, 64));
    EXPECT_FALSE(model.SetWindow(0, 49));
    EXPECT_EQ(model.FirstTrack(), 12);
    EXPECT_EQ(model.FirstStep(), 48);
    EXPECT_EQ(model.BeginRead(10, 4).request_id, 0u);
    EXPECT_EQ(model.BeginRead(0, 0).request_id, 0u);
}

TEST(SequencerGridModelTest, RapidEditsAccumulateUntilFreshBackendReadback) {
    SequencerGridModel model;
    auto page = Reply(model.BeginRead(1, 0));
    page.steps[3].on = 1;
    page.steps[3].velocity = 80;
    page.steps[3].probability = 75;
    ASSERT_TRUE(model.Accept(page));
    SequencerGridModel::Step step;
    ASSERT_TRUE(model.CopyStepForEdit(0, 3, step));
    step.velocity += 5;
    model.InvalidateRow(0);
    model.PreviewStep(0, 3, step);
    auto stale = Reply(model.BeginRead(2, 0));
    ASSERT_TRUE(model.CopyStepForEdit(0, 3, step));
    step.velocity += 5;
    step.probability -= 10;
    model.InvalidateRow(0);
    model.PreviewStep(0, 3, step);
    auto fresh = Reply(model.BeginRead(3, 0));
    EXPECT_FALSE(model.Accept(stale));
    ASSERT_TRUE(model.CopyStepForEdit(0, 3, step));
    EXPECT_EQ(step.on, 1);
    EXPECT_EQ(step.velocity, 90);
    EXPECT_EQ(step.probability, 65);
    EXPECT_EQ(model.Row(0).steps[3].velocity, 80);    // confirmed state stays separate
    EXPECT_FALSE(model.CopyStepForEdit(0, 4, step));  // no guesses for another cell
    fresh.steps[3].velocity = 88;                     // backend wins even if it differs
    ASSERT_TRUE(model.Accept(fresh));
    ASSERT_TRUE(model.CopyStepForEdit(0, 3, step));
    EXPECT_EQ(step.velocity, 88);
}
TEST(SequencerGridModelTest, PreviewLifetimeIsBoundedBySelectionWindowAndLink) {
    SequencerGridModel model;
    SequencerGridModel::Step step;
    EXPECT_FALSE(model.CopyStepForEdit(0, 0, step));
    model.PreviewStep(0, 0, step);
    ASSERT_TRUE(model.Accept(Reply(model.BeginRead(1, 1))));
    EXPECT_TRUE(model.CopyStepForEdit(0, 0, step));  // another row cannot acknowledge it
    model.DiscardPreview();
    EXPECT_FALSE(model.CopyStepForEdit(0, 0, step));
    model.PreviewStep(0, 0, step);
    model.SetWindow(4, 16);
    EXPECT_FALSE(model.CopyStepForEdit(0, 0, step));
    model.PreviewStep(0, 0, step);
    model.Invalidate();
    EXPECT_FALSE(model.CopyStepForEdit(0, 0, step));
    EXPECT_FALSE(model.CopyStepForEdit(4, 0, step));
    EXPECT_FALSE(model.CopyStepForEdit(0, 16, step));
}
