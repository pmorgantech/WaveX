#include "listeners.h"

ListenersManager::ListenersManager()
    : m_meter_cb(NULL)
    , m_wave_cb(NULL)
    , m_browse_resp_cb(NULL)
    , m_sample_status_cb(NULL)
    , m_meter_ud(NULL)
    , m_wave_ud(NULL)
    , m_browse_resp_ud(NULL)
    , m_sample_status_ud(NULL)
{
}

void ListenersManager::set_meter_listener(wavex_meter_cb_t cb, void* user_data)
{
    m_meter_cb = cb;
    m_meter_ud = user_data;
}

void ListenersManager::set_wave_chunk_listener(wavex_wave_chunk_cb_t cb, void* user_data)
{
    m_wave_cb = cb;
    m_wave_ud = user_data;
}

void ListenersManager::set_browse_resp_listener(wavex_browse_resp_cb_t cb, void* user_data)
{
    m_browse_resp_cb = cb;
    m_browse_resp_ud = user_data;
}

void ListenersManager::set_sample_status_listener(wavex_sample_status_cb_t cb, void* user_data)
{
    m_sample_status_cb = cb;
    m_sample_status_ud = user_data;
}

void ListenersManager::invoke_meter_callback(float rms, float peak)
{
    if (m_meter_cb) {
        m_meter_cb(rms, peak, m_meter_ud);
    }
}

void ListenersManager::invoke_wave_chunk_callback(uint32_t offset, const int16_t* samples, uint16_t count)
{
    if (m_wave_cb) {
        m_wave_cb(offset, samples, count, m_wave_ud);
    }
}

void ListenersManager::invoke_browse_resp_callback(const uint8_t* data, size_t length)
{
    if (m_browse_resp_cb) {
        m_browse_resp_cb(data, length, m_browse_resp_ud);
    }
}

void ListenersManager::invoke_sample_status_callback(uint8_t state, uint32_t sample_rate, uint8_t channels, uint32_t frames_played)
{
    if (m_sample_status_cb) {
        m_sample_status_cb(state, sample_rate, channels, frames_played, m_sample_status_ud);
    }
}
