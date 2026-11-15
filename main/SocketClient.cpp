/*
 * Created by @georgezhao2010
 * https://github.com/georgezhao2010/esp32_meizu_remoter_gateway/
 * involute@sina.com
 */
#include <esp_log.h>
#include <lwip/sockets.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <cJSON.h>
#include <ctype.h>
#include "SocketClient.h"

#define LOG_TAG "SocketClient"


SocketClient::SocketClient(int sock, const char * pAddr, int nPort,  SocketServer * pServer, on_client_close on_close_cb,
                           void * pKernel, on_client_message on_message_cb){
    m_pKernel = pKernel;
    m_onmessage_cb = on_message_cb;
    m_socket = sock;
    int len = strlen(pAddr);
    m_pAddr = (char *)malloc(len + 1);
    memcpy(m_pAddr, pAddr, len + 1);
    m_nPort = nPort;
    struct timeval timeout = {CONFIG_SOCKET_TIMEOUT, 0};
    setsockopt(m_socket,SOL_SOCKET,SO_RCVTIMEO,(char*)&timeout,sizeof(struct timeval));
    struct timeval send_timeout = {5, 0};
    setsockopt(m_socket, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    m_pServer = pServer;
    m_onclose_cb = on_close_cb;
}

SocketClient::~SocketClient(){
    free(m_pAddr);
    if(m_socket > 0){
        shutdown(m_socket, SHUT_RDWR);
        closesocket(m_socket);
    }
}

void SocketClient::task_Receive(void * param){
    SocketClient * pClient = (SocketClient *)param;
    char buff[1024];
    size_t used = 0;
    int counter = 0;
    bool invalid = false;
    while(!invalid){
        int len = recv(pClient->m_socket, buff + used, sizeof(buff) - used - 1, 0);
        if(len > 0){
            counter = 0;
            used += len;
            buff[used] = 0;
            while(used){
                size_t whitespace = 0;
                while(whitespace < used && isspace(static_cast<unsigned char>(buff[whitespace]))) ++whitespace;
                if(whitespace){memmove(buff, buff + whitespace, used - whitespace + 1); used -= whitespace;}
                if(!used) break;
                const char *end = nullptr;
                cJSON *message = cJSON_ParseWithOpts(buff, &end, false);
                if(!message){
                    if(used >= 511) invalid = true;
                    break; // A complete JSON object may span multiple recv calls.
                }
                size_t consumed = end - buff;
                bool object = cJSON_IsObject(message);
                cJSON_Delete(message);
                if(!object || consumed == 0 || consumed > 511){invalid = true; break;}
                char next = buff[consumed];
                buff[consumed] = 0;
                if(pClient->m_onmessage_cb) pClient->m_onmessage_cb(pClient->m_pKernel, pClient, buff, consumed);
                buff[consumed] = next;
                memmove(buff, buff + consumed, used - consumed + 1);
                used -= consumed;
            }
        } else if(len == 0){
            break;
        } else if(errno == EINTR){
            continue;
        } else if(errno != EAGAIN || ++counter >= 4){
            break;
        }
    }
    pClient->m_onclose_cb(pClient->m_pServer, pClient);
    vTaskDelete(NULL);
}

void SocketClient::Send(char * pMessage, int nLen){
    int sent = 0;
    while(sent < nLen){
        int count = send(m_socket, pMessage + sent, nLen - sent, 0);
        if(count < 0 && errno == EINTR) continue;
        if(count <= 0) break;
        sent += count;
    }
}

void SocketClient::Start(){
    xTaskCreate(task_Receive, "task_Receive", 4096, this, tskIDLE_PRIORITY, NULL);
}