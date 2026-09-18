#pragma once
#include "components/ui_value_tile.h"
#include "track_page_model.h"
#include "ui_page.h"
namespace wavex_ui {
class UITrackPage : public UIPage {
   public:
    const char* name() const override { return "Project"; }
    void onEnter(lv_obj_t*) override;
    void onExit() override;
    void onTrackChanged() override;
    void onInput(const InputEvent&) override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;
    std::array<Softkey, NUM_SOFTKEYS> getShiftedSoftkeys() override;
    size_t consoleState(char*, size_t, size_t) override;
    bool consoleCommand(const char*, char*, size_t) override;

   private:
    struct Cell {
        UITrackPage* owner;
        uint8_t index;
        lv_obj_t* object;
        lv_obj_t* label;
    };
    std::array<Cell, 8> cells_{};
    TrackPageModel model_;
    ValueTile midi_, level_, pan_, mute_;
    TrackMixModel mix_;
    uint32_t mix_read_at_ = 0, mix_accepted_at_ = 0;
    bool mix_pending_ = false;
    uint8_t control_ = 0;  // MIDI / level / pan / mute: shared touch and encoder focus.
    void readMix();
    void serviceMix();
    void setMix(uint8_t op, uint16_t value);
    void adjustMix(uint8_t op, int delta);
    void focusControl(uint8_t control);
    static void controlEvent(lv_event_t* event);
    lv_obj_t* heading_ = nullptr;
    lv_obj_t* status_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    uint8_t first_ = 0;
    uint32_t read_id_ = 0, read_at_ = 0, accepted_at_ = 0, soft_state_ = UINT32_MAX;
    bool pending_ = false, alive_ = false;
    void select(uint8_t);
    void setMidi(uint8_t);
    void setProgramChange(bool);
    void adjust(int);
    void read();
    void service();
    void render();
    static void tick(lv_timer_t*);
    static void choose(lv_event_t*);
};
std::shared_ptr<UIPage> createTrackPage();
}  // namespace wavex_ui
