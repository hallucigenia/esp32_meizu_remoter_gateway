/* Non-blocking station recovery. Provisioning remains in wifi.c. */
#include <string.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "wifi.h"
#include "Backoff.h"

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static wifi_diagnostics_t s_stats = {0};
static uint64_t s_retry_at_ms = 0;
static bool s_reassociate_pending = false;
static esp_timer_handle_t s_retry_timer, s_reassociate_timer;
static on_connect s_callback;

bool wifi_connected(){
    taskENTER_CRITICAL(&s_mux);bool connected = s_stats.connected;taskEXIT_CRITICAL(&s_mux);
    return connected;
}
void wifi_get_diagnostics(wifi_diagnostics_t *output){
    uint64_t now = esp_timer_get_time()/1000;
    taskENTER_CRITICAL(&s_mux);*output = s_stats;
    output->retry_in_ms = s_retry_at_ms>now ? s_retry_at_ms-now : 0;taskEXIT_CRITICAL(&s_mux);
    wifi_ps_type_t mode;
    output->power_save_mode=esp_wifi_get_ps(&mode)==ESP_OK ? (int8_t)mode : -1;
    wifi_ap_record_t access_point;
    output->rssi=output->connected && esp_wifi_sta_get_ap_info(&access_point)==ESP_OK ? access_point.rssi : -127;
}
static void schedule_retry(){
    uint32_t wait_ms;
    taskENTER_CRITICAL(&s_mux);
    if(s_stats.connected){taskEXIT_CRITICAL(&s_mux);return;}
    wait_ms = RetryDelay(s_stats.consecutive_failures,1000,30000) + esp_random()%251;
    s_retry_at_ms = esp_timer_get_time()/1000+wait_ms;
    taskEXIT_CRITICAL(&s_mux);
    esp_timer_stop(s_retry_timer);
    esp_err_t error = esp_timer_start_once(s_retry_timer,(uint64_t)wait_ms*1000);
    if(error != ESP_OK) ESP_LOGE("wifi","Retry timer failed: %s",esp_err_to_name(error));
}
static void connect_attempt(void *unused){
    (void)unused;
    taskENTER_CRITICAL(&s_mux);
    if(s_stats.connected){taskEXIT_CRITICAL(&s_mux);return;}
    ++s_stats.reconnect_attempts;s_retry_at_ms = 0;
    taskEXIT_CRITICAL(&s_mux);
    esp_err_t error = esp_wifi_connect();
    if(error != ESP_OK){
        taskENTER_CRITICAL(&s_mux);
        if(s_stats.consecutive_failures<32) ++s_stats.consecutive_failures;
        taskEXIT_CRITICAL(&s_mux);
        ESP_LOGW("wifi","Connect request failed: %s",esp_err_to_name(error));
        schedule_retry();
    }
}
static void station_event(void *arg,esp_event_base_t base,int32_t id,void *data){
    (void)arg;
    if(base == WIFI_EVENT && id == WIFI_EVENT_STA_START){
        connect_attempt(NULL);
    }else if(base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED){
        wifi_event_sta_disconnected_t *event = data;
        taskENTER_CRITICAL(&s_mux);
        s_stats.connected = false;++s_stats.disconnects;
        if(s_stats.consecutive_failures<32) ++s_stats.consecutive_failures;
        s_stats.last_disconnect_reason = event ? event->reason : 0;
        uint32_t count = s_stats.disconnects;uint16_t reason = s_stats.last_disconnect_reason;
        taskEXIT_CRITICAL(&s_mux);
        ESP_LOGW("wifi","Disconnected reason=%u count=%u",reason,count);
        if(s_callback) s_callback(false);
        schedule_retry();
    }else if(base == IP_EVENT && id == IP_EVENT_STA_GOT_IP){
        taskENTER_CRITICAL(&s_mux);
        s_stats.connected = true;s_stats.consecutive_failures = 0;s_retry_at_ms = 0;
        taskEXIT_CRITICAL(&s_mux);
        esp_timer_stop(s_retry_timer);
        ESP_LOGI("wifi","Station connected; retry counter cleared");
        if(s_callback) s_callback(true);
    }
}
static void reassociate(void *unused){
    (void)unused;
    taskENTER_CRITICAL(&s_mux);s_reassociate_pending = false;taskEXIT_CRITICAL(&s_mux);
    // The resulting STA_DISCONNECTED event uses exactly the normal recovery path.
    esp_err_t error = esp_wifi_disconnect();
    if(error != ESP_OK) ESP_LOGW("wifi","Disconnect request failed: %s",esp_err_to_name(error));
}
bool wifi_request_reconnect(){
    taskENTER_CRITICAL(&s_mux);
    bool accepted = s_stats.connected && !s_reassociate_pending;
    if(accepted) s_reassociate_pending = true;
    taskEXIT_CRITICAL(&s_mux);
    if(!accepted) return false;
    if(esp_timer_start_once(s_reassociate_timer,250000) == ESP_OK) return true;
    taskENTER_CRITICAL(&s_mux);s_reassociate_pending = false;taskEXIT_CRITICAL(&s_mux);return false;
}
bool wifi_init_sta(on_connect callback){
    s_callback = callback;
    esp_timer_create_args_t retry = {.callback=connect_attempt,.arg=NULL,.dispatch_method=ESP_TIMER_TASK,.name="wifi_retry"};
    esp_timer_create_args_t reset = {.callback=reassociate,.arg=NULL,.dispatch_method=ESP_TIMER_TASK,.name="wifi_reassociate"};
    if(esp_timer_create(&retry,&s_retry_timer) != ESP_OK || esp_timer_create(&reset,&s_reassociate_timer) != ESP_OK) return false;
    if(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,station_event,NULL) != ESP_OK ||
       esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,station_event,NULL) != ESP_OK) return false;
    if(esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return false;
    if(esp_wifi_start()!=ESP_OK) return false;
    // This USB-powered gateway needs predictable receive latency, including
    // recovery mode without BLE. Avoid AP/modem-sleep buffering interactions.
    if(esp_wifi_set_ps(WIFI_PS_NONE)!=ESP_OK) ESP_LOGW("wifi","Could not disable Wi-Fi power save");
    return true;
}
