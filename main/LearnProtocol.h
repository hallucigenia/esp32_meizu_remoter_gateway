#ifndef MEIZU_LEARN_PROTOCOL_H
#define MEIZU_LEARN_PROTOCOL_H
#include "IRProtocol.h"
#include <functional>
#include <vector>

struct LearnResult {
    const char *status = "ready_timeout";
    std::vector<uint8_t> raw;
};
using LearnCancelled = std::function<bool()>;
using LearnProgress = std::function<void(const char *, size_t, size_t)>;

// Receive only: this transaction never uploads or transmits an infrared code.
LearnResult LearnIR(IRTransport &, uint8_t, uint32_t, LearnCancelled, LearnProgress);
#endif
