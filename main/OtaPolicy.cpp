#include "OtaPolicy.h"
#include <ctype.h>

namespace meizu_ota {
bool Hex(const std::string &value, size_t length) {
    if(value.size() != length) return false;
    for(char c : value) if(!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
bool Version(const std::string &value) {
    if(value.empty() || value.size() > 31 || !isdigit(static_cast<unsigned char>(value[0]))) return false;
    for(char c : value) if(!isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-') return false;
    return true;
}
bool TrustedURL(const std::string &url, const std::string &version) {
    // No arbitrary URLs, mutable "latest" images, or credentials in URLs.
    return url == "https://github.com/hallucigenia/esp32_meizu_remoter_gateway/releases/download/v" +
                  version + "/firmware.ota.bin";
}
const char *Validate(const Image &image, size_t capacity) {
    if(!Hex(image.id,32) || !Version(image.version) || !Hex(image.sha256,64) ||
       image.timestamp < 1700000000ULL || image.timestamp > 4102444800ULL) return "invalid_parameters";
    if(image.size < 1024 || image.size > capacity || image.size > SLOT_SIZE) return "image_too_large";
    if(!TrustedURL(image.url,image.version)) return "untrusted_url";
    return nullptr;
}
std::string Authorization(const char *operation, const std::string &serial,
                          const std::string &nonce, const Image &image) {
    return std::string("meizu-ota-v1\n") + operation + "\n" + serial + "\n" + nonce + "\n" +
           image.id + "\n" + image.version + "\n" + std::to_string(image.size) + "\n" +
           image.sha256 + "\n" + image.url + "\n" + std::to_string(image.timestamp);
}
}
