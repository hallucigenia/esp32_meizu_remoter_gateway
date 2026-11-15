#include "IRProtocol.h"
#include <assert.h>
#include <deque>
#include <vector>
#include <string.h>
#include <stdio.h>

class Fake : public IRTransport {
public:
    uint64_t now = 0;
    int fail_write = -1;
    std::deque<std::vector<uint8_t>> replies;
    std::vector<std::vector<uint8_t>> writes;
    bool WriteFrame(const uint8_t *data, size_t length) override {
        assert(length <= 20);
        writes.emplace_back(data, data + length);
        return static_cast<int>(writes.size()) != fail_write;
    }
    bool ReceiveFrame(IRFrame &frame, uint32_t timeout) override {
        if(replies.empty()) {now += timeout; return false;}
        auto value = replies.front(); replies.pop_front();
        frame.length = value.size(); memcpy(frame.data, value.data(), value.size());
        ++now;
        return true;
    }
    uint64_t NowMs() override {return now;}
};

int main(){
    uint8_t key[9] = {92, 0, 0x1c};
    std::vector<uint8_t> code(92);
    for(size_t i=0; i<code.size(); ++i) code[i] = i;
    {
        Fake io; io.replies = {{0x55,4,6,4,1}, {0x55,4,7,4,1}};
        IRResult r = SendIR(io,7,key,9,nullptr,0);
        assert(r.confirmed && r.cached && r.ack == 4 && io.writes.size() == 1);
    }
    {
        Fake io; io.replies = {{0x55,4,7,4,0}};
        IRResult r = SendIR(io,7,key,9,nullptr,0);
        assert(!r.confirmed && strcmp(r.status,"cache_miss") == 0 && io.writes.size() == 1);
    }
    for(size_t length : {size_t(15),size_t(30),size_t(92)}){
        Fake io; key[0] = length;
        io.replies = {{0x55,3,7,2}, {0x55,4,7,4,0}, {0x55,3,7,2}};
        IRResult r = SendIR(io,7,key,9,code.data(),length);
        assert(r.confirmed && !r.cached && r.ack == 2);
        const size_t packets = (length+14)/15;
        assert(io.writes.size() == packets+2 && io.writes[1][5] == packets+1);
        std::vector<uint8_t> reconstructed;
        for(size_t i=2; i<io.writes.size(); ++i){
            const auto &frame = io.writes[i];
            assert(frame[4] == i-1 && frame[3] == 0 && frame[2] == 7);
            reconstructed.insert(reconstructed.end(), frame.begin()+5,frame.end());
        }
        assert(reconstructed == std::vector<uint8_t>(code.begin(),code.begin()+length));
    }
    key[0] = 92;
    {
        Fake io; io.replies = {{0x55,4,7,4,0}, {0x55,4,7,1,7}, {0x55,3,7,2}};
        IRResult r = SendIR(io,7,key,9,code.data(),92);
        assert(r.confirmed && io.writes.back()[4] == 7 && io.writes.back().size() == 7);
    }
    {
        Fake io; io.replies = {{0x55,4,7,4,0}, {0x55,4,7,1,8}};
        IRResult r = SendIR(io,7,key,9,code.data(),92);
        assert(!r.confirmed && strcmp(r.status,"invalid_ack") == 0);
    }
    {
        Fake io; io.replies = {{0x55,4,7,4,0}};
        IRResult r = SendIR(io,7,key,9,code.data(),92);
        assert(!r.confirmed && strcmp(r.status,"ack_timeout") == 0 && io.writes.size() == 9);
    }
    {
        Fake io; io.replies = {{0x55,3,7,4,1}};
        assert(!SendIR(io,7,key,9,nullptr,0).confirmed);
    }
    {
        Fake io; io.fail_write = 1;
        IRResult r = SendIR(io,7,key,9,nullptr,0);
        assert(!r.confirmed && strcmp(r.status,"write_failed") == 0);
    }
    {
        Fake io;
        IRResult r = SendIR(io,7,key,9,code.data(),30);
        assert(!r.confirmed && io.writes.empty());
    }
    puts("IR protocol tests passed");
}
