#ifndef ESP_APP_DESC_H
#define ESP_APP_DESC_H

// Mock esp_app_desc for host tests (wavex_application.cpp logs version info).

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char version[32];
    char project_name[32];
    char time[16];
    char date[16];
    char idf_ver[32];
} esp_app_desc_t;

const esp_app_desc_t* esp_app_get_description(void);

#ifdef __cplusplus
}
#endif

#endif  // ESP_APP_DESC_H
