// SPDX-License-Identifier: MPL-2.0
// Deterministic full-surface output for comparison against a previous compositor.
#include "compositor.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
using namespace skyline::soc::host1x::video;
namespace {
void Field(Bytes &data, unsigned base, unsigned bit, unsigned count, unsigned value) {
    for (unsigned i=0;i<count;++i) {
        auto &byte=data.at(base+(bit+i)/8);
        const auto mask=1U<<((bit+i)%8);
        byte=static_cast<std::uint8_t>((byte&~mask)|(((value>>i)&1U)?mask:0U));
    }
}
}
int main(int argc,char **argv) {
    if (argc!=3) return 2;
    const bool benchmark=std::string(argv[2])=="bench";
    std::ofstream output(argv[1],std::ios::binary);
    if (!output) return 3;
    std::uint64_t elapsed{};
    unsigned cases{};
    const unsigned w=benchmark?640U:32U,h=benchmark?360U:18U;
    for(unsigned format:{31U,32U,35U,36U,67U,68U})
    for(bool linear:{false,true})
    for(unsigned scale:{1U,2U})
    for(unsigned flags=0;flags<(benchmark?1U:4U);++flags)
    for(unsigned alpha=0;alpha<(benchmark?1U:3U);++alpha) {
        if (benchmark && scale!=1) continue;
        const unsigned dw=w*scale,dh=h*scale;
        const bool rgb=format<67;
        Plane src(w*(rgb?4U:1U),h,linear,1),uv((w+1)/2*2,(h+1)/2,linear,1);
        Plane dst(dw*4,dh,!linear,1);
        Bytes ram(8*1024*1024);
        std::uint32_t random=12345;
        for(auto &value:ram) { random=random*1664525U+1013904223U; value=static_cast<std::uint8_t>(random>>24); }
        Memory memory{
            [&](auto address,auto bytes){std::copy_n(ram.begin()+static_cast<std::ptrdiff_t>(address),bytes.size(),bytes.begin());},
            [&](auto address,auto bytes){std::copy(bytes.begin(),bytes.end(),ram.begin()+static_cast<std::ptrdiff_t>(address));},
            [&](auto address,auto size){if(address>ram.size()||size>ram.size()-address)throw std::runtime_error("range");}};
        Bytes config(0x610);
        auto surface=[&](unsigned off,unsigned fmt,unsigned sw,unsigned sh,bool lin) {
            Field(config,off,0,7,fmt);Field(config,off,11,4,lin?0:1);Field(config,off,15,4,1);
            Field(config,off,32,14,sw-1);Field(config,off,46,14,sh-1);
            Field(config,off,64,14,sw-1);Field(config,off,78,14,sh-1);
            Field(config,off,96,14,(sw+1)/2-1);Field(config,off,110,14,(sh+1)/2-1);
        };
        surface(0x20,32,dw,dh,!linear);surface(0xd0,format,w,h,linear);
        Field(config,0x10,0,3,alpha);Field(config,0x10,6,10,500);
        Field(config,0x10,48,1,flags&1);Field(config,0x10,49,1,flags>>1);
        Field(config,0x10,80,14,dw-1);Field(config,0x10,112,14,dh-1);
        Field(config,0x90,0,1,1);Field(config,0x90,128,10,12);Field(config,0x90,138,10,990);
        Field(config,0x90,288,30,(w-1)<<16);Field(config,0x90,352,30,(h-1)<<16);
        Field(config,0x90,400,14,dw-1);Field(config,0x90,432,14,dh-1);
        // Enable a matrix and alpha override in half the flip cases.
        if (flags&1) {
            Field(config,0xf0,127,1,1);
            for(unsigned c=0;c<3;++c)Field(config,0xf0,c*84,20,256);
            Field(config,0xf0,192,20,4096);
            Field(config,0x90,170,1,1);Field(config,0x90,160,10,700);
        }
        std::copy(config.begin(),config.end(),ram.begin()+0x1000);
        Compositor vic;
        vic.Method(0x1c2,0x10,memory);vic.Method(0x100,0x100,memory);
        vic.Method(0x101,0x2000,memory);vic.Method(0x1c8,0x4000,memory);
        const auto start=std::chrono::steady_clock::now();
        for(unsigned i=0;i<(benchmark?8U:1U);++i)vic.Method(0xc0,1,memory);
        elapsed+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count());
        output.write(reinterpret_cast<const char*>(ram.data()+0x400000),static_cast<std::streamsize>(dst.Size()));
        ++cases;
    }
    if (!output) return 4;
    std::cout<<"cases="<<cases<<" compositor_us="<<elapsed<<"\n";
}
