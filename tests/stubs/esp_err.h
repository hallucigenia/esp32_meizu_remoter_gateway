#pragma once
#ifdef __cplusplus
extern "C" {
#endif
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 0x103
static inline const char *esp_err_to_name(esp_err_t error){(void)error;return "host-error";}
#ifdef __cplusplus
}
#endif
