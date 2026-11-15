#pragma once
#include "OtaPolicy.h"
#include "SemaphoreLock.h"
#include <atomic>
#include <cJSON.h>
#include <esp_ota_ops.h>

class Kernel;
class SocketClient;

class OtaManager {
public:
    explicit OtaManager(Kernel &kernel): kernel_(kernel) {}
    bool Init();
    bool Recovery() const { return recovery_; }
    void Tick(bool kernel_ready);
    void Console();
    cJSON *Handle(SocketClient *client, const char *type, cJSON *data);
    cJSON *Status();
    static void RejectPending();
private:
    Kernel &kernel_;
    SemaphoreLock lock_;
    uint8_t key_[32] = {};
    bool paired_ = false, recovery_ = false, rollback_seen_ = false;
    std::atomic<bool> confirmed_{false};
    uint64_t boot_ms_ = 0, healthy_since_ = 0, nonce_deadline_ = 0;
    uint32_t nonce_owner_ = 0;
    uint32_t target_address_ = 0;
    std::string nonce_, phase_ = "idle", error_;
    meizu_ota::Image image_;
    size_t written_ = 0;
    std::atomic<bool> active_{false}, cancelled_{false};
    esp_ota_img_states_t initial_state_ = ESP_OTA_IMG_UNDEFINED;
    static void Worker(void *context);
    void Download();
    void Fail(const char *reason);
    bool Persist(const char *phase);
    bool Authenticate(SocketClient *client, const char *operation, const meizu_ota::Image &image, const char *auth);
    bool Layout() const;
};
extern OtaManager ota;
