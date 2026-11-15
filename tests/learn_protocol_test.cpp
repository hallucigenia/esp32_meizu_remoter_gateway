#include "LearnProtocol.h"
#include <assert.h>
#include <deque>
#include <stdio.h>
#include <string.h>

class Fake:public IRTransport {
public:
    uint64_t now=0;
    bool alive=true;
    std::deque<std::vector<uint8_t>> frames;
    std::vector<std::vector<uint8_t>> writes;
    bool WriteFrame(const uint8_t *data,size_t length) override {
        assert(length<=20);writes.emplace_back(data,data+length);return true;
    }
    bool ReceiveFrame(IRFrame &frame,uint32_t timeout) override {
        if(frames.empty()){now+=timeout;return false;}
        auto data=frames.front();frames.pop_front();
        frame.length=data.size();memcpy(frame.data,data.data(),data.size());++now;return true;
    }
    uint64_t NowMs() override {return now;}
    bool LinkAlive() override {return alive;}
    void Capture(size_t length){
        frames.push_back({0x55,3,7,7});
        frames.push_back({0x55,7,7,9,0,static_cast<uint8_t>(1+(length+14)/15),static_cast<uint8_t>(length),static_cast<uint8_t>(length>>8)});
        for(size_t i=0;i<length;i+=15){
            size_t count=length-i<15 ? length-i : 15;
            std::vector<uint8_t> frame={0x55,static_cast<uint8_t>(count+4),7,9,static_cast<uint8_t>(i/15+1)};
            for(size_t j=0;j<count;++j) frame.push_back((i+j)&255);
            frames.push_back(frame);
        }
    }
    void ReceiveOnly(){for(auto frame:writes) assert(frame[3]!=0 && frame[3]!=3);}
};

int main(){
    {
        Fake io;io.alive=false;
        auto result=LearnIR(io,7,15000,[]{return false;},[](const char *,size_t,size_t){});
        assert(!strcmp(result.status,"disconnected") && io.now==0 && io.writes.back()[3]==6);
    }
    for(size_t length:{size_t(15),size_t(30),size_t(40),size_t(1500)}){
        Fake io;io.Capture(length);bool ready=false;
        auto result=LearnIR(io,7,15000,[]{return false;},[&](const char *phase,size_t,size_t total){if(!strcmp(phase,"ready")){ready=true;assert(total==15000);}});
        assert(ready && !strcmp(result.status,"ok") && result.raw.size()==length);
        for(size_t i=0;i<length;++i) assert(result.raw[i]==(i&255));
        assert(io.writes[io.writes.size()-2][3]==0x0b && io.writes.back()[3]==0x06);io.ReceiveOnly();
    }
    {
        Fake io;bool ready=false;
        auto result=LearnIR(io,7,15000,[]{return false;},[&](const char *,size_t,size_t){ready=true;});
        assert(!ready && !strcmp(result.status,"ready_timeout") && result.raw.empty() && io.now==8000);
        assert(io.writes.back()[3]==6);io.ReceiveOnly();
    }
    {
        Fake io;io.frames={{0x55,3,6,7},{0x55,3,7,7}};
        auto result=LearnIR(io,7,1000,[]{return false;},[](const char *,size_t,size_t){});
        assert(!strcmp(result.status,"learning_timeout") && io.now>=1000 && io.now<2000);io.ReceiveOnly();
    }
    {
        Fake io;io.Capture(40);bool cancel=false;
        auto result=LearnIR(io,7,15000,[&]{return cancel;},[&](const char *,size_t,size_t){cancel=true;});
        assert(!strcmp(result.status,"cancelled") && io.writes.back()[3]==6);io.ReceiveOnly();
    }
    {
        Fake io;io.Capture(40);io.frames.insert(io.frames.begin()+1,{0x55,3,99,8});
        auto packet=io.frames[3];io.frames.erase(io.frames.begin()+3);io.frames.push_back(packet);
        // Out-of-order packets trigger an explicit request for the missing packet.
        auto result=LearnIR(io,7,15000,[]{return false;},[](const char *,size_t,size_t){});
        assert(strcmp(result.status,"ok")!=0);
        bool repair=false;for(auto frame:io.writes) if(frame[3]==10 && frame[4]==1) repair=true;
        assert(repair && io.writes.back()[3]==6);io.ReceiveOnly();
    }
    {
        Fake io;io.Capture(40);
        auto one=io.frames[2],two=io.frames[3],three=io.frames[4];
        io.frames.erase(io.frames.begin()+2);
        io.frames.push_back(one);io.frames.push_back(two);io.frames.push_back(three);
        auto result=LearnIR(io,7,15000,[]{return false;},[](const char *,size_t,size_t){});
        assert(!strcmp(result.status,"ok") && result.raw.size()==40);
        bool repair=false;for(auto frame:io.writes) if(frame[3]==10 && frame[4]==1) repair=true;
        assert(repair);io.ReceiveOnly();
    }
    {
        Fake io;io.frames={{0x55,3,7,7},{0x55,7,7,9,0,2,255,255}};
        auto result=LearnIR(io,7,15000,[]{return false;},[](const char *,size_t,size_t){});
        assert(!strcmp(result.status,"invalid_header") && result.raw.empty());io.ReceiveOnly();
    }
    puts("Learning protocol tests passed");
}
