#pragma once
#include "allocation_model.h"
#include "components/ui_value_tile.h"
#include "ui_page.h"
namespace wavex_ui {
class UIAllocationPage : public UIPage {
   public:
    explicit UIAllocationPage(uint8_t scope) : scope_(scope) {}
    const char* name() const override {
        return scope_ ? "Track Polyphony" : "Instrument Polyphony";
    }
    void onEnter(lv_obj_t*) override;
    void onExit() override;
    void onTrackChanged() override;
    void onInput(const InputEvent&) override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;
    size_t consoleState(char*, size_t, size_t) override;
    bool consoleCommand(const char*, char*, size_t) override;

   private:
    AllocationModel model_;
    ValueTile tiles_[4]{};
    lv_obj_t* status_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    uint32_t read_at_ = 0, pending_at_ = 0, keys_ = UINT32_MAX;
    uint8_t scope_ = 0, focus_ = 1;
    bool alive_ = false, timed_out_ = false;
    void read();
    void service();
    void render();
    void adjust(uint8_t, int);
    bool send(uint8_t op, WaveX::Allocation::Policy policy, bool inherit);
    static void tick(lv_timer_t*);
};
std::shared_ptr<UIPage> createAllocationPage(uint8_t scope);
}  // namespace wavex_ui
