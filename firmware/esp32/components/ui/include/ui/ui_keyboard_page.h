// WaveX On-Screen Keyboard / Pad Grid
#pragma once

#include <lvgl.h>

#include "ui_page.h"

#include <array>
#include <cstdint>
#include <memory>

namespace wavex_ui {

/**
 * @brief A 4x4 grid of touch pads that trigger notes on the Daisy.
 *
 * A grid whose cell n sends note `root + n` IS a chromatic keyboard spanning
 * 16 semitones, so "virtual piano" and "pad grid" are one widget here - they
 * differ only in the note map and the labels, not in the input handling or the
 * message path (features/digital-voice-audition.md §3).
 *
 * This is currently the ONLY way to trigger a digital voice without external
 * MIDI hardware, and therefore the only way to hear the per-voice filter,
 * envelope and live parameter edits at all. Note that the sample browser's
 * Audition button does NOT exercise this path: it uses the streaming ring
 * buffer, which bypasses VoiceManager entirely.
 *
 * Requires a 16-bit sample to be RAM-resident (loaded from the browser) - the
 * Daisy drops the note otherwise, since there is nothing to play.
 */
class UIKeyboardPage : public UIPage {
   public:
    static constexpr int kRows = 4;
    static constexpr int kCols = 4;
    static constexpr int kPads = kRows * kCols;

    // Voice parameters editable from this page, in display order. These are
    // the digital-path destinations Stage 1 wired up; the values are sent as
    // MSG_CONTROL_CHANGE and reach both sounding voices and the next trigger.
    enum class Param : uint8_t { Cutoff, Resonance, Attack, Decay, Sustain, Release, kCount };

    const char* name() const override { return "Keyboard"; }
    void onEnter(lv_obj_t* parent) override;
    void onExit() override;
    void onInput(const InputEvent& evt) override;
    std::array<Softkey, NUM_SOFTKEYS> getSoftkeys() override;
    std::array<Softkey, NUM_SOFTKEYS> getShiftedSoftkeys() override;

   private:
    static void pad_event_cb(lv_event_t* e);

    void press(int index);
    void release(int index);
    void releaseAll();
    void toggleLatch(int index);
    void setRoot(int root);
    void refreshLabels();

    void stepParam(int direction);
    void selectParam(int direction);
    void sendParam();
    void refreshParamLabel();

    lv_obj_t* pads_[kPads] = {nullptr};
    lv_obj_t* pad_labels_[kPads] = {nullptr};
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* param_label_ = nullptr;

    // Parameters live on THIS page rather than a page of their own, because
    // leaving the page releases every held note - you cannot hold a note here
    // and sweep a control somewhere else.
    //
    // And the sweep itself needs a control that is not the touchscreen: the
    // LVGL port is single-touch, so a finger holding a pad cannot also drag a
    // slider. Hence two paths to the same value - the physical encoder (hold a
    // pad, turn with the other hand) and Latch (tap to sustain, both hands
    // free), so neither a missing encoder nor single-touch blocks the gesture.
    uint16_t param_value_[static_cast<size_t>(Param::kCount)] = {0};
    Param current_param_ = Param::Cutoff;
    bool latch_ = false;

    // The note actually sent for a pad that is currently down. Held per pad
    // rather than recomputed from root_note_ at release time, because the root
    // can change (or the page can exit) between press and release - recomputing
    // would send Note Off for a note that was never started and leave the real
    // one hanging.
    uint8_t sounding_note_[kPads] = {0};
    bool pad_down_[kPads] = {false};

    int root_note_ = 60;  // C4, scientific pitch notation
    uint8_t velocity_ = 100;
};

std::shared_ptr<UIPage> createKeyboardPage();

}  // namespace wavex_ui
