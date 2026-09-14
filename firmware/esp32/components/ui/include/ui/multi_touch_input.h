#pragma once

#include "lvgl.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace wavex_ui {

// One controller snapshot, with identity independent of its array position.
// Coordinates are in the driver's native orientation; LVGL rotates each pointer.
struct TouchContact {
    uint8_t id = 0;
    lv_point_t point{};
};

// All methods run under the LVGL port lock. Owns pointer devices, not the panel,
// controller, tick or task. No allocations after init().
class MultiTouchInput {
   public:
    static constexpr size_t kMaxContacts = 5;
    MultiTouchInput() = default;
    MultiTouchInput(const MultiTouchInput&) = delete;
    MultiTouchInput& operator=(const MultiTouchInput&) = delete;
    bool init(lv_display_t* display);
    void deinit();
    void update(const TouchContact* contacts, size_t count);
    lv_indev_t* pointer(size_t index) const { return slots_[index].indev; }

   private:
    struct Slot {
        lv_indev_t* indev = nullptr;
        TouchContact contact{};
        bool pressed = false;
    };
    static void read(lv_indev_t* indev, lv_indev_data_t* data);
    std::array<Slot, kMaxContacts> slots_{};
};

}  // namespace wavex_ui
