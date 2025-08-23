#include "listeners.h"

ListenersManager::ListenersManager()
    : m_meter_cb(NULL)
    , m_wave_cb(NULL)
    , m_meter_ud(NULL)
    , m_wave_ud(NULL)
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
