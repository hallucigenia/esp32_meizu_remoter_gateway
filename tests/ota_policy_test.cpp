#include "OtaPolicy.h"
#include <cassert>
#include <iostream>
using namespace meizu_ota;
int main(){
    Image image;image.id=std::string(32,'a');image.version="0.7.1";image.size=1300000;
    image.sha256=std::string(64,'b');image.timestamp=1794750000;
    image.url="https://github.com/hallucigenia/esp32_meizu_remoter_gateway/releases/download/v0.7.1/firmware.ota.bin";
    assert(Validate(image,SLOT_SIZE)==nullptr);
    for(const auto &url:{std::string("http://github.com/x"),std::string("https://evil.invalid/firmware.ota.bin"),image.url+"?next=http://evil.invalid"}){
        auto changed=image;changed.url=url;assert(Validate(changed,SLOT_SIZE)!=nullptr);
    }
    auto changed=image;changed.size=SLOT_SIZE+1;assert(Validate(changed,SLOT_SIZE)!=nullptr);
    changed=image;changed.version="0.7.2";assert(Validate(changed,SLOT_SIZE)!=nullptr);
    changed=image;changed.id="bad\nvalue";assert(Validate(changed,SLOT_SIZE)!=nullptr);
    changed=image;changed.timestamp=0;assert(Validate(changed,SLOT_SIZE)!=nullptr);
    assert(!Hex(std::string(64,'A'),64));assert(!Version("0.7.1/../../"));assert(!Version("0.7.1\n"));
    auto message=Authorization("start","ABC123",std::string(64,'c'),image);
    assert(message=="meizu-ota-v1\nstart\nABC123\n"+std::string(64,'c')+"\n"+std::string(32,'a')+
        "\n0.7.1\n1300000\n"+std::string(64,'b')+"\n"+image.url+"\n1794750000");
    assert(message!=Authorization("cancel","ABC123",std::string(64,'c'),image));
    assert(message!=Authorization("start","OTHER",std::string(64,'c'),image));
    std::cout<<"OTA policy and authorization tests passed\n";
}
