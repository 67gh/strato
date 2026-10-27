// SPDX-License-Identifier: MIT
// Parameter-set reconstruction adapted from Ryujinx (MIT), see THIRD_PARTY.md.
#pragma once
#include "memory.h"

namespace skyline::soc::host1x::video {
class BitWriter {
    Bytes data;
    unsigned bit{};
public:
    void Put(std::uint32_t value, unsigned count) {
        for (unsigned i = count; i; --i) {
            if (!(bit % 8)) data.push_back(0);
            data.back() |= static_cast<std::uint8_t>(((value >> (i-1)) & 1) << (7-bit%8));
            ++bit;
        }
    }
    void Ue(std::uint32_t value) {
        if (value > 65535) throw std::runtime_error("H264: invalid Exp-Golomb value");
        unsigned n{};
        for (auto x=value+1; x >>= 1;) ++n;
        Put(0,n); Put(value+1,n+1);
    }
    void Se(int value) { Ue(static_cast<std::uint32_t>(value <= 0 ? -2*value : 2*value-1)); }
    Bytes Nal(std::uint8_t header) {
        Put(1,1);
        while (bit%8) Put(0,1);
        Bytes result{0,0,0,1,header};
        unsigned zeros{};
        for (auto b : data) {
            if (zeros >= 2 && b <= 3) { result.push_back(3); zeros=0; }
            result.push_back(b);
            zeros = b == 0 ? zeros+1 : 0;
        }
        return result;
    }
};
inline Bytes H264Parameters(std::span<const std::uint8_t> p) {
    const auto flags=U64(p,0xb0);
    const auto get=[&](unsigned b, unsigned n) { return static_cast<std::uint32_t>((flags>>b)&((1ULL<<n)-1)); };
    const auto width=U32(p,0x64), height=U32(p,0x68), poc=get(14,2);
    if (!width || !height || width>256 || height>256 || U32(p,0x60)!=1 || get(5,1) || get(0,1) ||
        get(12,2)!=1 || get(8,4)>12 || U32(p,0x58)>12 || poc==1 || poc>2 ||
        U32(p,0x78)>31 || U32(p,0x7c)>31 || get(32,2)>2)
        throw std::runtime_error("H264: unsupported geometry, field, chroma or POC configuration");
    BitWriter s;
    s.Put(100,8); s.Put(0,8); s.Put(51,8); s.Ue(0); s.Ue(1);
    s.Ue(0); s.Ue(0); s.Put((U32(p,0x2d0)>>1)&1,1); s.Put(0,1);
    s.Ue(get(8,4)); s.Ue(poc);
    if (!poc) s.Ue(U32(p,0x58));
    s.Ue(16); s.Put(0,1); s.Ue(width-1); s.Ue(height-1);
    s.Put(1,1); s.Put(get(1,1),1); s.Put(0,1); s.Put(0,1);
    Bytes result=s.Nal(0x67);
    BitWriter b;
    b.Ue(0); b.Ue(0); b.Put(U32(p,0x70)!=0,1); b.Put(U32(p,0x74)!=0,1);
    b.Ue(0); b.Ue(U32(p,0x78)); b.Ue(U32(p,0x7c)); b.Put(get(2,1),1); b.Put(get(32,2),2);
    b.Se(Signed(get(16,6),6)); b.Se(0); b.Se(Signed(get(22,5),5));
    b.Put(U32(p,0x80)!=0,1); b.Put(get(3,1),1); b.Put(U32(p,0x84)!=0,1);
    const bool transform=U32(p,0x88)!=0;
    b.Put(transform,1); b.Put(1,1);
    constexpr std::array<unsigned,16> scan4{0,1,4,8,5,2,3,6,9,12,13,10,7,11,14,15};
    constexpr std::array<unsigned,64> scan8{0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};
    auto scaling=[&](std::size_t offset, auto scan) {
        b.Put(1,1); int last=8;
        for (auto index : scan) {
            if (offset+index>=p.size() || !p[offset+index]) throw std::runtime_error("H264: invalid scaling list");
            const int value=p[offset+index];
            int delta=value-last;
            if (delta>127) delta-=256;
            if (delta< -128) delta+=256;
            b.Se(delta); last=value;
        }
    };
    for (unsigned i=0;i<6;++i) scaling(0x1c0+i*16,scan4);
    if (transform) for (unsigned i=0;i<2;++i) scaling(0x220+i*64,scan8);
    b.Se(Signed(get(27,5),5));
    auto pps=b.Nal(0x68); result.insert(result.end(),pps.begin(),pps.end());
    return result;
}
}
