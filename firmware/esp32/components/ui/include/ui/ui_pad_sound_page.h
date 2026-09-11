#pragma once
#include "components/ui_value_tile.h"
#include "pad_sound_model.h"
#include "ui_page.h"

namespace wavex_ui {
class UIPadSoundPage : public UIPage {
   public:
    explicit UIPadSoundPage(uint8_t pad) : pad_(pad) {}
    const char* name() const override { return "Pad Sound"; }
    const char* contextLine() const override { return context_; }
    void onEnter(lv_obj_t* parent) override;
    void onExit() override;
    void onTrackChanged() override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;
    size_t consoleState(char* out, size_t cap, size_t len) override;
    bool consoleCommand(const char* args, char* reply, size_t cap) override;

   private:
    void service();
    void read();
    void render();
    void adjust(uint8_t field, int delta);
    void select(int delta);
    static void tick(lv_timer_t* timer);
    PadSoundModel model_;
    ValueTile tiles_[4]{};
    lv_obj_t* status_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    uint8_t pad_ = 0;
    uint32_t read_at_ = 0, pending_at_ = 0;
    uint32_t softkey_state_ = UINT32_MAX;
    bool alive_ = false, timed_out_ = false;
    char context_[96]{};
};
std::shared_ptr<UIPage> createPadSoundPage(uint8_t pad);
}  // namespace wavex_ui
