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
    /**
     * @brief Construct a new WaveX Application object
     */
    WaveXApplication();

    /**
     * @brief Destroy the WaveX Application object
     */
    ~WaveXApplication() = default;

    // Delete copy/move operations - application should be unique
    WaveXApplication(const WaveXApplication&) = delete;
    WaveXApplication& operator=(const WaveXApplication&) = delete;
    WaveXApplication(WaveXApplication&&) = delete;
    WaveXApplication& operator=(WaveXApplication&&) = delete;

    /**
     * @brief Initialize the application
     *
     * Initializes all subsystems in the correct order.
     * @return true if initialization succeeded, false otherwise
     */
    bool initialize();

    /**
     * @brief Run the main application loop
     *
     * This method contains the main application loop and should not return
     * under normal circumstances.
     */
    void run();

   private:
    /**
     * @brief Initialize inter-MCU communication
     * @return true if successful
     */
    bool initializeInterMCU();

    /**
     * @brief Initialize PCNT encoders
     * @return true if successful
     */
    bool initializePCNT();

    /**
     * @brief Initialize UI system
     * @return true if successful
     */
    bool initializeUI();

    /**
     * @brief Log system status periodically
     */
    void logSystemStatus();

    // Application context owns all system components
    ApplicationContext m_context;

    // Status tracking
    bool m_initialized;
    int m_loopCounter;
    int m_lastHeapLogTime;
};

}  // namespace WaveX
