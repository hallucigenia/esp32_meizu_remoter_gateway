/* Created by @georgezhao2010; maintained by this fork. */
#include "RemoterInfo.h"

static void Update(std::string &target, const char *value, uint16_t length){
    if(value && length) target.assign(value, length > 255 ? 255 : length);
}
void MEIZURemoterInfo::update_manufacturer(const char *v, uint16_t n){Update(m_manufacturer,v,n);}
void MEIZURemoterInfo::update_model(const char *v, uint16_t n){Update(m_model,v,n);}
void MEIZURemoterInfo::update_firmware(const char *v, uint16_t n){Update(m_firmware,v,n);}

void MEIZURemoterInfo::update_voltage(uint8_t voltage){
    if(voltage >= 30) m_battery = 100;
    else switch(voltage){
        case 29: m_battery = 90; break;
        case 28: m_battery = 80; break;
        case 27: m_battery = 60; break;
        case 26: m_battery = 30; break;
        default: m_battery = 10;
    }
    m_voltage = voltage / 10.0f;
    m_hasVoltage = true;
}

cJSON *MEIZURemoterInfo::ToJSON() const {
    cJSON *json = cJSON_CreateObject();
    if(!json) return nullptr;
    if(has_manufacturer()) cJSON_AddStringToObject(json,"manufacturer",m_manufacturer.c_str());
    if(has_model()) cJSON_AddStringToObject(json,"model",m_model.c_str());
    // Keep the original wire spelling for existing HA clients.
    if(has_firmware()) cJSON_AddStringToObject(json,"fireware",m_firmware.c_str());
    if(m_hasTemperature) cJSON_AddNumberToObject(json,"temperature",m_temperature);
    if(m_hasHumidity) cJSON_AddNumberToObject(json,"humidity",m_humidity);
    if(m_hasVoltage){
        cJSON_AddNumberToObject(json,"battery",m_battery);
        cJSON_AddNumberToObject(json,"voltage",m_voltage);
    }
    if(m_rssi != -127) cJSON_AddNumberToObject(json,"rssi",m_rssi);
    return json;
}
