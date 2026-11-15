#pragma once
#include "esp_err.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {uint8_t reason;} wifi_event_sta_disconnected_t;
#define WIFI_MODE_STA 1
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_set_mode(int);
esp_err_t esp_wifi_start(void);
#ifdef __cplusplus
}
#endif
