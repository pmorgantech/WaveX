#ifndef ESP_LOG_H
#define ESP_LOG_H

#include <cstdarg>
#include <cstdio>

// Mock ESP-IDF logging macros
#define ESP_LOGE(tag, format, ...) printf("[E][%s] " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) printf("[W][%s] " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) printf("[I][%s] " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, format, ...) printf("[D][%s] " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGV(tag, format, ...) printf("[V][%s] " format "\n", tag, ##__VA_ARGS__)

// Mock log level
typedef enum {
    ESP_LOG_NONE = 0,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE
} esp_log_level_t;

// Mock logging functions
void esp_log_level_set(const char* tag, esp_log_level_t level);
esp_log_level_t esp_log_level_get(const char* tag);

#endif  // ESP_LOG_H
