#include "OtaManager.h"
#include "Kernel.h"
#include "SocketClient.h"
#include "mdnsmd.h"
#include "wifi.h"
#include <esp_crt_bundle.h>
#include <esp_https_ota.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <nvs.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace {
const char *TAG = "OTA";
uint64_t Now() { return esp_timer_get_time()/1000; }
std::string Hex(const uint8_t *data, size_t size) {
    std::string out(size*2,'0');
    const char *digits="0123456789abcdef";
    for(size_t i=0;i<size;++i){out[i*2]=digits[data[i]>>4];out[i*2+1]=digits[data[i]&15];}
    return out;
}
std::string ReadString(nvs_handle_t handle,const char *key) {
    size_t size=0;
    if(nvs_get_str(handle,key,nullptr,&size)!=ESP_OK || size>256) return {};
    std::string result(size,'\0');
    if(nvs_get_str(handle,key,&result[0],&size)!=ESP_OK) return {};
    result.resize(size-1);return result;
}
std::string String(cJSON *data,const char *key) {
    auto *item=cJSON_GetObjectItemCaseSensitive(data,key);
    return cJSON_IsString(item) ? item->valuestring : "";
}
uint64_t Number(cJSON *data,const char *key) {
    auto *item=cJSON_GetObjectItemCaseSensitive(data,key);
    if(!cJSON_IsNumber(item) || item->valuedouble<0 || item->valuedouble>4102444800.0 ||
       item->valuedouble!=static_cast<uint64_t>(item->valuedouble)) return 0;
    return static_cast<uint64_t>(item->valuedouble);
}
cJSON *Result(bool accepted,const char *status) {
    auto *result=cJSON_CreateObject();
    cJSON_AddBoolToObject(result,"accepted",accepted);cJSON_AddStringToObject(result,"status",status);
    return result;
}
}

bool OtaManager::Layout() const {
    // Accept only the measured 4 MB layout; never guess offsets or change it over OTA.
    const auto *a=esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_0,nullptr);
    const auto *b=esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_1,nullptr);
    const auto *n=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_NVS,nullptr);
    const auto *o=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_OTA,nullptr);
    return a && b && n && o && a->address==0x10000 && b->address==0x1d0000 &&
           a->size==meizu_ota::SLOT_SIZE && b->size==meizu_ota::SLOT_SIZE &&
           n->address==0x9000 && n->size==0x5000 && o->address==0xe000 && o->size==0x2000;
}
bool OtaManager::Init() {
    boot_ms_=Now();
    auto *running=esp_ota_get_running_partition();
    esp_ota_get_state_partition(running,&initial_state_);
    nvs_handle_t handle;
    if(nvs_open("meizu_ota",NVS_READWRITE,&handle)!=ESP_OK) return false;
    size_t key_size=sizeof key_;
    esp_err_t key_error=nvs_get_blob(handle,"key",key_,&key_size);
    if(key_error==ESP_ERR_NVS_NOT_FOUND){
        esp_fill_random(key_,sizeof key_);
        paired_=nvs_set_blob(handle,"key",key_,sizeof key_)==ESP_OK;
    }else paired_=key_error==ESP_OK && key_size==sizeof key_;
    uint8_t boots=0,seen=0;
    nvs_get_u8(handle,"boots",&boots);nvs_get_u8(handle,"rollback_seen",&seen);
    recovery_=boots>=3;rollback_seen_=seen || initial_state_==ESP_OTA_IMG_PENDING_VERIFY;
    nvs_set_u8(handle,"boots",boots<255 ? boots+1 : 255);
    if(rollback_seen_) nvs_set_u8(handle,"rollback_seen",1);
    image_.id=ReadString(handle,"id");image_.version=ReadString(handle,"version");
    image_.sha256=ReadString(handle,"sha256");
    std::string saved=ReadString(handle,"phase");
    uint32_t size=0;nvs_get_u32(handle,"size",&size);image_.size=size;
    nvs_get_u32(handle,"target",&target_address_);
    if(saved=="rebooting" || saved=="complete") written_=size;
    if(!image_.id.empty()){
        if(saved=="rebooting" && running && running->address==target_address_ &&
           image_.version==esp_ota_get_app_description()->version &&
           (initial_state_==ESP_OTA_IMG_PENDING_VERIFY || (initial_state_==ESP_OTA_IMG_VALID && rollback_seen_))) phase_="verifying_boot";
        else if(saved=="rebooting") {phase_="failed";error_="rolled_back_or_bootloader_unsupported";}
        else if(saved=="complete" && image_.version==esp_ota_get_app_description()->version) phase_="complete";
        else if(saved=="downloading") {phase_="failed";error_="interrupted";}
        else if(saved=="failed" || saved=="cancelled") {
            phase_=saved;error_=ReadString(handle,"error");
            if(error_.empty()) error_="previous_update_failed";
        }
    }
    bool committed=nvs_commit(handle)==ESP_OK;nvs_close(handle);
    paired_=paired_ && committed;
    if(recovery_ && esp_wifi_set_ps(WIFI_PS_NONE)!=ESP_OK) return false;
    fcntl(STDIN_FILENO,F_SETFL,fcntl(STDIN_FILENO,F_GETFL,0)|O_NONBLOCK);
    ESP_LOGI(TAG,"OTA ready; recovery=%d rollback_seen=%d. USB command: ota-key",recovery_,rollback_seen_);
    return paired_;
}
void OtaManager::RejectPending() {
    esp_ota_img_states_t state=ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(esp_ota_get_running_partition(),&state);
    if(state==ESP_OTA_IMG_PENDING_VERIFY){
        ESP_LOGE(TAG,"Startup failed; returning to previous application");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}
bool OtaManager::Persist(const char *phase) {
    nvs_handle_t handle;
    if(nvs_open("meizu_ota",NVS_READWRITE,&handle)!=ESP_OK) return false;
    bool ok=nvs_set_str(handle,"id",image_.id.c_str())==ESP_OK &&
        nvs_set_str(handle,"version",image_.version.c_str())==ESP_OK &&
        nvs_set_str(handle,"sha256",image_.sha256.c_str())==ESP_OK &&
        nvs_set_str(handle,"error",error_.c_str())==ESP_OK &&
        nvs_set_u32(handle,"size",image_.size)==ESP_OK &&
        nvs_set_u32(handle,"target",target_address_)==ESP_OK &&
        nvs_set_str(handle,"phase",phase)==ESP_OK && nvs_commit(handle)==ESP_OK;
    nvs_close(handle);return ok;
}
void OtaManager::Tick(bool kernel_ready) {
    if(confirmed_ || active_) return;
    if(!kernel_ready){healthy_since_=0;RejectPending();return;}
    // Local service health; no dependency on an individual remote being online.
    if(!wifi_connected()){
        healthy_since_=0;
        if(initial_state_==ESP_OTA_IMG_PENDING_VERIFY && Now()-boot_ms_>120000) RejectPending();
        return;
    }
    if(!healthy_since_) healthy_since_=Now();
    if(Now()-healthy_since_<60000) return;
    if(initial_state_==ESP_OTA_IMG_NEW){error_="bootloader_rollback_unsupported";return;}
    if(esp_ota_mark_app_valid_cancel_rollback()!=ESP_OK) return;
    nvs_handle_t handle;
    if(nvs_open("meizu_ota",NVS_READWRITE,&handle)!=ESP_OK) return;
    bool ok=nvs_set_u8(handle,"boots",0)==ESP_OK && nvs_commit(handle)==ESP_OK;nvs_close(handle);
    if(!ok) return;
    lock_.Lock();
    if(phase_=="verifying_boot") {
        if(!Persist("complete")){lock_.Unlock();return;}
        phase_="complete";
    }
    confirmed_=true;
    lock_.Unlock();ESP_LOGI(TAG,"Boot confirmed after stable service window");
}
void OtaManager::Console() {
    static char command[40];static size_t used=0;
    char c;
    while(read(STDIN_FILENO,&c,1)==1){
        if(c=='\r' || c=='\n'){
            command[used]=0;
            if(strcmp(command,"ota-key")==0 && paired_) printf("OTA_KEY=%s\n",Hex(key_,sizeof key_).c_str());
            used=0;
        }else if(used<sizeof(command)-1) command[used++]=c;
        else used=0;
    }
}
cJSON *OtaManager::Status() {
    auto *data=cJSON_CreateObject();lock_.Lock();
    cJSON_AddStringToObject(data,"phase",phase_.c_str());cJSON_AddStringToObject(data,"id",image_.id.c_str());
    cJSON_AddStringToObject(data,"version",image_.version.c_str());cJSON_AddStringToObject(data,"sha256",image_.sha256.c_str());
    cJSON_AddStringToObject(data,"error",error_.c_str());cJSON_AddNumberToObject(data,"size",image_.size);
    cJSON_AddNumberToObject(data,"written",written_);lock_.Unlock();
    cJSON_AddBoolToObject(data,"active",active_);cJSON_AddBoolToObject(data,"confirmed",confirmed_);
    cJSON_AddBoolToObject(data,"bluetooth_paused",radio_stopped_);
    cJSON_AddBoolToObject(data,"safe_mode",recovery_);cJSON_AddBoolToObject(data,"paired",paired_);
    cJSON_AddBoolToObject(data,"rollback_verified",rollback_seen_);cJSON_AddBoolToObject(data,"layout_valid",Layout());
    cJSON_AddStringToObject(data,"board",meizu_ota::BOARD);
    cJSON_AddStringToObject(data,"running_version",esp_ota_get_app_description()->version);
    auto *running=esp_ota_get_running_partition();esp_ota_img_states_t state=ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(running,&state);
    cJSON_AddStringToObject(data,"slot",running ? running->label : "unknown");
    cJSON_AddNumberToObject(data,"boot_state",state);cJSON_AddNumberToObject(data,"max_size",meizu_ota::SLOT_SIZE);
    return data;
}
bool OtaManager::Authenticate(SocketClient *client,const char *operation,const meizu_ota::Image &image,const char *auth) {
    if(!paired_ || nonce_.empty() || nonce_owner_!=client->Id() || Now()>nonce_deadline_ ||
       !auth || !meizu_ota::Hex(auth,64)) return false;
    std::string message=meizu_ota::Authorization(operation,mdns_get_serial(),nonce_,image);
    uint8_t digest[32];
    auto *md=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    bool ok=mbedtls_md_hmac(md,key_,sizeof key_,reinterpret_cast<const uint8_t *>(message.data()),message.size(),digest)==0;
    std::string expected=Hex(digest,sizeof digest);uint8_t difference=0;
    for(size_t i=0;i<64;++i) difference|=expected[i]^auth[i];
    nonce_.clear();return ok && difference==0;
}
cJSON *OtaManager::Handle(SocketClient *client,const char *type,cJSON *data) {
    if(strcmp(type,"ota_status")==0) return Status();
    if(strcmp(type,"ota_challenge")==0){
        uint8_t nonce[32];esp_fill_random(nonce,sizeof nonce);lock_.Lock();
        nonce_=Hex(nonce,sizeof nonce);nonce_owner_=client->Id();nonce_deadline_=Now()+60000;
        auto *result=cJSON_CreateObject();cJSON_AddStringToObject(result,"nonce",nonce_.c_str());
        cJSON_AddBoolToObject(result,"paired",paired_);lock_.Unlock();return result;
    }
    lock_.Lock();
    meizu_ota::Image image;
    image.id=String(data,"id");image.version=String(data,"version");image.url=String(data,"url");
    image.sha256=String(data,"sha256");image.size=Number(data,"size");image.timestamp=Number(data,"ts");
    std::string auth=String(data,"auth");
    bool cancel=strcmp(type,"ota_cancel")==0;
    bool check=strcmp(type,"ota_auth")==0;
    if(!Authenticate(client,check ? "check" : cancel ? "cancel" : "start",image,auth.c_str())){
        lock_.Unlock();return Result(false,"authentication_failed");
    }
    if(check){lock_.Unlock();return Result(true,"authenticated");}
    if(cancel){
        bool match=active_ && image_.id==image.id && phase_!="rebooting";
        if(match) cancelled_=true;
        lock_.Unlock();return Result(match,match ? "cancelling" : "not_cancellable");
    }
    if(active_){
        bool same=image_.id==image.id && image_.sha256==image.sha256 && image_.version==image.version;
        lock_.Unlock();return Result(same,same ? "already_running" : "update_busy");
    }
    if(image_.id==image.id){
        bool same=image_.sha256==image.sha256 && image_.version==image.version && phase_=="complete";
        lock_.Unlock();return Result(same,same ? "already_complete" : "request_id_used");
    }
    const auto *partition=esp_ota_get_next_update_partition(nullptr);
    const char *invalid=meizu_ota::Validate(image,partition ? partition->size : 0);
    if(!Layout()) invalid="unsupported_layout";
    if(!confirmed_) invalid="boot_not_confirmed";
    if(invalid){lock_.Unlock();return Result(false,invalid);}
    if(!kernel_.PauseOTA(true)){lock_.Unlock();return Result(false,"ble_busy");}
    image_=image;target_address_=partition->address;phase_="downloading";error_.clear();written_=0;cancelled_=false;
    if(!Persist("downloading")){kernel_.PauseOTA(false);lock_.Unlock();return Result(false,"storage_failed");}
    active_=true;
    bool started=xTaskCreate(Worker,"ota_worker",10240,this,tskIDLE_PRIORITY+1,nullptr)==pdPASS;
    if(!started){active_=false;phase_="failed";error_="no_memory";Persist("failed");kernel_.PauseOTA(false);}
    lock_.Unlock();return Result(started,started ? "accepted" : "no_memory");
}
void OtaManager::Fail(const char *reason) {
    lock_.Lock();phase_=cancelled_ ? "cancelled" : "failed";error_=cancelled_ ? "cancelled" : reason;
    Persist(phase_.c_str());lock_.Unlock();
    active_=false;ESP_LOGW(TAG,"Update stopped: %s",reason);
    if(radio_stopped_){
        // Recreate all GATT registrations from a clean boot rather than trying
        // to reuse handles owned by the stopped Bluedroid stack. Boot selection
        // has not changed on download/identity/hash/cancel failures.
        ESP_LOGI(TAG,"Restarting unchanged application to restore Bluetooth");
        vTaskDelay(pdMS_TO_TICKS(1500));esp_restart();
    }
    kernel_.PauseOTA(false);
}
void OtaManager::Worker(void *context) {
    static_cast<OtaManager *>(context)->Download();vTaskDelete(nullptr);
}
void OtaManager::Download() {
    uint64_t started=Now();
    while(!kernel_.OTAIdle()){
        if(cancelled_ || Now()-started>20000){Fail("ble_busy");return;}
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if(cancelled_){Fail("cancelled");return;}
    if(!recovery_){
        radio_stopped_=true;
        if((esp_bluedroid_get_status()==ESP_BLUEDROID_STATUS_ENABLED && esp_bluedroid_disable()!=ESP_OK) ||
           (esp_bluedroid_get_status()==ESP_BLUEDROID_STATUS_INITIALIZED && esp_bluedroid_deinit()!=ESP_OK) ||
           (esp_bt_controller_get_status()==ESP_BT_CONTROLLER_STATUS_ENABLED && esp_bt_controller_disable()!=ESP_OK) ||
           (esp_bt_controller_get_status()==ESP_BT_CONTROLLER_STATUS_INITED && esp_bt_controller_deinit()!=ESP_OK)){
            Fail("bluetooth_stop_failed");return;
        }
    }
    if(esp_wifi_set_ps(WIFI_PS_NONE)!=ESP_OK){Fail("wifi_mode_failed");return;}
    // The timestamp is covered by the per-device HMAC; TLS certificate dates
    // can be validated even on a device without a battery-backed clock.
    timeval clock={static_cast<time_t>(image_.timestamp),0};settimeofday(&clock,nullptr);
    esp_http_client_config_t http={};
    http.url=image_.url.c_str();http.crt_bundle_attach=esp_crt_bundle_attach;
    http.timeout_ms=15000;http.buffer_size=4096;http.buffer_size_tx=1024;
    http.keep_alive_enable=true;
    esp_https_ota_config_t config={};config.http_config=&http;
    esp_https_ota_handle_t handle=nullptr;
    auto *target=esp_ota_get_next_update_partition(nullptr);
    esp_err_t err=esp_https_ota_begin(&config,&handle);
    if(err!=ESP_OK){Fail("download_connect_failed");return;}
    esp_app_desc_t description={};
    err=esp_https_ota_get_img_desc(handle,&description);
    if(err!=ESP_OK || strncmp(description.project_name,"esp32_meizu_remoter_gateway",sizeof description.project_name)!=0 ||
       strncmp(description.version,image_.version.c_str(),sizeof description.version)!=0){
        esp_https_ota_abort(handle);Fail("image_identity_mismatch");return;
    }
    do {
        if(cancelled_ || Now()-started>300000){esp_https_ota_abort(handle);Fail(cancelled_ ? "cancelled" : "download_timeout");return;}
        err=esp_https_ota_perform(handle);
        int count=esp_https_ota_get_image_len_read(handle);
        lock_.Lock();written_=count>0 ? count : 0;lock_.Unlock();
        if(count>static_cast<int>(image_.size)){esp_https_ota_abort(handle);Fail("size_mismatch");return;}
        vTaskDelay(1);
    }while(err==ESP_ERR_HTTPS_OTA_IN_PROGRESS);
    if(err!=ESP_OK || !esp_https_ota_is_complete_data_received(handle) || written_!=image_.size){
        esp_https_ota_abort(handle);Fail("download_incomplete");return;
    }
    lock_.Lock();phase_="checking";lock_.Unlock();
    // Hash exactly the published file bytes. esp_partition_get_sha256() hashes
    // an app image differently and must not be compared to the release-file SHA.
    mbedtls_sha256_context hash;mbedtls_sha256_init(&hash);mbedtls_sha256_starts_ret(&hash,0);
    uint8_t buffer[4096],digest[32];bool read_ok=true;
    for(size_t offset=0;offset<image_.size;){
        size_t count=std::min(sizeof buffer,image_.size-offset);
        if(cancelled_ || esp_partition_read(target,offset,buffer,count)!=ESP_OK){read_ok=false;break;}
        mbedtls_sha256_update_ret(&hash,buffer,count);offset+=count;vTaskDelay(1);
    }
    mbedtls_sha256_finish_ret(&hash,digest);mbedtls_sha256_free(&hash);
    if(!read_ok || Hex(digest,sizeof digest)!=image_.sha256){esp_https_ota_abort(handle);Fail("sha256_mismatch");return;}
    // Commit transaction metadata before the boot selector, so a lost result or
    // power cut can be reconciled after reconnect. This does not alter bindings.
    lock_.Lock();
    bool was_cancelled=cancelled_;
    if(!was_cancelled) phase_="rebooting";
    bool saved=!was_cancelled && Persist("rebooting");lock_.Unlock();
    if(was_cancelled){esp_https_ota_abort(handle);Fail("cancelled");return;}
    if(!saved){esp_https_ota_abort(handle);Fail("storage_failed");return;}
    err=esp_https_ota_finish(handle);
    // IDF 4.3.7 finish() can mask a boot-selector error; verify the actual value.
    if(err!=ESP_OK || esp_ota_get_boot_partition()!=target){Fail("commit_failed");return;}
    lock_.Lock();phase_="rebooting";lock_.Unlock();
    ESP_LOGI(TAG,"Image verified; rebooting to %s",target->label);
    vTaskDelay(pdMS_TO_TICKS(1500));esp_restart();
}
