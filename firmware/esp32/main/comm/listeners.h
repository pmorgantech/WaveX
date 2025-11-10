#pragma once

#include <stddef.h>
#include <stdint.h>

// Callback function types
typedef void (*wavex_meter_cb_t)(float rms, float peak, void* user_data);
typedef void (*wavex_wave_chunk_cb_t)(uint32_t offset,
                                      const int16_t* samples,
                                      uint16_t count,
                                      void* user_data);
typedef void (*wavex_browse_resp_cb_t)(const uint8_t* data, size_t length, void* user_data);
typedef void (*wavex_sample_status_cb_t)(
    uint8_t state, uint32_t sample_rate, uint8_t channels, uint32_t frames_played, void* user_data);

// Listeners manager that handles callback registration and invocation
class ListenersManager {
   public:
    ListenersManager();
    ~ListenersManager() = default;

    // Set listeners
    void set_meter_listener(wavex_meter_cb_t cb, void* user_data);
    void set_wave_chunk_listener(wavex_wave_chunk_cb_t cb, void* user_data);
    void set_browse_resp_listener(wavex_browse_resp_cb_t cb, void* user_data);
    void set_sample_status_listener(wavex_sample_status_cb_t cb, void* user_data);

    // Invoke listeners
    void invoke_meter_callback(float rms, float peak);
    void invoke_wave_chunk_callback(uint32_t offset, const int16_t* samples, uint16_t count);
    void invoke_browse_resp_callback(const uint8_t* data, size_t length);
    void invoke_sample_status_callback(uint8_t state,
                                       uint32_t sample_rate,
                                       uint8_t channels,
                                       uint32_t frames_played);

    // Check if listeners are set
    bool has_meter_listener() const { return m_meter_cb != NULL; }
    bool has_wave_chunk_listener() const { return m_wave_cb != NULL; }
    bool has_browse_resp_listener() const { return m_browse_resp_cb != NULL; }
    bool has_sample_status_listener() const { return m_sample_status_cb != NULL; }

   private:
    // Callback functions and user data
    wavex_meter_cb_t m_meter_cb;
    wavex_wave_chunk_cb_t m_wave_cb;
    wavex_browse_resp_cb_t m_browse_resp_cb;
    wavex_sample_status_cb_t m_sample_status_cb;
    void* m_meter_ud;
    void* m_wave_ud;
    void* m_browse_resp_ud;
    void* m_sample_status_ud;
};
