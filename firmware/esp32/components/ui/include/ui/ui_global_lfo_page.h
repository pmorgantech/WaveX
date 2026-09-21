#pragma once
#include "components/ui_value_tile.h"
#include "spi_protocol/protocol.h"
#include "ui_page.h"
namespace wavex_ui {
class UIGlobalLfoPage : public UIPage {
   public:
    const char* name() const override { return "Global LFO"; }
    void onEnter(lv_obj_t*) override;
    void onExit() override;
    EncoderBindings encoderBindings() override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;
    size_t consoleState(char*, size_t, size_t) override;
    bool consoleCommand(const char*, char*, size_t) override;

   private:
    void service();
    void read();
    void send(uint8_t op);
    void adjust(uint8_t field, int delta);
    void render();
    bool ready() const;
    lv_obj_t *root_ = nullptr, *status_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    ValueTile tiles_[4];
    WaveX::Protocol::GlobalLfoSyncMessage state_;
    WaveX::Protocol::GlobalLfoSettings desired_;
    uint32_t expected_ = 0, pending_ = 0, read_at_ = 0, sent_at_ = 0, received_at_ = 0;
    bool valid_ = false, alive_ = false, drawn_ready_ = false;
};
std::shared_ptr<UIPage> createGlobalLfoPage();
}  // namespace wavex_ui
