#include "ui/multi_touch_input.h"

namespace wavex_ui {

bool MultiTouchInput::init(lv_display_t* display) {
    for (auto& slot: slots_) {
        slot.indev = lv_indev_create();
        if (!slot.indev) {
            deinit();
            return false;
        }
        lv_indev_set_type(slot.indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_disp(slot.indev, display);
        lv_indev_set_driver_data(slot.indev, &slot);
        lv_indev_set_read_cb(slot.indev, read);
        // One controller poll drives all pointers from the same snapshot.
        lv_indev_set_mode(slot.indev, LV_INDEV_MODE_EVENT);
    }
    return true;
}

void MultiTouchInput::deinit() {
    for (auto& slot: slots_) {
        if (slot.indev) {
            lv_indev_delete(slot.indev);
        }
        slot = {};
    }
}

void MultiTouchInput::read(lv_indev_t* indev, lv_indev_data_t* data) {
    const auto* slot = static_cast<const Slot*>(lv_indev_get_driver_data(indev));
    data->point = slot->contact.point;
    data->state = slot->pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void MultiTouchInput::update(const TouchContact* contacts, size_t count) {
    if (!contacts) {
        count = 0;
    }
    if (count > kMaxContacts) {
        count = kMaxContacts;
    }
    // Release missing IDs before a slot can be reused. In particular, lifting
    // contact 0 must not turn contact 1 into a drag on contact 0's widget.
    for (auto& slot: slots_) {
        if (!slot.pressed) {
            continue;
        }
        bool found = false;
        for (size_t i = 0; i < count; ++i) {
            found |= contacts[i].id == slot.contact.id;
        }
        if (!found) {
            slot.pressed = false;
            lv_indev_read(slot.indev);
        }
    }
    for (size_t i = 0; i < count; ++i) {
        Slot* match = nullptr;
        for (auto& slot: slots_) {
            if (slot.pressed && slot.contact.id == contacts[i].id) {
                match = &slot;
                break;
            }
        }
        if (!match) {
            for (auto& slot: slots_) {
                if (!slot.pressed) {
                    match = &slot;
                    break;
                }
            }
        }
        if (match) {
            match->contact = contacts[i];
            match->pressed = true;
        }
    }
    for (auto& slot: slots_) {
        if (slot.pressed) {
            lv_indev_read(slot.indev);
        }
    }
}

}  // namespace wavex_ui
