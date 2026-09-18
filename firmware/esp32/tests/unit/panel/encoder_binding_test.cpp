#include "ui/encoder_binding.h"

#include <gtest/gtest.h>
TEST(EncoderBinding, NormalFineDisabledAndUnknownInputsUseOneDispatchContract) {
    int received = 0;
    wavex_ui::EncoderBindings bindings;
    bindings[2].owner = &received;
    bindings[2].enabled = true;
    bindings[2].parameter = 9;
    bindings[2].onSteps = [](void* target, uint8_t parameter, int steps) {
        EXPECT_EQ(parameter, 9);
        *static_cast<int*>(target) += steps;
    };
    EXPECT_TRUE(wavex_ui::InvokeEncoder(bindings, 2, 3, false));
    EXPECT_EQ(received, 12);
    EXPECT_TRUE(wavex_ui::InvokeEncoder(bindings, 2, -3, true));
    EXPECT_EQ(received, 9);
    EXPECT_FALSE(wavex_ui::InvokeEncoder(bindings, 4, 1, false));
    EXPECT_FALSE(wavex_ui::InvokeEncoder(bindings, 0, 1, false));
    bindings[2].enabled = false;
    EXPECT_FALSE(wavex_ui::InvokeEncoder(bindings, 2, 1, false));
    EXPECT_EQ(received, 9);
}
