#include "RemoterInfo.h"
#include "Recovery.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <string>
#include <cstdio>

int main(){
    MEIZURemoterInfo missing;
    missing.update_manufacturer("Meizu",5);
    cJSON *json = missing.ToJSON();
    assert(json && !cJSON_GetObjectItem(json,"model") && !cJSON_GetObjectItem(json,"temperature"));
    cJSON_Delete(json);
    const char *escaped = "Me\"i\\zu\n";
    missing.update_manufacturer(escaped,strlen(escaped));
    missing.update_temperature(0);missing.update_humidity(0);missing.update_voltage(29);
    json = missing.ToJSON();char *wire = cJSON_PrintUnformatted(json);cJSON *parsed = cJSON_Parse(wire);
    assert(parsed && strcmp(cJSON_GetObjectItem(parsed,"manufacturer")->valuestring,escaped)==0);
    assert(cJSON_GetObjectItem(parsed,"temperature")->valuedouble==0);
    assert(fabs(cJSON_GetObjectItem(parsed,"voltage")->valuedouble-2.9)<.001);
    cJSON_free(wire);cJSON_Delete(parsed);cJSON_Delete(json);
    for(int n=0;n<10000;++n){
        MEIZURemoterInfo info;
        std::string long_value(255,'x');info.update_manufacturer(long_value.data(),long_value.size());
        info.update_model("R16",3);info.update_firmware("v_test",6);
        MEIZURemoterInfo copy = info;json = copy.ToJSON();assert(json);cJSON_Delete(json);
    }
    RecoveryState recovery;uint64_t now = 1234;
    assert(recovery.Due(now) && !recovery.available && !recovery.has_sample);
    for(int n=0;n<100;++n){
        assert(recovery.Due(now));recovery.Failure(now);
        assert(!recovery.available && recovery.next_attempt_ms>now);
        assert(recovery.next_attempt_ms-now<=900000);
        assert(!recovery.Due(recovery.next_attempt_ms-1));now = recovery.next_attempt_ms;
    }
    recovery.Success(now,300000);assert(recovery.available && recovery.has_sample && recovery.failures==0);
    uint64_t measured = recovery.last_success_ms;recovery.Failure(now+300000);
    assert(recovery.has_sample && recovery.last_success_ms==measured && !recovery.available);
    assert(recovery.next_attempt_ms==now+305000);
    assert(RetryDelay(100,1000,30000)==30000);
    CommandState expired(100);assert(!expired.Start(100) && expired.phase==CommandState::Cancelled);
    assert(!expired.Start(0));
    CommandState cancelled(100);assert(cancelled.CancelQueued() && !cancelled.Start(1));
    CommandState running(100);assert(running.Start(99) && !running.CancelQueued() && !running.Start(99));
    puts("Metadata, ownership, recovery beyond five failures, and command deadlines passed");
}
