/*
 * Created by @georgezhao2010
 * https://github.com/georgezhao2010/esp32_meizu_remoter_gateway/
 * involute@sina.com
 */
#include "SemaphoreLock.h"
#include "esp_log.h"

SemaphoreLock::SemaphoreLock(bool initLock){
    m_semaphore = xSemaphoreCreateCounting(1, 0);
    xSemaphoreGive(m_semaphore);
    if(initLock){
        xSemaphoreTake(m_semaphore, portMAX_DELAY);
    }
}

SemaphoreLock::~SemaphoreLock(){
    vSemaphoreDelete(m_semaphore);
}

void SemaphoreLock::Lock(){
    xSemaphoreTake(m_semaphore, portMAX_DELAY);
}

void SemaphoreLock::Unlock(){
    xSemaphoreGive(m_semaphore);
}

bool SemaphoreLock::Wait(uint32_t timeout_ms){
    return xSemaphoreTake(m_semaphore, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void SemaphoreLock::Reset(){
    while(xSemaphoreTake(m_semaphore, 0) == pdTRUE) {}
}
