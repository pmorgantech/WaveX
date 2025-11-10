#ifndef ESP_LVGL_PORT_H
#define ESP_LVGL_PORT_H

// Mock ESP-LVGL port header for unit testing

#ifdef __cplusplus
extern "C" {
#endif

void esp_lvgl_port_lock(void);
void esp_lvgl_port_unlock(void);

#ifdef __cplusplus
}
#endif

#endif  // ESP_LVGL_PORT_H
