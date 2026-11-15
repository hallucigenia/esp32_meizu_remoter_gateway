#pragma once
#include "esp_err.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {uint8_t reason;} wifi_event_sta_disconnected_t;
typedef enum {WIFI_PS_NONE=0,WIFI_PS_MIN_MODEM=1,WIFI_PS_MAX_MODEM=2} wifi_ps_type_t;
typedef struct {int8_t rssi;} wifi_ap_record_t;
#define WIFI_MODE_STA 1
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_set_mode(int);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_set_ps(wifi_ps_type_t);
esp_err_t esp_wifi_get_ps(wifi_ps_type_t *);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *);
#ifdef __cplusplus
}
#endif
