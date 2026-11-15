#include "IRProtocol.h"
#include <string.h>

namespace {
constexpr size_t CHUNK = 15; // 4 framing + 1 packet index + 15 data = ATT 20.

bool Write(IRTransport &io, uint8_t seq, uint8_t cmd,
           const uint8_t *payload, size_t length) {
    if (length > 16) return false;
    uint8_t frame[20] = {0x55, static_cast<uint8_t>(length + 3), seq, cmd};
    if (length) memcpy(frame + 4, payload, length);
    return io.WriteFrame(frame, length + 4);
}

bool Packet(IRTransport &io, uint8_t seq, uint8_t index,
            const uint8_t *key, const uint8_t *code, size_t length) {
    uint8_t payload[16] = {index};
    if (index == 0) {
        payload[1] = static_cast<uint8_t>((length + CHUNK - 1) / CHUNK + 1);
        memcpy(payload + 2, key, 9);
        return Write(io, seq, 0x00, payload, 11);
    }
    const size_t offset = (index - 1) * CHUNK;
    if (offset >= length) return false;
    const size_t count = length - offset < CHUNK ? length - offset : CHUNK;
    memcpy(payload + 1, code + offset, count);
    return Write(io, seq, 0x00, payload, count + 1);
}
}

IRResult SendIR(IRTransport &io, uint8_t seq, const uint8_t *key,
                size_t key_length, const uint8_t *code, size_t length,
                uint32_t timeout_ms) {
    IRResult result;
    if (!key || key_length != 9 || length > 3800 || (length && !code) ||
        (length && static_cast<size_t>(key[0] | (key[1] << 8)) != length)) {
        result.status = "invalid_parameters";
        return result;
    }
    const uint64_t deadline = io.NowMs() + timeout_ms;
    if (!Write(io, seq, 0x03, key, key_length)) {
        result.status = "write_failed";
        return result;
    }
    bool uploading = false;
    unsigned int repairs = 0;
    while (io.NowMs() < deadline) {
        IRFrame frame = {};
        const uint64_t now = io.NowMs();
        if (now >= deadline || !io.ReceiveFrame(frame, static_cast<uint32_t>(deadline - now))) break;
        if (frame.length < 4 || frame.data[0] != 0x55 || frame.data[2] != seq) continue;
        const uint8_t cmd = frame.data[3];
        if (cmd != 0x01 && cmd != 0x02 && cmd != 0x04) continue;
        if (frame.data[1] + 1u != frame.length ||
            frame.length != (cmd == 0x02 ? 4u : 5u)) {
            result.status = "invalid_ack";
            return result;
        }
        if (cmd == 0x04 && !uploading) {
            if (frame.data[4] != 0) {
                result.confirmed = true;
                result.cached = true;
                result.ack = 0x04;
                result.status = "ok";
                return result;
            }
            if (!length) {
                result.status = "cache_miss";
                return result;
            }
            uploading = true;
            const size_t packets = (length + CHUNK - 1) / CHUNK;
            for (size_t index = 0; index <= packets; ++index) {
                if (io.NowMs() >= deadline) return result;
                if (!Packet(io, seq, index, key, code, length)) {
                    result.status = "write_failed";
                    return result;
                }
            }
        } else if (cmd == 0x02 && uploading) {
            result.confirmed = true;
            result.ack = 0x02;
            result.status = "ok";
            return result;
        } else if (cmd == 0x01 && uploading) {
            const size_t packets = (length + CHUNK - 1) / CHUNK;
            if (frame.data[4] > packets || ++repairs > packets * 2 + 2) {
                result.status = "invalid_ack";
                return result;
            }
            if (!Packet(io, seq, frame.data[4], key, code, length)) {
                result.status = "write_failed";
                return result;
            }
        }
    }
    return result;
}
