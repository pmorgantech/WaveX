#pragma once
#include "components/ui_value_tile.h"
#include "spi_protocol/protocol.h"
#include "ui_page.h"
namespace wavex_ui {
class UISampleRecordPage : public UIPage {
   public:
    const char* name() const override { return "SampleRecord"; }
    void onEnter(lv_obj_t*) override;
    void onExit() override;
    void onInput(const InputEvent&) override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;
    size_t consoleState(char*, size_t, size_t) override;
    bool consoleCommand(const char*, char*, size_t) override;

   private:
    void adjust(uint8_t, int);
    void send(uint8_t);
    void read();
    void service();
    void render();
    bool ready() const;
    lv_obj_t *root_ = nullptr, *status_label_ = nullptr, *input_ = nullptr, *keyboard_ = nullptr;
    lv_obj_t* meters_[2]{};
    lv_timer_t* timer_ = nullptr;
    ValueTile tiles_[5];
    WaveX::Protocol::RecordOpMessage config_;
    WaveX::Protocol::RecordStatusMessage status_;
    uint32_t read_id_ = 0, pending_ = 0, requested_ = 0, received_ = 0, keys_ = UINT32_MAX;
    uint8_t focus_ = 0;
    bool valid_ = false, alive_ = false;
    char message_[384]{};
};
std::shared_ptr<UIPage> createSampleRecordPage();
}  // namespace wavex_ui
