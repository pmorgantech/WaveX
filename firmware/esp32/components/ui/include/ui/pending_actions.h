#pragma once

#include <array>
#include <functional>
#include <utility>

namespace wavex_ui {

// Serialized by the LVGL port lock. A page exit cancels its pending work before its widgets or
// listener owner can be destroyed. Taking an action removes it before calling
// it, so navigation from inside that action can safely cancel the rest.
class PendingActions {
   public:
    static constexpr size_t kCapacity = 8;
    bool push(std::function<void()> action) {
        if (count_ == kCapacity) {
            return false;
        }
        actions_[(head_ + count_) % kCapacity] = std::move(action);
        ++count_;
        return true;
    }
    std::function<void()> pop() {
        if (count_ == 0) {
            return {};
        }
        auto action = std::move(actions_[head_]);
        actions_[head_] = {};
        head_ = (head_ + 1) % kCapacity;
        --count_;
        return action;
    }
    void clear() {
        while (count_ != 0) {
            (void)pop();
        }
    }

   private:
    std::array<std::function<void()>, kCapacity> actions_{};
    size_t head_ = 0;
    size_t count_ = 0;
};

}  // namespace wavex_ui
