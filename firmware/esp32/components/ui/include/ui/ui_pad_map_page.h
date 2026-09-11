#pragma once
#include "components/ui_value_tile.h"
#include "key_map_model.h"
#include "spi_protocol/protocol.h"
#include "ui_page.h"

namespace wavex_ui {
class UIPadMapPage : public UIPage {
   public:
    explicit UIPadMapPage(bool keyboard = false) : keyboard_(keyboard) {}
    const char* name() const override { return keyboard_ ? "Key Map" : "Pad Map"; }
    const char* contextLine() const override { return context_; }
    void onEnter(lv_obj_t* parent) override;
    void onExit() override;
    void onTrackChanged() override;
    void onInput(const InputEvent& event) override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;
    std::array<Softkey, NUM_SOFTKEYS> getShiftedSoftkeys() override;
    size_t consoleState(char* out, size_t cap, size_t len) override;
    bool consoleCommand(const char* args, char* reply, size_t cap) override;

   private:
    void renderKeys();
    void adjustKey(uint8_t field, int delta);
    void applyKeys();
    bool sendKey(WaveX::Protocol::InstKeyMapOpMessage request);
    bool keyDirty() const { return keyboard_ && key_model_.Dirty(); }
    uint8_t newOp() const {
        return keyboard_ ? WaveX::Protocol::INST_OP_NEW_KEYBOARD : WaveX::Protocol::INST_OP_NEW;
    }
    uint16_t selectedSample() const {
        return keyboard_ ? key_model_.Draft().sample_id : map_.pads[selected_].sample_id;
    }
    const bool keyboard_;
    KeyMapModel key_model_;
    ValueTile key_tiles_[5]{};
    uint8_t audition_note_ = 0, audition_track_ = 0;
    uint32_t audition_at_ = 0;
    enum class View { Pads, Names, Samples, ConfirmNew };
    struct Button {
        UIPadMapPage* owner = nullptr;
        uint8_t index = 0;
        lv_obj_t* object = nullptr;
        lv_obj_t* label = nullptr;
        uint16_t sample = 0;
    };
    static void padEvent(lv_event_t*);
    static void sampleEvent(lv_event_t*);
    static void tick(lv_timer_t*);
    static void keyboardEvent(lv_event_t*);
    void service();
    void render();
    void read();
    bool send(WaveX::Protocol::InstOpMessage request);
    void select(uint8_t pad, bool audition);
    void setPad(uint16_t sample, uint8_t choke);
    void showNames(uint8_t op);
    void acceptName();
    void showSamples(uint16_t first = 0);
    void closeOverlay();
    bool editable() const;
    Button pads_[16]{}, choices_[8]{};
    lv_obj_t* status_ = nullptr;
    lv_obj_t* title_ = nullptr;
    lv_obj_t* overlay_ = nullptr;
    lv_obj_t* input_ = nullptr;
    lv_obj_t* name_hint_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    WaveX::Protocol::InstZoneSyncMessage map_{};
    View view_ = View::Pads;
    uint8_t selected_ = 0, name_op_ = 0;
    uint16_t sample_first_ = 0, sample_total_ = 0;
    uint32_t read_id_ = 0, pending_id_ = 0, requested_at_ = 0, pending_at_ = 0;
    bool valid_ = false, alive_ = false, failed_ = false;
    char message_[160]{}, context_[96]{};
};
std::shared_ptr<UIPage> createPadMapPage();
std::shared_ptr<UIPage> createKeyMapPage();
}  // namespace wavex_ui
