/*
 * Created by @georgezhao2010
 * https://github.com/georgezhao2010/esp32_meizu_remoter_gateway/
 * involute@sina.com
 */
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gatt_common_api.h>
#include <esp_log.h>
#include <string.h>
#include "GATTDevice.h"
#include "GATTRemoteService.h"
#include "GATTRemoteCharacteristic.h"

#define LOG_TAG "GATTDevice"

uint16_t g_appid = 0;
std::map<uint16_t, GATTDevice*> g_appidmap;
std::map<esp_gatt_if_t, GATTDevice*> g_ifmap;

on_bind g_bind = nullptr;
on_device_found g_device_found = nullptr;
on_rssi_read g_rssi_read = nullptr;
void * g_caller = nullptr;
bool g_scanning = false;
int g_scan_threshold = CONFIG_BLE_RSSI_HIGH_THRLD;

GATTDevice::GATTDevice(){
    m_lock = new SemaphoreLock(true);
    m_gattc_if = ESP_GATT_IF_NONE;
    m_connid = 0xffff;
    m_readlen = 0;
    m_waiting = false;
    m_appid = g_appid++;
    m_connectingState = GATTConnectionState::Idle;
    g_appidmap.insert(std::pair<uint16_t, GATTDevice*>(m_appid, this));
    esp_err_t ret = esp_ble_gattc_app_register(m_appid);
    if (ret != ESP_OK){
        ESP_LOGE("GATTDevice::GATTDevice", "Gattc app register failed, error code = %x", ret);
        return;
    }
}

GATTDevice::~GATTDevice(){
    ClearServices();
    esp_ble_gattc_app_unregister(m_gattc_if);
    delete m_lock;
    if(g_appidmap.find(m_appid) != g_appidmap.end())
        g_appidmap.erase(m_appid);
}

void GATTDevice::BeginWait(esp_gattc_cb_event_t event, uint16_t handle){
    m_lock->Reset();
    m_waitEvent = event;
    m_waitHandle = handle;
    m_status = ESP_GATT_ERROR;
    m_waiting = true;
}

bool GATTDevice::Wait(uint32_t timeout_ms){
    bool received = m_lock->Wait(timeout_ms);
    m_waiting = false;
    return received && m_status == ESP_GATT_OK;
}

void GATTDevice::Complete(esp_gattc_cb_event_t event, esp_gatt_status_t status, uint16_t handle){
    if(m_waiting && m_waitEvent == event && m_waitHandle == handle){
        m_status = status;
        m_lock->Unlock();
    }
}

bool GATTDevice::Connect(BLEAddress addr){
    Close();
    ClearServices(); // Handles belong to one device; do not reuse across pucks.
    m_addr = addr;
    m_connectingState = GATTConnectionState::Connecting;
    BeginWait(ESP_GATTC_DIS_SRVC_CMPL_EVT);
    esp_err_t ret = esp_ble_gattc_open(m_gattc_if, *m_addr.getNative(), BLE_ADDR_TYPE_PUBLIC, true);
    if(ret != ESP_OK || !Wait(12000)){
        m_waiting = false;
        Close();
        m_connectingState = GATTConnectionState::Failed;
        return false;
    }
    BeginWait(ESP_GATTC_SEARCH_CMPL_EVT);
    ret = esp_ble_gattc_search_service(m_gattc_if, m_connid, NULL);
    if(ret != ESP_OK || !Wait(3000)){
        m_waiting = false;
        Close();
        return false;
    }
    return m_connectingState == GATTConnectionState::Connected;
}

void GATTDevice::Close(){
    if(m_connid != 0xffff){
        BeginWait(ESP_GATTC_CLOSE_EVT);
        if(esp_ble_gattc_close(m_gattc_if, m_connid) == ESP_OK) Wait(1500);
    } else if(m_connectingState == GATTConnectionState::Connecting) {
        esp_ble_gap_disconnect(*m_addr.getNative());
    }
    m_waiting = false;
    m_connid = 0xffff;
    m_connectingState = GATTConnectionState::Idle;
}

bool GATTDevice::Init(void * pCaller, on_bind onBind, on_device_found OnDeviceFound, on_rssi_read OnRSSIRead){
    g_caller = pCaller;
    g_bind = onBind;
    g_device_found = OnDeviceFound;
    g_rssi_read = OnRSSIRead;
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    esp_err_t ret;
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(LOG_TAG, "Failed to bluetooth controller initialization: %s", esp_err_to_name(ret));
        return false;
    }
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(LOG_TAG, "Failed to enable bluetooth controller: %s\n", esp_err_to_name(ret));
        return false;
    }
    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(LOG_TAG, "Failed to bluetooth initializatio: %s\n", esp_err_to_name(ret));
        return false;
    }
    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(LOG_TAG, "Failed to enable bluetooth: %s\n", esp_err_to_name(ret));
        return false;
    }
    ret = esp_ble_gap_register_callback(GATTDevice::GAPEventHandler);
    if (ret){
        ESP_LOGE(LOG_TAG, "Failed to GAP register: %s", esp_err_to_name(ret));
        return false;
    }
    ret = esp_ble_gattc_register_callback(GATTDevice::GATTEventHandler);
    if(ret){
        ESP_LOGE(LOG_TAG, "Failed to GATTC register: %s", esp_err_to_name(ret));
        return false;
    }
    ret = esp_ble_gatt_set_local_mtu(500);
    if (ret){
        ESP_LOGE(LOG_TAG, "Failed to set local MTU:%s", esp_err_to_name(ret));
        return false;
    }
    return true;
}

void GATTDevice::GAPEventHandler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t * param){
    switch (event){
        case ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT:
            if (param->read_rssi_cmpl.status != ESP_BT_STATUS_SUCCESS){
                ESP_LOGE(LOG_TAG, "Read rssi failed, error status = %x", param->read_rssi_cmpl.status);
                break;
            }
            if(g_rssi_read != nullptr){
                g_rssi_read(g_caller, &param->read_rssi_cmpl.remote_addr, param->read_rssi_cmpl.rssi);
            }
            break; 
        case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
            break;
        case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
            if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS){
                ESP_LOGE(LOG_TAG, "Scan start failed, error status = %x", param->scan_start_cmpl.status);
                if(g_bind != nullptr){
                    g_bind(g_caller, false);
                }
                break;
            }
            ESP_LOGI(LOG_TAG, "BLE devices scanning");
            if(g_bind != nullptr){
                g_bind(g_caller, true);
            }
            break;
        case ESP_GAP_BLE_SCAN_RESULT_EVT:{
                esp_ble_gap_cb_param_t *scan_result = (esp_ble_gap_cb_param_t *)param;
                switch (scan_result->scan_rst.search_evt){
                    case ESP_GAP_SEARCH_INQ_RES_EVT:{
                            uint8_t *adv_name = NULL;
                            uint8_t adv_name_len = 0;
                            adv_name = esp_ble_resolve_adv_data(scan_result->scan_rst.ble_adv,ESP_BLE_AD_TYPE_NAME_CMPL, &adv_name_len);
                            if(scan_result->scan_rst.rssi >= g_scan_threshold) {
                                char * pName = (char *)malloc(adv_name_len + 1);
                                memcpy(pName, adv_name, adv_name_len);
                                pName[adv_name_len] = 0;
                                ESP_LOGI(LOG_TAG, "Nearly device found - %s", 
                                    pName[0] == 0? "Noname device": pName);
                                if (scan_result->scan_rst.scan_rsp_len == 10 && memcmp(&scan_result->scan_rst.ble_adv[scan_result->scan_rst.adv_data_len + 4], "RW-BLE", 6) == 0){
                                    if(g_device_found!=nullptr){
                                        g_device_found(g_caller, &scan_result->scan_rst.bda);
                                    }
                                }
                                free(pName);
                            }
                        }
                        break;
                    case ESP_GAP_SEARCH_INQ_CMPL_EVT:
                        g_scanning = false;
                        ESP_LOGI(LOG_TAG, "No new device found, stop scan");
                        if(g_bind != nullptr){
                            g_bind(g_caller, false);
                        }
                        break;
                    default:
                        break;
                }
            }
            break;
        case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
            if (param->scan_stop_cmpl.status == ESP_BT_STATUS_SUCCESS && g_bind != nullptr){
                g_scanning = false;
                g_bind(g_caller, false);
            }
            
            break;
        case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
            if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS){
                ESP_LOGE(LOG_TAG, "Adv stop failed, error status = %x", param->adv_stop_cmpl.status);
                break;
            }
            break;
        default:
            break;
    }
}

void GATTDevice::SetBindThreshold(int threshold){
    g_scan_threshold = threshold;
}

bool GATTDevice::StartScan(){
    if(g_scanning == false){
        uint32_t duration = CONFIG_BLE_BIND_DURATION;
        esp_err_t ret = esp_ble_gap_start_scanning(duration);
        if(ret != ESP_OK){
            ESP_LOGI(LOG_TAG, "Failed to start binding mode: %s", esp_err_to_name(ret));
            return false;
        }
        ESP_LOGI(LOG_TAG, "Start binding mode");
        g_scanning = true;
        return true;
    }
    return false;
}

bool GATTDevice::StopScan(){
    if(g_scanning == true){
        esp_ble_gap_stop_scanning();
        return true;
    }
    return false;
}

void GATTDevice::GATTEventHandler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param){
    if (event == ESP_GATTC_REG_EVT){
        if (param->reg.status == ESP_GATT_OK){
            if(g_appidmap.find(param->reg.app_id) != g_appidmap.end()){
                GATTDevice * pThis = g_appidmap.find(param->reg.app_id)->second;
                pThis->m_gattc_if = gattc_if;
                g_ifmap.insert(std::pair<esp_gatt_if_t, GATTDevice*>(gattc_if, pThis));
            }
        } 
        else{
            ESP_LOGI(LOG_TAG, "Failed to reg app, app_id %04x, status %d",
                    param->reg.app_id,
                    param->reg.status);
        }
    }
    else if(gattc_if != ESP_GATT_IF_NONE){
        if(g_ifmap.find(gattc_if) != g_ifmap.end())
            g_ifmap.find(gattc_if)->second->HandleEvent(event, gattc_if, param);
    }
}

void GATTDevice::HandleEvent(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param){
    if (m_gattc_if == ESP_GATT_IF_NONE && event != ESP_GATTC_REG_EVT){
        return;
    }
    
    switch(event){
        case ESP_GATTC_OPEN_EVT:
            if(memcmp(param->open.remote_bda, *m_addr.getNative(), ESP_BD_ADDR_LEN) != 0) break;
            if(param->open.status == ESP_GATT_OK){
                if(m_connectingState != GATTConnectionState::Connecting){
                    esp_ble_gattc_close(m_gattc_if, param->open.conn_id);
                    break;
                }
                m_connid = param->open.conn_id;
                esp_ble_gap_read_rssi(*m_addr.getNative());
            } else {
                m_connectingState = GATTConnectionState::Failed;
                Complete(ESP_GATTC_DIS_SRVC_CMPL_EVT, param->open.status);
            }
            break;
        case ESP_GATTC_DIS_SRVC_CMPL_EVT:
            if(param->dis_srvc_cmpl.conn_id != m_connid) break;
            m_connectingState = param->dis_srvc_cmpl.status == ESP_GATT_OK ?
                GATTConnectionState::Connected : GATTConnectionState::Failed;
            Complete(event, param->dis_srvc_cmpl.status);
            break;
        case ESP_GATTC_CLOSE_EVT:
            if(param->close.conn_id != m_connid) break;
            Complete(event, param->close.status);
            m_connid = 0xffff;
            m_connectingState = GATTConnectionState::Idle;
            break;
        case ESP_GATTC_DISCONNECT_EVT:
            if(param->disconnect.conn_id != m_connid) break;
            m_connectingState = GATTConnectionState::Idle;
            m_connid = 0xffff;
            if(m_waiting){
                m_status = ESP_GATT_ERROR;
                m_lock->Unlock();
            }
            break;
        case ESP_GATTC_SEARCH_CMPL_EVT:
            if(param->search_cmpl.conn_id == m_connid) Complete(event, param->search_cmpl.status);
            break;
        case ESP_GATTC_SEARCH_RES_EVT:{
            // 7 When GATT service discovery result is got, the event comes
                BLEUUID uuid = BLEUUID(param->search_res.srvc_id.uuid);
                if(IsInterestedServices(uuid)){
                    GATTRemoteService* pRemoteService = new GATTRemoteService(
                        this,
                        param->search_res.srvc_id,
                        param->search_res.start_handle,
                        param->search_res.end_handle
                    );
                    m_mapServices.insert(std::pair<BLEUUID, GATTRemoteService*>(uuid, pRemoteService));
                }
            }
            break;
        case ESP_GATTC_READ_CHAR_EVT:
            if(param->read.conn_id != m_connid || !m_waiting || m_waitEvent != event || m_waitHandle != param->read.handle) break;
            m_readlen = 0;
            if(param->read.status == ESP_GATT_OK && param->read.value_len < sizeof(m_readbuff)){
                m_readlen = param->read.value_len;
                memcpy(m_readbuff, param->read.value, m_readlen);
                m_readbuff[m_readlen] = 0;
            }
            Complete(event, param->read.status, param->read.handle);
            break;
        case ESP_GATTC_WRITE_CHAR_EVT:
        case ESP_GATTC_WRITE_DESCR_EVT:
            if(param->write.conn_id == m_connid) Complete(event, param->write.status, param->write.handle);
            break;
        case ESP_GATTC_REG_FOR_NOTIFY_EVT:
            Complete(event, param->reg_for_notify.status, param->reg_for_notify.handle);
            break;
        case ESP_GATTC_UNREG_FOR_NOTIFY_EVT:
            Complete(event, param->unreg_for_notify.status, param->unreg_for_notify.handle);
            break;
        case ESP_GATTC_NOTIFY_EVT:
            if(param->notify.conn_id != m_connid) break;
            for (auto &it : m_mapServices){
                GATTRemoteCharacteristic *pCharacteristic = it.second->GetCharacteristicByHandle(param->notify.handle);
                if(pCharacteristic != nullptr){
                    OnNotify(pCharacteristic, param->notify.value, param->notify.value_len);
                }
            }
            break;
        default:
            break;
    }
}

void GATTDevice::ClearServices(){
    for (auto &it : m_mapServices) {
       delete it.second;
    }
    m_mapServices.clear();
}

GATTRemoteService * GATTDevice::GetService(const char * uuid){
    return GetService(BLEUUID(uuid));
}

GATTRemoteService * GATTDevice::GetService(BLEUUID uuid){
    GATTRemoteService * ret = nullptr;
    if(m_mapServices.find(uuid) != m_mapServices.end()){
        ret = m_mapServices.find(uuid)->second;
    }
    return ret;
}

bool GATTDevice::Read(GATTRemoteCharacteristic *characteristic){
    m_readlen = 0;
    if(!characteristic || m_connectingState != GATTConnectionState::Connected) return false;
    BeginWait(ESP_GATTC_READ_CHAR_EVT, characteristic->GetHandle());
    if(!characteristic->Read()) {m_waiting = false; return false;}
    if(!Wait(1000)){Close(); return false;}
    return m_readlen > 0;
}

bool GATTDevice::Write(GATTRemoteCharacteristic *characteristic, uint8_t *data, uint16_t length, bool response){
    if(!characteristic || m_connectingState != GATTConnectionState::Connected) return false;
    BeginWait(ESP_GATTC_WRITE_CHAR_EVT, characteristic->GetHandle());
    if(!characteristic->Write(data, length, response)) {m_waiting = false; return false;}
    if(!Wait(1000)){Close(); return false;}
    return true;
}

bool GATTDevice::Subscribe(GATTRemoteCharacteristic *characteristic, bool enable){
    if(!characteristic) return false;
    const uint16_t handle = characteristic->GetHandle();
    BeginWait(enable ? ESP_GATTC_REG_FOR_NOTIFY_EVT : ESP_GATTC_UNREG_FOR_NOTIFY_EVT, handle);
    if(!(enable ? characteristic->RegNotify() : characteristic->UnregNotify())) {m_waiting = false; return false;}
    if(!Wait(1000)) return false;
    if(!enable) return true; // Disconnect resets the remote CCCD state.
    esp_bt_uuid_t uuid = {};
    uuid.len = ESP_UUID_LEN_16;
    uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
    esp_gattc_descr_elem_t descriptor;
    uint16_t count = 1;
    if(esp_ble_gattc_get_descr_by_char_handle(m_gattc_if, m_connid, handle, uuid, &descriptor, &count) != ESP_GATT_OK || count != 1) return false;
    uint8_t value[2] = {1, 0};
    BeginWait(ESP_GATTC_WRITE_DESCR_EVT, descriptor.handle);
    if(esp_ble_gattc_write_char_descr(m_gattc_if, m_connid, descriptor.handle, 2, value,
            ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE) != ESP_OK) {m_waiting = false; return false;}
    return Wait(1000);
}
