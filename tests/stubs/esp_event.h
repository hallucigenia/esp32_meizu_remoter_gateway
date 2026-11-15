#pragma once
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef const char *esp_event_base_t;
extern esp_event_base_t WIFI_EVENT,IP_EVENT;
#define ESP_EVENT_ANY_ID -1
#define WIFI_EVENT_STA_START 1
#define WIFI_EVENT_STA_DISCONNECTED 2
#define IP_EVENT_STA_GOT_IP 3
typedef void (*esp_event_handler_t)(void *,esp_event_base_t,int32_t,void *);
esp_err_t esp_event_handler_register(esp_event_base_t,int32_t,esp_event_handler_t,void *);
#ifdef __cplusplus
}
#endif
