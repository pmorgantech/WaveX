#pragma once
#include "track_page_model.h"
#include "ui_page.h"

namespace wavex_ui {
class UIMixerPage : public UIPage {
   public:
    const char* name() const override { return "Mixer"; }
    void onEnter(lv_obj_t*) override;
    void onExit() override;
    void onTrackChanged() override;
    void onInput(const InputEvent&) override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;
    size_t consoleState(char*, size_t, size_t) override;
    bool consoleCommand(const char*, char*, size_t) override;

   private:
    struct Strip {
        UIMixerPage* owner = nullptr;
        uint8_t index = 0;
        TrackMixModel model;
        uint32_t accepted_at = 0;
        lv_obj_t* card = nullptr;
        lv_obj_t* title = nullptr;
        lv_obj_t* slider = nullptr;
        lv_obj_t* level = nullptr;
        lv_obj_t* pan = nullptr;
        lv_obj_t* pan_label = nullptr;
        lv_obj_t* mute = nullptr;
        lv_obj_t* solo = nullptr;
    };
    std::array<Strip, 9> strips_{};
    lv_obj_t* status_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    uint8_t first_ = 0, selected_ = 0, poll_ = 0;
    bool pan_focus_ = false, alive_ = false, pending_ = false, solo_sync_ = false;
    uint32_t sent_at_ = 0, soft_state_ = UINT32_MAX;
    uint8_t track(uint8_t index) const;
    void reset();
    void service();
    void render();
    void read();
    void select(uint8_t);
    bool set(uint8_t, uint8_t, uint16_t);
    bool solo(uint8_t);
    void adjust(int);
    void page();
    static void event(lv_event_t*);
    static void tick(lv_timer_t*);
};
std::shared_ptr<UIPage> createMixerPage();
}  // namespace wavex_ui
