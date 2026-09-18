#include "pot_store.h"

#include "nvs.h"
#include "nvs_flash.h"
namespace wavex_panel {
esp_err_t LoadPotCalibration(WaveX::Panel::Calibration& values) {
    // Never erase NVS automatically on init/version/full errors.
    auto result = nvs_flash_init();
    if (result != ESP_OK)
        return result;
    nvs_handle_t handle;
    result = nvs_open("wavex_panel", NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND)
        return ESP_OK;
    if (result != ESP_OK)
        return result;
    uint8_t bytes[WaveX::Panel::kCalibrationBytes];
    size_t length = sizeof(bytes);
    result = nvs_get_blob(handle, "pots_v1", bytes, &length);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND)
        return ESP_OK;
    if (result == ESP_OK && !WaveX::Panel::DecodeCalibration(bytes, length, values))
        return ESP_ERR_INVALID_RESPONSE;
    return result;
}
esp_err_t SavePotCalibration(const WaveX::Panel::Calibration& values) {
    for (const auto& value: values)
        if (!value.Valid())
            return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    auto result = nvs_open("wavex_panel", NVS_READWRITE, &handle);
    if (result != ESP_OK)
        return result;
    const auto bytes = WaveX::Panel::EncodeCalibration(values);
    result = nvs_set_blob(handle, "pots_v1", bytes.data(), bytes.size());
    if (result == ESP_OK)
        result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}
}  // namespace wavex_panel
