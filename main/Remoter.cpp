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

bool MEIZURemoter::ReadAllInfo(MEIZURemoterInfo *info){
    m_lockDevice.Lock();
    bool measured = false;
    if(Connect(info->m_addr)){
        GATTRemoteService *service = GetService(infoUUID);
        if(service){
            if(!info->has_manufacturer()){
                auto *ch = service->GetCharacteristic(manufacturerUUID);
                if(Read(ch)) info->update_manufacturer((char *)m_readbuff,m_readlen);
            }
            if(!info->has_model()){
                auto *ch = service->GetCharacteristic(modelUUID);
                if(Read(ch)) info->update_model((char *)m_readbuff,m_readlen);
            }
            if(!info->has_firmware()){
                auto *ch = service->GetCharacteristic(firmwareUUID);
                if(Read(ch)) info->update_firmware((char *)m_readbuff,m_readlen);
            }
        }
        service = GetService(serviceUUID);
        if(service){
            auto *ch = service->GetCharacteristic(characteristicUUID);
            uint8_t sequence = NextSequence();
            bool temperature = SendCommand(ch,sequence,CMD_TEMP_HUMI) && Read(ch) &&
                m_readlen == 8 && m_readbuff[0] == 0x55 && m_readbuff[1] == 7 &&
                m_readbuff[2] == sequence && m_readbuff[3] == CMD_TEMP_HUMI;
            float temperature_value = 0,humidity_value = 0;
            if(temperature){
                temperature_value = ((m_readbuff[5] << 8) + m_readbuff[4]) / 100.0f;
                humidity_value = ((m_readbuff[7] << 8) + m_readbuff[6]) / 100.0f;
            }
            sequence = NextSequence();
            bool voltage = SendCommand(ch,sequence,CMD_VOLTAGE) && Read(ch) &&
                m_readlen == 5 && m_readbuff[0] == 0x55 && m_readbuff[1] == 4 &&
                m_readbuff[2] == sequence && m_readbuff[3] == CMD_VOLTAGE;
            measured = temperature && voltage;
            if(measured){
                info->update_temperature(temperature_value);
                info->update_humidity(humidity_value);
                info->update_voltage(m_readbuff[4]);
            }
        }
        Close();
    }
    ESP_LOGI(LOG_TAG,"Measurement %s for %s",measured ? "complete" : "failed",info->m_addr.toString().c_str());
    m_lockDevice.Unlock();
    return measured;
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

LearnResult MEIZURemoter::LearnCode(BLEAddress addr,uint32_t window_ms,LearnCancelled cancelled,LearnProgress progress){
    LearnResult result;
    if(!m_lockDevice.Wait(15000)){result.status="busy";return result;}
    if(cancelled()) result.status="cancelled";
    else if(!Connect(addr)) result.status="connect_failed";
    else {
        auto *service=GetService(serviceUUID);
        m_irCharacteristic=service ? service->GetCharacteristic(characteristicUUID) : nullptr;
        if(!m_irCharacteristic) result.status="characteristic_missing";
        else if(!m_irFrames) result.status="no_memory";
        else {
            xQueueReset(m_irFrames);
            if(Subscribe(m_irCharacteristic,true)) result=LearnIR(*this,NextSequence(),window_ms,cancelled,progress);
            else result.status="subscribe_failed";
            Subscribe(m_irCharacteristic,false);
        }
        Close();m_irCharacteristic=nullptr;
    }
    m_lockDevice.Unlock();return result;
}

bool MEIZURemoter::ReceiveIRCode(BLEAddress addr){
    // Preserve the serial diagnostic entry point; use the same complete receiver.
    auto result=LearnCode(addr,15000,[]{return false;},[](const char *,size_t,size_t){});
    if(!result.raw.empty()) ESP_LOG_BUFFER_HEX(LOG_TAG,result.raw.data(),result.raw.size());
    return strcmp(result.status,"ok")==0;
}

void MEIZURemoter::OnNotify(GATTRemoteCharacteristic * pCh, uint8_t * data, uint16_t length){
    if(pCh == m_irCharacteristic && m_irFrames && length >= 4 && length <= 20 &&
            (data[3] == 0x01 || data[3] == 0x02 || data[3] == 0x04 ||
             data[3] == 0x07 || data[3] == 0x08 || data[3] == 0x09)){
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