#pragma once
#include "spi_protocol/protocol.h"
#include "ui_page.h"
namespace wavex_ui {
class UIBankPage : public UIPage {
   public:
    const char* name() const override { return "Bank Manager"; }
    const char* contextLine() const override {
        return "Browse Bank slots / recall to selected Track";
    }
    void onEnter(lv_obj_t* parent) override;
    void onExit() override;
    void onTrackChanged() override;
    void onInput(const InputEvent& event) override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;
    std::array<Softkey, NUM_SOFTKEYS> getShiftedSoftkeys() override;
    size_t consoleState(char* out, size_t cap, size_t len) override;
    bool consoleCommand(const char* args, char* reply, size_t cap) override;

   private:
    static void tick(lv_timer_t*);
    static void keyboardEvent(lv_event_t*);
    static void inputEvent(lv_event_t*);
    void service();
    void read();
    void render();
    bool ready() const;
    void choose(uint8_t op);
    void move(int delta);
    void send(uint8_t op);
    void cancel();
    void markSource();
    void slotTools(bool enabled);
    bool canTransfer() const;
    lv_obj_t* input_ = nullptr;
    lv_obj_t* keyboard_ = nullptr;
    lv_obj_t* hint_ = nullptr;
    lv_obj_t* last_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    WaveX::Protocol::BankStatusMessage status_{};
    uint32_t read_id_ = 0, pending_id_ = 0, requested_at_ = 0, pending_at_ = 0, received_at_ = 0,
             seen_read_id_ = 0;
    bool alive_ = false, valid_ = false, slot_tools_ = false;
    int source_slot_ = -1;
    uint32_t source_revision_ = 0;
    char source_name_[24]{};
    WaveX::Protocol::BankSlotOpMessage transfer_draft_;
    uint8_t confirm_ = 0, slot_ = 0;
    WaveX::Protocol::BankOpMessage draft_;
    char message_[192]{};
};
std::shared_ptr<UIPage> createBankPage();
}  // namespace wavex_ui
