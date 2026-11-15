#include "wifi.h"
#include <esp_event.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <list>
#include <cassert>
#include <cstdio>
struct esp_timer {esp_timer_create_args_t args;uint64_t at=0;bool active=false;};
static std::list<esp_timer> timers;
static uint64_t now_us=0;
static unsigned connects=0,disconnects=0,connected_callbacks=0,disconnected_callbacks=0;
static esp_event_handler_t handler;
static bool fail_connect=false;
static wifi_ps_type_t power_save=WIFI_PS_MIN_MODEM;
esp_event_base_t WIFI_EVENT="wifi",IP_EVENT="ip";
extern "C" {
int64_t esp_timer_get_time(){return now_us;}
uint32_t esp_random(){return 0;}
esp_err_t esp_timer_create(const esp_timer_create_args_t *args,esp_timer_handle_t *out){
    timers.emplace_back();timers.back().args=*args;*out=&timers.back();return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t timer,uint64_t delay){
    if(timer->active)return ESP_ERR_INVALID_STATE;
    timer->at=now_us+delay;timer->active=true;return ESP_OK;
}
esp_err_t esp_timer_stop(esp_timer_handle_t timer){timer->active=false;return ESP_OK;}
esp_err_t esp_event_handler_register(esp_event_base_t,int32_t,esp_event_handler_t value,void *){handler=value;return ESP_OK;}
esp_err_t esp_wifi_set_mode(int){return ESP_OK;}
esp_err_t esp_wifi_start(){return ESP_OK;}
esp_err_t esp_wifi_set_ps(wifi_ps_type_t value){power_save=value;return ESP_OK;}
esp_err_t esp_wifi_get_ps(wifi_ps_type_t *value){*value=power_save;return ESP_OK;}
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *value){value->rssi=-61;return ESP_OK;}
esp_err_t esp_wifi_connect(){++connects;return fail_connect?ESP_FAIL:ESP_OK;}
esp_err_t esp_wifi_disconnect(){
    ++disconnects;wifi_event_sta_disconnected_t event={8};handler(nullptr,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&event);return ESP_OK;
}
}
static void Advance(uint64_t milliseconds){
    const uint64_t end=now_us+milliseconds*1000;
    for(;;){
        esp_timer *next=nullptr;
        for(auto &timer:timers)if(timer.active&&timer.at<=end&&(!next||timer.at<next->at))next=&timer;
        if(!next)break;
        now_us=next->at;next->active=false;next->args.callback(next->args.arg);
    }
    now_us=end;
}
static void Callback(bool connected){if(connected)++connected_callbacks;else ++disconnected_callbacks;}
static void Lost(uint8_t reason){wifi_event_sta_disconnected_t event={reason};handler(nullptr,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&event);}
int main(){
    assert(wifi_init_sta(Callback));handler(nullptr,WIFI_EVENT,WIFI_EVENT_STA_START,nullptr);assert(connects==1);
    uint64_t before=now_us;Lost(201);assert(now_us==before); // Handler never sleeps.
    wifi_diagnostics_t info;wifi_get_diagnostics(&info);
    assert(!info.connected&&info.disconnects==1&&info.last_disconnect_reason==201&&info.retry_in_ms==1000);
    Advance(999);assert(connects==1);Advance(1);assert(connects==2);
    handler(nullptr,IP_EVENT,IP_EVENT_STA_GOT_IP,nullptr);wifi_get_diagnostics(&info);
    assert(info.connected&&info.consecutive_failures==0&&info.retry_in_ms==0);
    assert(info.power_save_mode==WIFI_PS_NONE&&info.rssi==-61);
    unsigned count=connects;Advance(60000);assert(connects==count);
    for(int n=0;n<40;++n){
        Lost(200);wifi_get_diagnostics(&info);assert(info.retry_in_ms>0&&info.retry_in_ms<=30000);Advance(info.retry_in_ms);
    }
    assert(connects==count+40); // Recovery never turns into a reboot loop.
    fail_connect=true;Lost(201);Advance(30000);wifi_get_diagnostics(&info);assert(info.retry_in_ms>0);
    fail_connect=false;handler(nullptr,IP_EVENT,IP_EVENT_STA_GOT_IP,nullptr);
    assert(wifi_request_reconnect()&&!wifi_request_reconnect());Advance(249);assert(disconnects==0);
    Advance(1);assert(disconnects==1&&!wifi_connected());wifi_get_diagnostics(&info);assert(info.last_disconnect_reason==8);
    Advance(1000);handler(nullptr,IP_EVENT,IP_EVENT_STA_GOT_IP,nullptr);
    assert(wifi_connected()&&connected_callbacks==3&&disconnected_callbacks>40);
    puts("Non-blocking Wi-Fi retry, backoff cap, diagnostics, and manual reassociation passed");
}
