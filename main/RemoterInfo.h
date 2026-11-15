/* Created by @georgezhao2010; maintained by this fork. */
#ifndef _REMOTER_INFO_H_
#define _REMOTER_INFO_H_
#include "BLEAddress.h"
#include "Recovery.h"
#include <string>
#include <cJSON.h>

class MEIZURemoterInfo {
public:
    void update_manufacturer(const char *value, uint16_t length);
    void update_model(const char *value, uint16_t length);
    void update_firmware(const char *value, uint16_t length);
    void update_temperature(float value){m_temperature = value; m_hasTemperature = true;}
    void update_humidity(float value){m_humidity = value; m_hasHumidity = true;}
    void update_voltage(uint8_t voltage);
    bool has_manufacturer() const {return !m_manufacturer.empty();}
    bool has_model() const {return !m_model.empty();}
    bool has_firmware() const {return !m_firmware.empty();}
    cJSON *ToJSON() const;
    BLEAddress m_addr;
    int8_t m_rssi = -127;
    uint32_t m_generation = 0;
    RecoveryState m_poll;
private:
    std::string m_manufacturer, m_model, m_firmware;
    float m_temperature = 0, m_humidity = 0, m_voltage = 0;
    uint16_t m_battery = 0;
    bool m_hasTemperature = false, m_hasHumidity = false, m_hasVoltage = false;
};
#endif
