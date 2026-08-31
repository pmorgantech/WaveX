/**
 * @file wavex_application.h
 * @brief WaveX Application Class
 *
 * This file defines the WaveXApplication class, which encapsulates the entire
 * application lifecycle and provides a clean, testable entry point.
 */

#pragma once

#include "application_context.h"

#include <memory>

namespace WaveX {

class WaveXApplication {
   public:
    WaveXApplication();
    ~WaveXApplication() = default;

    // Delete copy/move operations - application should be unique
    WaveXApplication(const WaveXApplication&) = delete;
    WaveXApplication& operator=(const WaveXApplication&) = delete;
    WaveXApplication(WaveXApplication&&) = delete;
    WaveXApplication& operator=(WaveXApplication&&) = delete;

    /**
     * @brief Bring up inter-MCU, PCNT encoders, MIDI (DIN + USB), and UI, in that order.
     *
     * MIDI failures are logged and skipped (the instrument works without MIDI);
     * every other subsystem failing here aborts startup. See main.cpp's app_main
     * for what a false return means for the caller (restart, not a plain return).
     */
    bool initialize();

    /** Runs until reset; does not return under normal operation. */
    void run();

   private:
    bool initializeInterMCU();
    bool initializePCNT();
    bool initializeUI();
    void logSystemStatus();

    // Application context owns all system components
    ApplicationContext m_context;

    // Status tracking
    bool m_initialized;
    int m_loopCounter;
    int m_lastHeapLogTime;
};

}  // namespace WaveX
