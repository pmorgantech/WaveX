// WaveX shared busy / progress overlay
#pragma once

#include <lvgl.h>

#include <cstdint>

namespace wavex_ui {

/**
 * @brief Modal "working on it" overlay: scrim, spinner, caption, optional bar.
 *
 * Shared rather than per page so browser load, preview fetch and card remount
 * all look the same. Created on the active screen, above everything.
 *
 * It is only honest because the ESP32 is *not* blocked during these
 * operations - the Daisy does the SD work and answers over the link, so LVGL
 * keeps redrawing and the spinner actually spins. An overlay on top of a
 * blocked UI task would freeze mid-frame, which is worse than showing nothing.
 *
 * All calls must be made from the UI task (LVGL context).
 */
namespace BusyOverlay {

/**
 * @brief Show (or re-caption) the overlay.
 *
 * @param caption       Headline, e.g. "Loading sample".
 * @param detail        Second line, usually the filename. May be nullptr.
 * @param timeout_ms    Auto-dismiss after this long with an error caption.
 *                      Never pass 0: a spinner that cannot resolve is
 *                      indistinguishable from the freeze it was added to
 *                      explain, and the backend genuinely can fail to answer
 *                      (card pulled mid-load).
 */
void show(const char* caption, const char* detail, uint32_t timeout_ms);

/** Switch to a determinate bar. Call after show(); 0..100. */
void setProgress(int percent);

/** Replace the detail line without disturbing the spinner. */
void setDetail(const char* detail);

/** Dismiss. Safe to call when not shown. */
void hide();

/** True while visible. */
bool isVisible();

/**
 * @brief setProgress()/hide() equivalents that are safe from any task.
 *
 * Progress and completion arrive on the UART RX task, which must not touch
 * widgets - the LVGL task renders on the other core. These only record the
 * request; service() applies it on the UI task.
 *
 * They live here rather than on the page that started the operation so a
 * completion still dismisses the overlay after the user has navigated away,
 * which is exactly when a stuck modal is most confusing.
 */
void requestProgress(int percent);
void requestHide();

/** Apply any pending request. UI task only, with the LVGL lock held. */
void service();

}  // namespace BusyOverlay
}  // namespace wavex_ui
