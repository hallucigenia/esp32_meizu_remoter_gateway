#ifndef MEIZU_IR_PROTOCOL_H
#define MEIZU_IR_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

struct IRFrame {
    uint8_t data[20];
    size_t length;
};

struct IRResult {
    bool confirmed = false;
    bool cached = false;
    uint8_t ack = 0;
    const char *status = "ack_timeout";
};

class IRTransport {
public:
    virtual ~IRTransport() {}
    virtual bool WriteFrame(const uint8_t *data, size_t length) = 0;
    virtual bool ReceiveFrame(IRFrame &frame, uint32_t timeout_ms) = 0;
    virtual uint64_t NowMs() = 0;
};

// One command only. Never retry a send after an ambiguous timeout: Auto and
// cycle keys are not idempotent. Only explicitly requested missing fragments
// may be retransmitted within the same upload transaction.
IRResult SendIR(IRTransport &transport, uint8_t sequence, const uint8_t *key,
                size_t key_length, const uint8_t *code, size_t code_length,
                uint32_t timeout_ms = 6000);

#endif
