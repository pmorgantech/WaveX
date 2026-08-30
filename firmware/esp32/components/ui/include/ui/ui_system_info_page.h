// WaveX Settings > System page
#pragma once

#include "ui_page.h"

#include <memory>

namespace wavex_ui {

/**
 * @brief Read-only facts about the frontend: firmware version, toolchain,
 *        silicon, reset reason, uptime and memory.
 *
 * This replaces a menu entry that logged "System Info selected" and returned.
 * Everything here is ESP32-local and free to read, so there was nothing to
 * wait for - the entry was a stub, not a hard problem.
 *
 * Deliberately *not* a duplicate of Diagnostics: that page trends live
 * telemetry from both MCUs; this one answers "what is this thing running?"
 */
std::shared_ptr<UIPage> createSystemInfoPage();

}  // namespace wavex_ui
