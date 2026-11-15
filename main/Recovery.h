#ifndef MEIZU_RECOVERY_H
#define MEIZU_RECOVERY_H
#include <stdint.h>

#include "Backoff.h"

struct RecoveryState {
    bool available = false;
    bool has_sample = false;
    uint32_t failures = 0;
    uint64_t last_success_ms = 0;
    uint64_t next_attempt_ms = 0;
    bool Due(uint64_t now) const {return now >= next_attempt_ms;}
    void Success(uint64_t now, uint32_t interval_ms){
        available = has_sample = true;
        failures = 0;
        last_success_ms = now;
        next_attempt_ms = now + interval_ms;
    }
    void Failure(uint64_t now){
        available = false;
        if(failures < 32) ++failures;
        next_attempt_ms = now + RetryDelay(failures, 5000, 900000);
    }
};

// Access under the scheduler lock. Expired queued requests must not emit later.
struct CommandState {
    enum Phase {Queued, Running, Done, Cancelled};
    Phase phase = Queued;
    uint64_t start_deadline_ms;
    explicit CommandState(uint64_t deadline): start_deadline_ms(deadline) {}
    bool Start(uint64_t now){
        if(phase != Queued) return false;
        if(now >= start_deadline_ms){phase = Cancelled; return false;}
        phase = Running;
        return true;
    }
    bool CancelQueued(){
        if(phase != Queued) return false;
        phase = Cancelled;
        return true;
    }
};
#endif
