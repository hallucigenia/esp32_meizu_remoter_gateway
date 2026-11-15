#include "LearnProtocol.h"
#include <string.h>

namespace {
bool Write(IRTransport &io, uint8_t seq, uint8_t cmd, int index = -1){
    uint8_t frame[5] = {0x55, static_cast<uint8_t>(index < 0 ? 3 : 4), seq, cmd,
                        static_cast<uint8_t>(index)};
    return io.WriteFrame(frame, index < 0 ? 4 : 5);
}
struct Stop {
    IRTransport &io;
    uint8_t seq;
    ~Stop(){Write(io,seq,0x06);}
};
}

LearnResult LearnIR(IRTransport &io,uint8_t seq,uint32_t window_ms,
                    LearnCancelled cancelled,LearnProgress progress){
    LearnResult result;
    if(window_ms < 1000 || window_ms > 15000){result.status="invalid_parameters";return result;}
    if(cancelled()){result.status="cancelled";return result;}
    if(!Write(io,seq,0x05)){result.status="write_failed";return result;}
    Stop stop{io,seq};
    uint64_t deadline=io.NowMs()+8000,hard_deadline=0;
    bool ready=false;
    size_t expected=0,packets=0,length=0,repairs=0;
    while(io.NowMs()<deadline){
        if(cancelled()){result.status="cancelled";return result;}
        if(!io.LinkAlive()){result.status="disconnected";result.raw.clear();return result;}
        IRFrame frame={};
        if(!io.ReceiveFrame(frame,200)) continue;
        if(frame.length<4 || frame.data[0]!=0x55 || frame.data[2]!=seq) continue;
        uint8_t cmd=frame.data[3];
        if(cmd!=0x07 && cmd!=0x08 && cmd!=0x09) continue;
        if(frame.data[1]+1u!=frame.length){result.status="invalid_frame";return result;}
        if(cmd==0x08){result.status="learning_timeout";return result;}
        if(cmd==0x07){
            if(frame.length!=4){result.status="invalid_frame";return result;}
            if(!ready){
                ready=true;deadline=io.NowMs()+window_ms;hard_deadline=io.NowMs()+30000;
                result.status="learning_timeout";progress("ready",0,window_ms);
            }
            continue;
        }
        // A previous session's data cannot make a new session ready.
        if(!ready || frame.length<5) continue;
        size_t index=frame.data[4];
        if(index!=expected){
            if(++repairs>204 || !Write(io,seq,0x0a,expected)){result.status="repair_failed";return result;}
            continue;
        }
        if(index==0){
            if(frame.length!=8){result.status="invalid_header";return result;}
            packets=frame.data[5];length=frame.data[6] | (frame.data[7]<<8);
            if(length<10 || length>1500 || packets!=1+(length+14)/15){result.status="invalid_header";return result;}
            result.raw.reserve(length);
        }else{
            size_t remaining=length-result.raw.size(),count=remaining<15 ? remaining : 15;
            if(frame.length!=count+5){result.status="length_mismatch";return result;}
            result.raw.insert(result.raw.end(),frame.data+5,frame.data+frame.length);
        }
        ++expected;
        progress("receiving",result.raw.size(),length);
        if(expected==packets){
            if(cancelled()){result.status="cancelled";result.raw.clear();return result;}
            if(!Write(io,seq,0x0b)){result.status="write_failed";return result;}
            result.status="ok";return result;
        }
        uint64_t fragment_deadline=io.NowMs()+5000;
        deadline=fragment_deadline<hard_deadline ? fragment_deadline : hard_deadline;
    }
    result.raw.clear();
    return result;
}
