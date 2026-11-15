/* Created by @georgezhao2010; maintained by this fork. */
#ifndef _KERNEL_H_
#define _KERNEL_H_
#include <map>
#include <set>
#include <deque>
#include <memory>
#include <vector>
#include <atomic>
#include <cJSON.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "SocketServer.h"
#include "SocketClient.h"
#include "Remoter.h"
#include "RemoterInfo.h"
#include "SemaphoreLock.h"

enum class KernelMode {Normal, ReadyBind, Bind};
struct BleJob {
    enum Kind {IR, Learn};
    Kind kind;
    BLEAddress address;
    uint32_t generation;
    std::vector<uint8_t> key, code;
    CommandState state;
    SemaphoreLock completed;
    IRResult result;
    std::atomic<bool> cancelled{false};
    uint32_t client_id=0,window_ms=15000;
    std::string request_id;
    BleJob(Kind k, BLEAddress a, uint32_t g, uint64_t now):
        kind(k),address(a),generation(g),state(now + 15000),completed(true) {}
};

class Kernel {
public:
    Kernel();
    bool Init(bool recovery=false);
    void Loop();
    bool PauseOTA(bool pause);
    bool OTAIdle();
    bool Ready() const {return m_recovery || m_worker != nullptr;}
    static void Restart();
private:
    SocketServer m_server;
    KernelMode m_mode = KernelMode::Normal;
    uint8_t m_nUpdateInterval = 5;
    uint32_t m_generation = 0;
    MEIZURemoter *m_pRemoterInterface = nullptr;
    std::map<BLEAddress,MEIZURemoterInfo> m_mapInfos;
    std::deque<std::shared_ptr<BleJob>> m_jobs;
    std::shared_ptr<BleJob> m_learning;
    std::set<BLEAddress> m_pendingBinds;
    bool m_startScan = false;
    SemaphoreLock m_lock;
    TaskHandle_t m_worker = nullptr;
    const char *m_activeJob = "idle";
    std::atomic<bool> m_updating{false};
    bool m_recovery=false;
    uint32_t m_expiredJobs = 0;
    static uint64_t Now(){return esp_timer_get_time() / 1000;}
    static void OnClientMessage(void *,SocketClient *,char *,int);
    static void task_Restart(void *);
    static void Worker(void *);
    static void OnBindModeChanged(void *,bool);
    static void OnDeviceFound(void *,esp_bd_addr_t *);
    static void OnRSSIRead(void *,esp_bd_addr_t *,int8_t);
    static uint8_t *ReadStreamFromASC(char *);
    static bool ValidAddress(const char *);
    static char *Reply(const char *,cJSON *);
    static cJSON *DeviceJSON(const MEIZURemoterInfo &,uint64_t);
    void Wake();
    bool StartBinding();
    bool Enqueue(const std::shared_ptr<BleJob> &);
    IRResult Await(const std::shared_ptr<BleJob> &);
    void RunJob(const std::shared_ptr<BleJob> &);
    void LearnEvent(const std::shared_ptr<BleJob> &,const char *,size_t=0,size_t=0,const LearnResult * = nullptr);
    void BindDevice(BLEAddress);
    void PollDevice(MEIZURemoterInfo);
    void BoardcastInfo(const MEIZURemoterInfo &);
    void SaveAddress(); // Caller holds m_lock; NVS only, never BLE or socket IO.
    char *process_message_configinfo();
    char *process_message_devices();
    char *process_message_diagnostics();
    char *process_message_refresh(cJSON *);
    char *process_message_sendir(cJSON *);
    char *process_message_recvir(cJSON *);
    char *process_message_learn(SocketClient *,cJSON *,bool);
    char *process_message_bind_threshold(cJSON *);
    char *process_message_bind();
    char *process_message_removebind(cJSON *);
    char *process_message_subscribe(SocketClient *);
    char *process_message_setinterval(cJSON *);
};
#endif
