/* Created by @georgezhao2010; maintained by this fork. */
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <string.h>
#include <ctype.h>
#include <limits>
#include "led.h"
#include "utils.h"
#include "wifi.h"
#include "nvsstorage.h"
#include "mdnsmd.h"
#include "Kernel.h"
#include "OtaManager.h"
#define LOG_TAG "Kernel"

Kernel::Kernel() {}
void Kernel::task_Restart(void *){
    led_set_mode(LED_MODE_FAST_BLINK,0);
    delay(10000);
    esp_restart();
}

bool Kernel::ValidAddress(const char *address){
    if(!address || strlen(address) != 17) return false;
    for(size_t i=0;i<17;++i)
        if(i%3 == 2 ? address[i] != ':' : !isxdigit(static_cast<unsigned char>(address[i]))) return false;
    return true;
}
char *Kernel::Reply(const char *type,cJSON *data){
    if(!data) return nullptr;
    cJSON *root = cJSON_CreateObject();
    if(!root){cJSON_Delete(data);return nullptr;}
    cJSON_AddStringToObject(root,"type",type);
    cJSON_AddItemToObject(root,"data",data);
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text;
}
void Kernel::Wake(){if(m_worker) xTaskNotifyGive(m_worker);}

bool Kernel::Init(bool recovery){
    m_recovery=recovery;
    gpio_set_direction(GPIO_NUM_0,GPIO_MODE_INPUT);
    int8_t threshold;
    if(!nvs_read_interval(&m_nUpdateInterval) || m_nUpdateInterval < 1 || m_nUpdateInterval > 30)
        m_nUpdateInterval = CONFIG_DEFAULT_UPDATE_INTERVAL;
    uint8_t count = 0;
    if(nvs_read_addrcount(&count) && count){
        std::vector<uint8_t> addresses(count * ESP_BD_ADDR_LEN);
        size_t length = addresses.size();
        if(nvs_read_addrs(addresses.data(),&length) && length == addresses.size()){
            for(size_t i=0;i<count;++i){
                MEIZURemoterInfo info;
                info.m_addr = BLEAddress(addresses.data()+i*ESP_BD_ADDR_LEN);
                info.m_generation = ++m_generation;
                m_mapInfos.emplace(info.m_addr,info);
                ESP_LOGI(LOG_TAG,"Loaded binding %s",info.m_addr.toString().c_str());
            }
        }else ESP_LOGE(LOG_TAG,"Invalid stored binding list");
    }
    m_pRemoterInterface = new MEIZURemoter();
    // TCP becomes usable before the first (possibly offline) BLE probe.
    m_server.SetCallback(this,OnClientMessage);
    if(!m_server.Listen(CONFIG_SOCKET_LISTEN_PORT)) return false;
    ESP_LOGI(LOG_TAG,"TCP ready before BLE probes");
    if(recovery){ESP_LOGW(LOG_TAG,"Recovery mode: networking and OTA only");return true;}
    if(!GATTDevice::Init(this,OnBindModeChanged,OnDeviceFound,OnRSSIRead)) return false;
    if(nvs_read_bindthrld(&threshold)) GATTDevice::SetBindThreshold(threshold);
    return xTaskCreate(Worker,"ble_worker",8192,this,tskIDLE_PRIORITY+1,&m_worker) == pdPASS;
}

void Kernel::Loop(){
    uint16_t counter = 0;
    while(true){
        delay(200);
        esp_task_wdt_reset();
        ota.Console();ota.Tick(Ready());
        if(m_recovery || m_updating) continue;
        if(gpio_get_level(GPIO_NUM_0) == 0){
            if(++counter == 10) StartBinding();
            else if(counter == 40){
                bool provisioned;
                wifi_provisioned(true,&provisioned);
                nvs_clear_addrcount();nvs_write_interval(0);
                ESP_LOGI(LOG_TAG,"Config reset, reboot in 10 seconds");
                Restart();counter = 0;
            }
        }else counter = 0;
    }
}

void Kernel::Worker(void *parameter){
    auto *kernel = static_cast<Kernel *>(parameter);
    while(true){
        std::shared_ptr<BleJob> job;
        MEIZURemoterInfo probe;
        BLEAddress binding;
        bool have_probe = false,have_binding = false,start_scan = false;
        kernel->m_lock.Lock();
        if(kernel->m_updating){
            // The in-flight operation has finished; keep the worker quiescent.
        }else if(!kernel->m_jobs.empty()){
            job = kernel->m_jobs.front();kernel->m_jobs.pop_front();
            kernel->m_activeJob = job->kind == BleJob::IR ? "ir" : "learn";
        }else if(kernel->m_startScan){
            kernel->m_startScan = false;start_scan = true;
            kernel->m_activeJob = "bind_scan";
        }else if(!kernel->m_pendingBinds.empty()){
            auto it = kernel->m_pendingBinds.begin();
            binding = *it;kernel->m_pendingBinds.erase(it);have_binding = true;
            kernel->m_activeJob = "bind";
        }else if(kernel->m_mode == KernelMode::Normal){
            uint64_t earliest = std::numeric_limits<uint64_t>::max(),now = Now();
            for(const auto &entry:kernel->m_mapInfos){
                if(entry.second.m_poll.Due(now) && entry.second.m_poll.next_attempt_ms < earliest){
                    probe = entry.second;earliest = probe.m_poll.next_attempt_ms;have_probe = true;
                }
            }
            if(have_probe) kernel->m_activeJob = "poll";
        }
        kernel->m_lock.Unlock();
        if(job) kernel->RunJob(job);
        else if(start_scan){
            if(!GATTDevice::StartScan()) OnBindModeChanged(kernel,false);
        }
        else if(have_binding) kernel->BindDevice(binding);
        else if(have_probe) kernel->PollDevice(probe);
        else ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(250));
        kernel->m_lock.Lock();kernel->m_activeJob = "idle";kernel->m_lock.Unlock();
    }
}

bool Kernel::Enqueue(const std::shared_ptr<BleJob> &job){
    m_lock.Lock();
    bool accepted = m_worker && !m_updating && !m_recovery && m_mode == KernelMode::Normal && !m_learning && m_jobs.size() < 8;
    if(accepted){m_jobs.push_back(job);if(job->kind==BleJob::Learn) m_learning=job;}
    else job->result.status = m_updating ? "firmware_updating" : !m_worker ? "worker_unavailable" :
        (m_learning ? "learning_busy" : (m_mode == KernelMode::Normal ? "queue_full" : "binding_in_progress"));
    m_lock.Unlock();
    if(accepted) Wake();
    return accepted;
}
IRResult Kernel::Await(const std::shared_ptr<BleJob> &job){
    bool completed = job->completed.Wait(45000);
    m_lock.Lock();
    IRResult result = job->result;
    if(!completed && job->state.phase != CommandState::Done){
        result.status = job->state.CancelQueued() ? "queue_timeout" : "result_timeout";
        result.confirmed = false;
    }
    m_lock.Unlock();
    return result;
}
void Kernel::RunJob(const std::shared_ptr<BleJob> &job){
    m_lock.Lock();
    auto found = m_mapInfos.find(job->address);
    bool bound = found != m_mapInfos.end() && found->second.m_generation == job->generation;
    bool start = bound && !job->cancelled && job->state.Start(Now());
    if(!start){
        job->result.status = job->cancelled ? "cancelled" : (bound ? "queue_timeout" : "device_not_bound");
        job->state.phase = CommandState::Done;
        ++m_expiredJobs;
    }
    m_lock.Unlock();
    LearnResult learned;
    if(start){
        IRResult result;
        if(job->kind == BleJob::IR){
            result = m_pRemoterInterface->SendIRCode(job->address,job->key.data(),job->key.size(),
                job->code.empty() ? nullptr : job->code.data(),job->code.size());
            if(result.confirmed) led_set_mode(LED_MODE_FAST_BLINK,1);
        }else{
            LearnEvent(job,"connecting");
            learned=m_pRemoterInterface->LearnCode(job->address,job->window_ms,
                [this,job]{
                    if(job->cancelled || (job->client_id && !m_server.Connected(job->client_id))) return true;
                    m_lock.Lock();auto found=m_mapInfos.find(job->address);
                    bool retained=found!=m_mapInfos.end() && found->second.m_generation==job->generation;
                    m_lock.Unlock();return !retained;
                },
                [this,job](const char *phase,size_t received,size_t total){LearnEvent(job,phase,received,total);});
            result.status=learned.status;
            ESP_LOGI(LOG_TAG,"Learning %s result=%s bytes=%u",job->address.toString().c_str(),learned.status,learned.raw.size());
            if(!job->client_id && !learned.raw.empty()) ESP_LOG_BUFFER_HEX(LOG_TAG,learned.raw.data(),learned.raw.size());
        }
        m_lock.Lock();job->result = result;job->state.phase = CommandState::Done;m_lock.Unlock();
    }
    if(job->kind==BleJob::Learn){
        m_lock.Lock();
        if(m_learning==job) m_learning.reset();
        auto device=m_mapInfos.find(job->address);
        if(job->cancelled || device==m_mapInfos.end() || device->second.m_generation!=job->generation){
            job->result.status="cancelled";learned.raw.clear();
        }
        if(device!=m_mapInfos.end() && device->second.m_generation==job->generation)
            device->second.m_poll.next_attempt_ms=0;
        m_lock.Unlock();
        learned.status=job->result.status;
        LearnEvent(job,strcmp(learned.status,"ok")==0 ? "complete" :
            (strcmp(learned.status,"cancelled")==0 ? "cancelled" : "failed"),0,0,&learned);
    }
    job->completed.Unlock();
}

void Kernel::LearnEvent(const std::shared_ptr<BleJob> &job,const char *phase,size_t received,size_t total,const LearnResult *result){
    if(!job->client_id) return;
    cJSON *data=cJSON_CreateObject();
    cJSON_AddStringToObject(data,"request_id",job->request_id.c_str());
    cJSON_AddStringToObject(data,"device",job->address.toString().c_str());
    cJSON_AddStringToObject(data,"phase",phase);
    if(strcmp(phase,"ready")==0) cJSON_AddNumberToObject(data,"timeout_ms",total);
    if(strcmp(phase,"receiving")==0){
        cJSON_AddNumberToObject(data,"received",received);cJSON_AddNumberToObject(data,"length",total);
    }
    if(result){
        cJSON_AddStringToObject(data,"status",result->status);
        if(strcmp(result->status,"ok")==0){
            std::string hex;hex.reserve(result->raw.size()*2);
            const char *digits="0123456789abcdef";
            for(uint8_t byte:result->raw){hex+=digits[byte>>4];hex+=digits[byte&15];}
            cJSON_AddNumberToObject(data,"length",result->raw.size());
            cJSON_AddStringToObject(data,"raw",hex.c_str());
        }
    }
    char *message=Reply("learn_event",data);
    if(message){m_server.SendTo(job->client_id,message,strlen(message));free(message);}
}

void Kernel::PollDevice(MEIZURemoterInfo snapshot){
    // No map mutex or network lock is held during radio operations. A concurrent
    // unbind is safe: work owns a value copy and commits only to its generation.
    bool success = m_pRemoterInterface->ReadAllInfo(&snapshot);
    m_lock.Lock();
    auto current = m_mapInfos.find(snapshot.m_addr);
    bool retained = current != m_mapInfos.end() && current->second.m_generation == snapshot.m_generation;
    if(retained){
        snapshot.m_rssi = current->second.m_rssi;
        snapshot.m_poll = current->second.m_poll;
        if(success) snapshot.m_poll.Success(Now(),m_nUpdateInterval*60000U);
        else snapshot.m_poll.Failure(Now());
        current->second = snapshot;
    }
    m_lock.Unlock();
    if(retained){
        BoardcastInfo(snapshot);
        ESP_LOGI(LOG_TAG,"Probe %s available=%d failures=%u next_ms=%llu",snapshot.m_addr.toString().c_str(),
            snapshot.m_poll.available,snapshot.m_poll.failures,snapshot.m_poll.next_attempt_ms);
    }
}

void Kernel::BindDevice(BLEAddress address){
    GATTDevice::StopScan();
    m_lock.Lock();
    auto found = m_mapInfos.find(address);
    if(found == m_mapInfos.end()){
        MEIZURemoterInfo info;info.m_addr = address;info.m_generation = ++m_generation;
        m_mapInfos.emplace(address,info);SaveAddress();
    }else found->second.m_poll.next_attempt_ms = 0;
    m_lock.Unlock();
    Wake();
}
bool Kernel::StartBinding(){
    m_lock.Lock();bool ready = !m_updating && !m_recovery && m_mode == KernelMode::Normal && !m_learning;
    if(ready){m_mode = KernelMode::ReadyBind;m_startScan = true;}
    m_lock.Unlock();
    if(ready){led_set_mode(LED_MODE_SLOW_BLINK,0);Wake();}
    return ready;
}
void Kernel::OnBindModeChanged(void *caller,bool binding){
    auto *kernel = static_cast<Kernel *>(caller);
    kernel->m_lock.Lock();kernel->m_mode = binding ? KernelMode::Bind : KernelMode::Normal;kernel->m_lock.Unlock();
    led_set_mode(binding ? LED_MODE_SLOW_BLINK : (wifi_connected() ? LED_MODE_ON : LED_MODE_OFF),0);
    kernel->Wake();
}
void Kernel::OnDeviceFound(void *caller,esp_bd_addr_t *address){
    auto *kernel = static_cast<Kernel *>(caller);
    kernel->m_lock.Lock();
    if(kernel->m_mode == KernelMode::Bind && kernel->m_pendingBinds.empty()){
        kernel->m_pendingBinds.insert(BLEAddress(*address));
        kernel->m_mode = KernelMode::ReadyBind; // Accept exactly one puck per binding window.
    }
    kernel->m_lock.Unlock();kernel->Wake();
}
void Kernel::OnRSSIRead(void *caller,esp_bd_addr_t *address,int8_t rssi){
    auto *kernel = static_cast<Kernel *>(caller);
    kernel->m_lock.Lock();auto found = kernel->m_mapInfos.find(BLEAddress(*address));
    if(found != kernel->m_mapInfos.end()) found->second.m_rssi = rssi;
    kernel->m_lock.Unlock();
}

cJSON *Kernel::DeviceJSON(const MEIZURemoterInfo &info,uint64_t now){
    cJSON *data = cJSON_CreateObject();
    if(!data) return nullptr;
    BLEAddress address = info.m_addr;
    cJSON_AddStringToObject(data,"device",address.toString().c_str());
    cJSON_AddNumberToObject(data,"available",info.m_poll.available ? 1 : 0);
    cJSON_AddNumberToObject(data,"failures",info.m_poll.failures);
    cJSON_AddNumberToObject(data,"next_probe_in_ms",info.m_poll.next_attempt_ms > now ? info.m_poll.next_attempt_ms-now : 0);
    if(info.m_poll.has_sample) cJSON_AddNumberToObject(data,"measurement_age_ms",now-info.m_poll.last_success_ms);
    cJSON_AddItemToObject(data,"status",info.ToJSON());
    return data;
}
void Kernel::BoardcastInfo(const MEIZURemoterInfo &info){
    char *message = Reply("update",DeviceJSON(info,Now()));
    if(message){m_server.Boardcast(message,strlen(message));free(message);}
}

void Kernel::OnClientMessage(void * pCaller, SocketClient * pClient, char * pMessage, int nLen){
    Kernel * pKernel = (Kernel *)pCaller;
    ESP_LOGD(LOG_TAG, "Received message from %s:%d - %s", pClient->GetPeerAddr(), pClient->GetPeerPort(), pMessage);
    cJSON *json, *jtype, *data;
    char * pReply = nullptr;
    json = cJSON_Parse(pMessage);
    if(json != nullptr){
        if((jtype = cJSON_GetObjectItem(json, "type")) != nullptr && jtype->type == cJSON_String){
            if (strcmp(jtype->valuestring,"ota_status")==0 || strcmp(jtype->valuestring,"ota_challenge")==0 ||
                strcmp(jtype->valuestring,"ota_start")==0 || strcmp(jtype->valuestring,"ota_cancel")==0 ||
                strcmp(jtype->valuestring,"ota_auth")==0) {
                pReply=Reply(jtype->valuestring,ota.Handle(pClient,jtype->valuestring,cJSON_GetObjectItem(json,"data")));
            } else if (strcmp(jtype->valuestring, "config_info") == 0) {
                pReply = pKernel->process_message_configinfo();
            } else if (strcmp(jtype->valuestring, "wifi_reconnect") == 0) {
                cJSON *result = cJSON_CreateObject();
                cJSON_AddBoolToObject(result,"accepted",wifi_request_reconnect());
                pReply = Reply("wifi_reconnect",result);
            } else if (strcmp(jtype->valuestring, "diagnostics") == 0) {
                pReply = pKernel->process_message_diagnostics();
            } else if (strcmp(jtype->valuestring, "refresh") == 0) {
                data = cJSON_GetObjectItem(json,"data");
                pReply = pKernel->process_message_refresh(data);
            } else if (strcmp(jtype->valuestring, "devices") == 0) {
                pReply = pKernel->process_message_devices();
            } else if (strcmp(jtype->valuestring, "irsend") == 0) {
                if((data=cJSON_GetObjectItem(json, "data")) != nullptr && data->type == cJSON_Object){
                    pReply = pKernel->process_message_sendir(data);
                }
            } else if (strcmp(jtype->valuestring, "learn_start") == 0 || strcmp(jtype->valuestring, "learn_cancel") == 0) {
                pReply = pKernel->process_message_learn(pClient,cJSON_GetObjectItem(json,"data"),strcmp(jtype->valuestring,"learn_cancel")==0);
            } else if (strcmp(jtype->valuestring, "irrecv") == 0) {
                if((data=cJSON_GetObjectItem(json, "data")) != nullptr && data->type == cJSON_Object){
                    pReply = pKernel->process_message_recvir(data);
                }
            } else if (strcmp(jtype->valuestring, "bindthrld") == 0) {
                if((data=cJSON_GetObjectItem(json, "data")) != nullptr && data->type == cJSON_Object){
                    pReply = pKernel->process_message_bind_threshold(data);
                }
            } else if (strcmp(jtype->valuestring, "bind") == 0) {
                pReply = pKernel->process_message_bind();
            } else if (strcmp(jtype->valuestring, "removebind") == 0) {
                if((data=cJSON_GetObjectItem(json, "data")) != nullptr && data->type == cJSON_Object){
                    pReply = pKernel->process_message_removebind(data);
                }
            } else if (strcmp(jtype->valuestring, "heartbeat") == 0) {
                pReply = pMessage;
            } else if (strcmp(jtype->valuestring, "subscribe") == 0){
                pReply = pKernel->process_message_subscribe(pClient);
            } else if (strcmp(jtype->valuestring, "setinterval") == 0){
                if((data=cJSON_GetObjectItem(json, "data")) != nullptr && data->type == cJSON_Object){
                    pReply = pKernel->process_message_setinterval(data);
                }
            }
        }
        cJSON_Delete(json);
    }
    if(pReply != nullptr){
        int len = strlen(pReply);
        pKernel->m_server.SendMessage(pClient, pReply, len);
        if(pReply != pMessage){
            free(pReply);
        }
    }
}

void Kernel::Restart(){
    xTaskCreate(Kernel::task_Restart, "task_restart", 1024, NULL, tskIDLE_PRIORITY, NULL);
}

uint8_t * Kernel::ReadStreamFromASC(char * asc){
    if(asc == nullptr){
        return nullptr;
    }
    int len = strlen(asc);
    if(len % 2 != 0){
        return nullptr;
    }
    len /= 2;
    uint8_t * stream = (uint8_t *)malloc(len);
    if(!stream) return nullptr;
    for(int n = 0; n < len; n++){
        uint8_t high, low;
        high = asc[2 * n];
        low = asc[2 * n + 1];
        if(high>= '0' && high <= '9'){
            high -= 0x30;
        }
        else if(high >= 'A' && high <='F'){
            high -= 0x37;
        }
        else if(high >= 'a' && high <='f'){
            high -= 0x57;
        }
        else{
            free(stream);
            return nullptr;
        }
        if(low >= '0' && low <= '9'){
            low -= 0x30;
        }
        else if(low >= 'A' && low <='F'){
            low -= 0x37;
        }
        else if(low >= 'a' && low <='f'){
            low -= 0x57;
        }
        else{
            free(stream);
            return nullptr;
        }
        stream[n] = (high <<4) | low;
    }
    return stream;
}

char *Kernel::process_message_configinfo(){
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data,"version",mdns_get_version());
    cJSON_AddStringToObject(data,"serialno",mdns_get_serial());
    m_lock.Lock();uint8_t interval = m_nUpdateInterval;m_lock.Unlock();
    cJSON_AddNumberToObject(data,"update_interval",interval);
    cJSON *caps = cJSON_AddArrayToObject(data,"capabilities");
    for(const char *cap:{"ir_ack_v1","recovery_v1","measurement_age_v1","diagnostics_v1","learn_v1"})
        cJSON_AddItemToArray(caps,cJSON_CreateString(cap));
    cJSON_AddItemToArray(caps,cJSON_CreateString("ota_pull_v1"));
    cJSON_AddItemToObject(data,"ota",ota.Status());
    return Reply("config_info",data);
}
char *Kernel::process_message_devices(){
    std::vector<MEIZURemoterInfo> snapshots;
    m_lock.Lock();for(const auto &entry:m_mapInfos) snapshots.push_back(entry.second);m_lock.Unlock();
    cJSON *data = cJSON_CreateObject(),*devices = cJSON_AddArrayToObject(data,"devices");
    uint64_t now = Now();
    for(const auto &info:snapshots) cJSON_AddItemToArray(devices,DeviceJSON(info,now));
    return Reply("devices",data);
}
char *Kernel::process_message_refresh(cJSON *data){
    cJSON *device = cJSON_GetObjectItem(data,"device");
    bool accepted = false;
    if(cJSON_IsString(device) && ValidAddress(device->valuestring)){
        m_lock.Lock();auto found = m_mapInfos.find(BLEAddress(device->valuestring));
        if(found != m_mapInfos.end()){found->second.m_poll.next_attempt_ms = 0;accepted = true;}
        m_lock.Unlock();
    }
    if(accepted) Wake();
    cJSON *reply = cJSON_CreateObject();cJSON_AddBoolToObject(reply,"accepted",accepted);
    return Reply("refresh",reply);
}
char *Kernel::process_message_diagnostics(){
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data,"uptime_ms",Now());
    cJSON_AddNumberToObject(data,"reset_reason",esp_reset_reason());
    cJSON_AddNumberToObject(data,"free_heap",esp_get_free_heap_size());
    cJSON_AddNumberToObject(data,"minimum_free_heap",esp_get_minimum_free_heap_size());
    m_lock.Lock();
    cJSON_AddBoolToObject(data,"ble_worker_ready",m_worker != nullptr);
    cJSON_AddStringToObject(data,"ble_active_job",m_activeJob);
    cJSON_AddNumberToObject(data,"ble_queue_depth",m_jobs.size());
    cJSON_AddNumberToObject(data,"expired_jobs",m_expiredJobs);
    m_lock.Unlock();
    wifi_diagnostics_t wifi;wifi_get_diagnostics(&wifi);
    cJSON *network = cJSON_AddObjectToObject(data,"wifi");
    cJSON_AddBoolToObject(network,"connected",wifi.connected);
    cJSON_AddNumberToObject(network,"disconnects",wifi.disconnects);
    cJSON_AddNumberToObject(network,"reconnect_attempts",wifi.reconnect_attempts);
    cJSON_AddNumberToObject(network,"consecutive_failures",wifi.consecutive_failures);
    cJSON_AddNumberToObject(network,"last_disconnect_reason",wifi.last_disconnect_reason);
    cJSON_AddNumberToObject(network,"retry_in_ms",wifi.retry_in_ms);
    return Reply("diagnostics",data);
}
char *Kernel::process_message_sendir(cJSON *data){
    uint64_t started = Now();IRResult result;result.status = "invalid_parameters";
    cJSON *request = cJSON_GetObjectItem(data,"request_id"),*device = cJSON_GetObjectItem(data,"device");
    cJSON *key = cJSON_GetObjectItem(data,"key"),*code = cJSON_GetObjectItem(data,"ircode");
    const char *id = cJSON_IsString(request) ? request->valuestring : "";
    const char *address = cJSON_IsString(device) ? device->valuestring : "";
    if(strlen(id) <= 64 && ValidAddress(address) && cJSON_IsString(key) && strlen(key->valuestring) == 18 && (!code || cJSON_IsString(code))){
        BLEAddress addr(address);m_lock.Lock();auto found = m_mapInfos.find(addr);
        bool bound = found != m_mapInfos.end();uint32_t generation = bound ? found->second.m_generation : 0;m_lock.Unlock();
        if(!bound) result.status = "device_not_bound";
        else{
            size_t length = code ? strlen(code->valuestring) : 0;
            uint8_t *key_bytes = ReadStreamFromASC(key->valuestring);
            uint8_t *code_bytes = length ? ReadStreamFromASC(code->valuestring) : nullptr;
            if(key_bytes && (!code || (length && length%2 == 0 && code_bytes))){
                auto job = std::make_shared<BleJob>(BleJob::IR,addr,generation,Now());
                job->key.assign(key_bytes,key_bytes+9);
                if(length) job->code.assign(code_bytes,code_bytes+length/2);
                if(Enqueue(job)) result = Await(job);else result = job->result;
            }
            free(key_bytes);free(code_bytes);
        }
    }
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply,"request_id",strlen(id)<=64 ? id : "");
    cJSON_AddStringToObject(reply,"device",address);
    cJSON_AddBoolToObject(reply,"success",result.confirmed);
    cJSON_AddBoolToObject(reply,"confirmed",result.confirmed);
    cJSON_AddBoolToObject(reply,"cached",result.cached);
    cJSON_AddStringToObject(reply,"status",result.status);
    cJSON_AddNumberToObject(reply,"ack",result.ack);
    cJSON_AddNumberToObject(reply,"elapsed_ms",Now()-started);
    ESP_LOGI(LOG_TAG,"IR %s result=%s ack=%02x",address,result.status,result.ack);
    return Reply("irsend_result",reply);
}
char *Kernel::process_message_learn(SocketClient *client,cJSON *data,bool cancel){
    cJSON *request=cJSON_GetObjectItem(data,"request_id"),*device=cJSON_GetObjectItem(data,"device");
    cJSON *timeout=cJSON_GetObjectItem(data,"timeout");
    const char *id=cJSON_IsString(request) ? request->valuestring : "";
    const char *address=cJSON_IsString(device) ? device->valuestring : "";
    const char *status="invalid_parameters";
    bool accepted=false;
    if(strlen(id)>0 && strlen(id)<=64 && ValidAddress(address)){
        if(cancel){
            m_lock.Lock();
            if(m_learning && m_learning->client_id==client->Id() && m_learning->request_id==id &&
                    m_learning->address==BLEAddress(address)){
                m_learning->cancelled=true;accepted=true;
            }
            m_lock.Unlock();Wake();
        }else if(!timeout || (cJSON_IsNumber(timeout) && timeout->valuedouble==timeout->valueint && timeout->valueint>=1 && timeout->valueint<=15)){
            BLEAddress addr(address);m_lock.Lock();auto found=m_mapInfos.find(addr);
            bool bound=found!=m_mapInfos.end();uint32_t generation=bound ? found->second.m_generation : 0;m_lock.Unlock();
            if(!bound) status="device_not_bound";
            else{
                auto job=std::make_shared<BleJob>(BleJob::Learn,addr,generation,Now());
                job->request_id=id;job->client_id=client->Id();job->window_ms=timeout ? timeout->valueint*1000 : 15000;
                accepted=Enqueue(job);status=accepted ? "ok" : job->result.status;
            }
        }
    }
    cJSON *reply=cJSON_CreateObject();
    cJSON_AddStringToObject(reply,"request_id",strlen(id)<=64 ? id : "");
    cJSON_AddStringToObject(reply,"device",address);
    if(cancel){cJSON_AddBoolToObject(reply,"accepted",accepted);return Reply("learn_cancel",reply);}
    cJSON_AddStringToObject(reply,"phase",accepted ? "queued" : "failed");
    cJSON_AddStringToObject(reply,"status",status);
    return Reply("learn_event",reply);
}

char *Kernel::process_message_recvir(cJSON *data){
    bool accepted = false;cJSON *device = cJSON_GetObjectItem(data,"device");
    if(cJSON_IsString(device) && ValidAddress(device->valuestring)){
        BLEAddress addr(device->valuestring);m_lock.Lock();auto found = m_mapInfos.find(addr);
        bool bound = found != m_mapInfos.end();uint32_t generation = bound ? found->second.m_generation : 0;m_lock.Unlock();
        if(bound) accepted = Enqueue(std::make_shared<BleJob>(BleJob::Learn,addr,generation,Now()));
    }
    cJSON *reply = cJSON_CreateObject();cJSON_AddBoolToObject(reply,"accepted",accepted);return Reply("irrecv",reply);
}
char *Kernel::process_message_bind_threshold(cJSON *data){
    if(m_updating || m_recovery) return nullptr;
    cJSON *threshold = cJSON_GetObjectItem(data,"threshold");
    if(cJSON_IsNumber(threshold) && threshold->valueint >= -60 && threshold->valueint <= -15){
        nvs_write_bindthrld(threshold->valueint);GATTDevice::SetBindThreshold(threshold->valueint);
    }
    return nullptr;
}
char *Kernel::process_message_bind(){
    cJSON *data = cJSON_CreateObject();cJSON_AddStringToObject(data,"status",StartBinding() ? "1" : "0");return Reply("bind",data);
}
char *Kernel::process_message_removebind(cJSON *data){
    if(m_updating || m_recovery) return nullptr;
    cJSON *device = cJSON_GetObjectItem(data,"device");bool removed = false;
    if(cJSON_IsString(device) && ValidAddress(device->valuestring)){
        BLEAddress address(device->valuestring);m_lock.Lock();removed = m_mapInfos.erase(address) != 0;
        m_pendingBinds.erase(address);if(removed) SaveAddress();m_lock.Unlock();
        if(removed){
            cJSON *reply = cJSON_CreateObject();cJSON_AddStringToObject(reply,"device",device->valuestring);
            char *message = Reply("removebind",reply);if(message){m_server.Boardcast(message,strlen(message));free(message);}
        }
    }
    return nullptr;
}
char *Kernel::process_message_subscribe(SocketClient *client){m_server.Subscribe(client);return nullptr;}
char *Kernel::process_message_setinterval(cJSON *data){
    if(m_updating || m_recovery) return nullptr;
    cJSON *interval = cJSON_GetObjectItem(data,"update_interval");
    if(cJSON_IsNumber(interval) && interval->valueint >= 1 && interval->valueint <= 30){
        m_lock.Lock();m_nUpdateInterval = interval->valueint;
        for(auto &entry:m_mapInfos) if(entry.second.m_poll.available)
            entry.second.m_poll.next_attempt_ms = entry.second.m_poll.last_success_ms+m_nUpdateInterval*60000U;
        nvs_write_interval(m_nUpdateInterval);m_lock.Unlock();Wake();
        cJSON *reply = cJSON_CreateObject();cJSON_AddNumberToObject(reply,"update_interval",interval->valueint);
        char *message = Reply("setinterval",reply);if(message){m_server.Boardcast(message,strlen(message));free(message);}
    }
    return nullptr;
}
void Kernel::SaveAddress(){
    std::vector<uint8_t> addresses;
    for(const auto &entry:m_mapInfos){BLEAddress address = entry.first;auto *bytes = *address.getNative();addresses.insert(addresses.end(),bytes,bytes+ESP_BD_ADDR_LEN);}
    const uint8_t empty = 0;
    if(!nvs_write_addrs(m_mapInfos.size(),addresses.empty() ? &empty : addresses.data(),addresses.size()))
        ESP_LOGE(LOG_TAG,"Failed to persist binding list");
}

bool Kernel::PauseOTA(bool pause){
    m_lock.Lock();
    bool accepted=!pause || (!m_updating && !m_learning && m_jobs.empty() &&
        m_mode==KernelMode::Normal && !m_startScan && m_pendingBinds.empty());
    if(accepted) m_updating=pause;
    m_lock.Unlock();Wake();return accepted;
}
bool Kernel::OTAIdle(){
    m_lock.Lock();bool idle=strcmp(m_activeJob,"idle")==0;m_lock.Unlock();return idle;
}
