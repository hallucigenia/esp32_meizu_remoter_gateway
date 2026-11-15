#ifndef MEIZU_BACKOFF_H
#define MEIZU_BACKOFF_H
#include <stdint.h>
static inline uint32_t RetryDelay(uint32_t failures,uint32_t first_ms,uint32_t maximum_ms){
    uint32_t delay = first_ms;
    for(uint32_t n=1;n<failures && delay<maximum_ms;++n)
        delay = delay>maximum_ms/2 ? maximum_ms : delay*2;
    return delay>maximum_ms ? maximum_ms : delay;
}
#endif
