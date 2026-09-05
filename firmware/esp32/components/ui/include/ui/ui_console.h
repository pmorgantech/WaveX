#pragma once

// ESP32 debug console (debug builds only - WAVEX_DEBUG_HARNESS_ENABLED).
//
// One line reader on the console UART carries every host-driven command:
// the legacy "WAVEX-LOG ..." and "WAVEX-SCREENSHOT" lines the scripts/ tools
// send, and the acknowledged "WAVEX-DBG <seq> <VERB> ..." grammar the HIL
// suite drives (docs/features/debug-harness-and-hil.md §3, grammar shared
// with the Daisy in firmware/shared/debug/console_command.h).
//
// Verbs that only enqueue (KEY, ENC, POT, TAP, TOUCH, LOG, SCREENSHOT) are
// answered from the console task. Verbs that read or change UI state
// (STATE, PAGE, TRACK, HOME) are handed to the UI task through a one-slot
// mailbox and answered when wavex_console_poll() has served them under the
// LVGL lock - nothing on the console task touches LVGL.

#include "config/logging_config.h"

#if WAVEX_DEBUG_HARNESS_ENABLED

// Starts the console task and the synthetic touch indev. Call once from the
// UI task after the display and navigator exist.
void wavex_console_start();

// Call once per UI-task loop pass, WITHOUT the LVGL lock held. Serves one
// pending UI-task verb; cheap when idle.
void wavex_console_poll();

#else

static inline void wavex_console_start() {}
static inline void wavex_console_poll() {}

#endif
