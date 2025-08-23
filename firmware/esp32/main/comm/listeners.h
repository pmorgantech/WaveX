#pragma once

#include <stdint.h>
#include <stddef.h>

// Callback function types
typedef void (*wavex_meter_cb_t)(float rms, float peak, void* user_data);
typedef void (*wavex_wave_chunk_cb_t)(uint32_t offset, const int16_t* samples, uint16_t count, void* user_data);

// Listeners manager that handles callback registration and invocation
class ListenersManager {
public:
    ListenersManager();
    ~ListenersManager() = default;
    
    // Set listeners
    void set_meter_listener(wavex_meter_cb_t cb, void* user_data);
    void set_wave_chunk_listener(wavex_wave_chunk_cb_t cb, void* user_data);
    
    // Invoke listeners
    void invoke_meter_callback(float rms, float peak);
    void invoke_wave_chunk_callback(uint32_t offset, const int16_t* samples, uint16_t count);
    
    // Check if listeners are set
    bool has_meter_listener() const { return m_meter_cb != NULL; }
    bool has_wave_chunk_listener() const { return m_wave_cb != NULL; }

private:
    // Callback functions and user data
    wavex_meter_cb_t m_meter_cb;
    wavex_wave_chunk_cb_t m_wave_cb;
    void* m_meter_ud;
    void* m_wave_ud;
};
