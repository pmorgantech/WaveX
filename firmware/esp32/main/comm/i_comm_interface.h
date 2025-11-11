/**
 * @file i_comm_interface.h
 * @brief Communication Interface Definition
 *
 * This file defines the ICommInterface abstract base class that provides
 * a clean separation between the UI layer and the communication layer.
 * The interface abstracts inter-MCU communication operations, reducing
 * tight coupling and enabling better testability and maintainability.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
#endif

// Include data structure definitions
#include "../inter_mcu.h"

// Callback type definitions (matching inter_mcu.h)
typedef void (*wavex_meter_cb_t)(
    float rms_left, float rms_right, float peak_left, float peak_right, void* user_data);
typedef void (*wavex_browse_resp_cb_t)(const uint8_t* data, size_t length, void* user_data);
typedef void (*wavex_sample_status_cb_t)(
    uint8_t state, uint32_t sample_rate, uint8_t channels, uint32_t frames_played, void* user_data);

namespace WaveX {
namespace Comm {

/**
 * @brief Interface for communication operations
 *
 * This interface abstracts the communication layer operations that the UI
 * depends on, reducing coupling between UI and comm layers.
 */
class ICommInterface {
   public:
    virtual ~ICommInterface() = default;

    // Meter data operations
    virtual void setMeterListener(wavex_meter_cb_t cb, void* user_data) = 0;
    virtual void getMeterData(wavex_meter_data_t* out) = 0;

    // File browsing operations
    virtual void setBrowseResponseListener(wavex_browse_resp_cb_t cb, void* user_data) = 0;
    virtual esp_err_t sendBrowseRequest(const char* path, uint8_t start_index) = 0;

    // Sample control operations
    virtual void setSampleStatusListener(wavex_sample_status_cb_t cb, void* user_data) = 0;
    virtual esp_err_t sendSamplePlayRequest(uint32_t file_index) = 0;
    virtual esp_err_t sendSampleStopRequest() = 0;

    // Diagnostics operations
    virtual void getBackendHeartbeat(wavex_backend_heartbeat_t* out) = 0;
    virtual void getPacketStats(wavex_packet_stats_t* out) = 0;

    // Utility operations
    virtual bool isBusy() = 0;
};

}  // namespace Comm
}  // namespace WaveX
