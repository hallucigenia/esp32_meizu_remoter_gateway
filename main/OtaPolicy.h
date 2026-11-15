#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>

namespace meizu_ota {
static constexpr size_t SLOT_SIZE = 0x1c0000;
static constexpr const char *BOARD = "esp32-4m-ab-v1";
struct Image {
    std::string id, version, url, sha256;
    size_t size = 0;
    uint64_t timestamp = 0;
};
bool Hex(const std::string &value, size_t length);
bool Version(const std::string &value);
bool TrustedURL(const std::string &url, const std::string &version);
const char *Validate(const Image &image, size_t capacity);
std::string Authorization(const char *operation, const std::string &serial,
                          const std::string &nonce, const Image &image);
}
