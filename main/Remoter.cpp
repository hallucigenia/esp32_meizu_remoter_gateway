/*
 * Created by @georgezhao2010
 * https://github.com/georgezhao2010/esp32_meizu_remoter_gateway/
 * involute@sina.com
 */
#include <esp_log.h>
#include <string.h>
#include <esp_timer.h>
#include "Remoter.h"
#include "GATTRemoteService.h"
#include "GATTRemoteCharacteristic.h"
#include "BLEUUID.h"
#include "utils.h"

#define CMD_PACKET                      (uint8_t)0x00
#define CMD_SEND_IR                     (uint8_t)0x03
#define CMD_SEND_IR_READY               (uint8_t)0x04
#define CMD_RECV_IR                     (uint8_t)0x05
#define CMD_RECV_IR_READY               (uint8_t)0x07
#define CMD_VOLTAGE                     (uint8_t)0x10
#define CMD_TEMP_HUMI                   (uint8_t)0x11


#define LOG_TAG "MEIZURemoter"

static BLEUUID serviceUUID("16F0");
static BLEUUID characteristicUUID("16F2");
static BLEUUID infoUUID("180A");
static BLEUUID manufacturerUUID("2A29");
static BLEUUID modelUUID("2A24");
static BLEUUID firmwareUUID("2A26");

MEIZURemoter::MEIZURemoter(){
    m_sequence = 0;
    m_irFrames = xQueueCreate(16, sizeof(IRFrame));
    m_irCharacteristic = nullptr;
    AddInterestedServices(serviceUUID);
    AddInterestedServices(infoUUID);
    AddInterestedCharacteristic(characteristicUUID);
    AddInterestedCharacteristic(manufacturerUUID);
    AddInterestedCharacteristic(modelUUID);
    AddInterestedCharacteristic(firmwareUUID);
}

MEIZURemoter::~MEIZURemoter(){
    if(m_irFrames) vQueueDelete(m_irFrames);
}

bool MEIZURemoter::ReadAllInfo(MEIZURemoterInfo * pInfo){
    m_lockDevice.Lock();
    bool temperature_received = false;
    bool voltage_received = false;
    if(pInfo->m_update == 0) pInfo->m_update = 1;
    ESP_LOGI(LOG_TAG, "Begin update device at %s", pInfo->m_addr.toString().c_str());
    if(Connect(pInfo->m_addr)){
         GATTRemoteService * pService;
        if(!pInfo->has_manufacturer()){
            pService = GetService(infoUUID);
            if(pService != nullptr){
                GATTRemoteCharacteristic * pCh = pService->GetCharacteristic(manufacturerUUID);
                if(pCh != nullptr){
                    if(Read(pCh)){
                        pInfo->update_manufacturer((char *)m_readbuff, m_readlen);
                    }
                }
                pCh = pService->GetCharacteristic(modelUUID);
                if(pCh != nullptr){
                    if(Read(pCh)){
                        pInfo->update_model((char *)m_readbuff, m_readlen);
                    }
                }
                pCh = pService->GetCharacteristic(firmwareUUID);
                if(pCh != nullptr){
                    if(Read(pCh)){
                        pInfo->update_firmware((char *)m_readbuff, m_readlen);
                    }
                }
            }
        }
        pService = GetService(serviceUUID);
        if(pService != nullptr){
            GATTRemoteCharacteristic * pCh = pService->GetCharacteristic(characteristicUUID);
            uint8_t sequence = NextSequence();
            bool written = SendCommand(pCh, sequence, CMD_TEMP_HUMI);
            bool read = written && Read(pCh);
            if (read && m_readbuff[2] == sequence && m_readlen == 8 && m_readbuff[0] == 0x55 && m_readbuff[1] == 0x07 && m_readbuff[3] == CMD_TEMP_HUMI) {
                pInfo->update_temperature(((m_readbuff[5] << 8) + m_readbuff[4]) / 100.0f);
                pInfo->update_humidity(((m_readbuff[7] << 8) + m_readbuff[6]) / 100.0f);
                temperature_received = true;
            }
            sequence = NextSequence();
            written = SendCommand(pCh, sequence, CMD_VOLTAGE);
            read = written && Read(pCh);
            if (read && m_readbuff[2] == sequence && m_readlen == 5 && m_readbuff[0] == 0x55 && m_readbuff[1] == 0x04 && m_readbuff[3] == CMD_VOLTAGE) {
                pInfo->update_voltage(m_readbuff[4]);
                voltage_received = true;
            }
        }
        if(temperature_received && voltage_received) pInfo->m_update = 0;
        Close();
        ESP_LOGI(LOG_TAG, "Device at %s was updated", m_addr.toString().c_str());
    }
    else{
        ESP_LOGE(LOG_TAG, "Can not connect to device at %s", m_addr.toString().c_str());
    }
    m_lockDevice.Unlock();
    return pInfo->m_update == 0;
}

IRResult MEIZURemoter::SendIRCode(BLEAddress addr, uint8_t *key, uint16_t keylen, uint8_t *ircode, uint16_t ircodelen){
    IRResult result;
    // Do not let an IR request wait indefinitely behind another connection.
    if(!m_lockDevice.Wait(15000)) {result.status = "busy"; return result;}
    if(!m_irFrames){
        result.status = "no_memory";
    } else if(!Connect(addr)){
        result.status = "connect_failed";
    } else {
        GATTRemoteService *service = GetService(serviceUUID);
        m_irCharacteristic = service ? service->GetCharacteristic(characteristicUUID) : nullptr;
        if(!m_irCharacteristic){
            result.status = "characteristic_missing";
        } else {
            xQueueReset(m_irFrames);
            if(Subscribe(m_irCharacteristic, true)){
                result = SendIR(*this, NextSequence(), key, keylen, ircode, ircodelen);
            } else {
                result.status = "subscribe_failed";
            }
            Subscribe(m_irCharacteristic, false);
        }
        Close();
        m_irCharacteristic = nullptr;
    }
    m_lockDevice.Unlock();
    return result;
}

uint8_t MEIZURemoter::NextSequence(){
    m_sequence = m_sequence >= 254 ? 1 : m_sequence + 1;
    return m_sequence;
}

bool MEIZURemoter::WriteFrame(const uint8_t *data, size_t length){
    return Write(m_irCharacteristic, const_cast<uint8_t *>(data), length, true);
}

uint64_t MEIZURemoter::NowMs(){
    return esp_timer_get_time() / 1000;
}

bool MEIZURemoter::ReceiveFrame(IRFrame &frame, uint32_t timeout_ms){
    const uint64_t deadline = NowMs() + timeout_ms;
    while(ConnectingState() == GATTConnectionState::Connected){
        const uint64_t now = NowMs();
        if(now >= deadline) break;
        uint32_t remaining = deadline - now;
        if(xQueueReceive(m_irFrames, &frame, pdMS_TO_TICKS(remaining < 100 ? remaining : 100)) == pdTRUE) return true;
        // R16 also exposes the most recent reply via Read. This covers a lost
        // notification without retransmitting an IR command.
        if(Read(m_irCharacteristic) && m_readlen >= 4 && m_readlen <= sizeof(frame.data)){
            frame.length = m_readlen;
            memcpy(frame.data, m_readbuff, m_readlen);
            return true;
        }
    }
    return false;
}

bool MEIZURemoter::ReceiveIRCode(BLEAddress addr){
    m_lockDevice.Lock();
    bool result = false;
    if(Connect(addr)){
        GATTRemoteService * pService = GetService(serviceUUID);
        if(pService != nullptr){
            GATTRemoteCharacteristic * pCh = pService->GetCharacteristic(characteristicUUID);
            pCh->RegNotify();
            SendCommand(pCh, NextSequence(), CMD_RECV_IR);
            delay(20000);
            SendCommand(pCh, m_sequence, 0x0b);
            SendCommand(pCh, m_sequence, 0x06);
            pCh->UnregNotify();
        }
        Close();
    }
    m_lockDevice.Unlock();
    return result;
}

void MEIZURemoter::OnNotify(GATTRemoteCharacteristic * pCh, uint8_t * data, uint16_t length){
    if(pCh == m_irCharacteristic && m_irFrames && length >= 4 && length <= 20 &&
            (data[3] == 0x01 || data[3] == 0x02 || data[3] == 0x04)){
        IRFrame frame = {};
        frame.length = length;
        memcpy(frame.data, data, length);
        xQueueSend(m_irFrames, &frame, 0);
    }
    if(!m_irCharacteristic){
        // Keep the existing USB learning diagnostics available.
        ESP_LOGI(LOG_TAG, "OnNotify received %d bytes", length);
        ESP_LOG_BUFFER_HEX(LOG_TAG, data, length);
    }
}

bool MEIZURemoter::SendCommand(GATTRemoteCharacteristic * pCh, uint8_t * data, uint16_t length){
    bool result = false;
    if(pCh != nullptr){
        result = Write(pCh, data, length, true);
    }
    return result;
}

bool MEIZURemoter::SendCommand(GATTRemoteCharacteristic * pCh, uint8_t sequence, uint8_t cmd, uint8_t * stream, uint16_t length){
    if(length > 16 || (length && !stream)) return false;
    uint8_t data[20] = {0x55, static_cast<uint8_t>(length + 3), sequence, cmd};
    if(length) memcpy(data + 4, stream, length);
    return SendCommand(pCh, data, length + 4);
}