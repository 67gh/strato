// SPDX-License-Identifier: MIT
// VIC ABI and matrix semantics derived from Ryujinx (MIT), see THIRD_PARTY.md.
#include "compositor.h"
#include <memory>

namespace skyline::soc::host1x::video {
namespace {
using Pixel=std::array<int,4>;
struct Surface {
    unsigned format,w,h,cw,ch;
    Plane y,uv;
    Surface(std::span<const std::uint8_t> s)
        : format(Bits(s,0,7)), w(Bits(s,32,14)+1), h(Bits(s,46,14)+1),
          cw(Bits(s,96,14)+1),ch(Bits(s,110,14)+1),
          y((Bits(s,64,14)+1)*(IsRgb()?4:1),Bits(s,78,14)+1,Bits(s,11,4)==0,Bits(s,15,4)),
          uv(cw*2,ch,Bits(s,11,4)==0,Bits(s,15,4)) {
        if ((format!=31 && format!=32 && format!=35 && format!=36 && format!=67 && format!=68) ||
            w>4096 || h>4096 || (Bits(s,11,4)!=0 && Bits(s,11,4)!=1) ||
            y.width < w*(IsRgb()?4U:1U) || y.height<h ||
            (!IsRgb() && (cw<(w+1)/2 || ch<(h+1)/2)))
            throw std::runtime_error("VIC: unsupported format/layout or invalid surface dimensions");
    }
    bool IsRgb() const { return format==31 || format==32 || format==35 || format==36; }
};
Pixel Matrix(std::span<const std::uint8_t> m, Pixel p) {
    if (!Bits(m,127,1)) return p;
    Pixel result=p;
    const auto shift=Bits(m,60,4);
    for (unsigned row=0;row<3;++row) {
        std::int64_t sum{};
        for (unsigned col=0;col<3;++col)
            sum += std::int64_t(p[col])*Signed(Bits(m,col*64+row*20,20),20);
        sum=((sum>>shift)+Signed(Bits(m,192+row*20,20),20))>>8;
        result[row]=static_cast<int>(std::clamp<std::int64_t>(sum,0,1023));
    }
    return result;
}
}
void Compositor::Method(std::uint32_t method, std::uint32_t value, const Memory &memory) {
    if (method>=regs.size()) throw std::runtime_error("VIC: method outside register bank");
    regs[method]=value;
    if (method==0xc0) Execute(memory);
}
void Compositor::Execute(const Memory &memory) {
    const auto config=memory.Read(std::uint64_t(regs[0x1c2])<<8,0x610);
    const auto bytes=std::span<const std::uint8_t>(config);
    const auto output=bytes.subspan(0x10,16);
    const Surface dst(bytes.subspan(0x20,16));
    if (Bits(output,50,1) || Bits(output,46,2) || Bits(output,0,3))
        throw std::runtime_error("VIC: transpose/regamma/alpha fill mode unsupported");
    if (Bits(output,64,14) || Bits(output,96,14) || Bits(output,80,14)!=dst.w-1 || Bits(output,112,14)!=dst.h-1)
        throw std::runtime_error("VIC: partial output target rectangle unsupported");
    const auto outY=std::uint64_t(regs[0x1c8])<<8, outUV=std::uint64_t(regs[0x1c9])<<8;
    memory.Check(outY,dst.y.Size());
    if (!dst.IsRgb()) memory.Check(outUV,dst.uv.Size());
    int enabled=-1;
    for (int slot=0;slot<8;++slot) if (Bits(bytes.subspan(0x90+slot*0xb0,64),0,1)) {
        if (enabled>=0) throw std::runtime_error("VIC: multi-slot blending unsupported");
        enabled=slot;
    }
    // Keep padding/unused rows intact. Commit only after all reads and validation.
    Bytes out=memory.Read(outY,dst.y.Size()), outChroma;
    if (!dst.IsRgb()) outChroma=memory.Read(outUV,dst.uv.Size());
    std::span<const std::uint8_t> slot;
    std::unique_ptr<Surface> src;
    Bytes in,inChroma;
    unsigned left{},right{},top{},bottom{},sx0{},sx1{},sy0{},sy1{},clampLow{},clampHigh{1023};
    if (enabled>=0) {
        slot=bytes.subspan(0x90+static_cast<unsigned>(enabled)*0xb0,0xb0);
        if (Bits(slot,1,5) || Bits(slot,9,7) || Bits(slot,16,4) || Bits(slot,24,12) ||
            Bits(slot,171,3) || Bits(slot,175,8) || Bits(slot,183,2) || Bits(slot,186,1) ||
            Bits(slot.subspan(0x50,16),104,1))
            throw std::runtime_error("VIC: interlacing, enhancement, compression or luma-key unsupported");
        // One opaque slot. Reject blending/overrides rather than silently drawing a wrong image.
        if (U64(slot,0xa0)>>32 || U64(slot,0xa8))
            throw std::runtime_error("VIC: blend factors/overrides unsupported");
        src=std::make_unique<Surface>(slot.subspan(0x40,16));
        const auto reg=0x100+static_cast<unsigned>(enabled)*24;
        in=memory.Read(std::uint64_t(regs[reg])<<8,src->y.Size());
        if (!src->IsRgb()) inChroma=memory.Read(std::uint64_t(regs[reg+1])<<8,src->uv.Size());
        left=Bits(slot,384,14); right=Bits(slot,400,14);
        top=Bits(slot,416,14); bottom=Bits(slot,432,14);
        sx0=Bits(slot,256,30); sx1=Bits(slot,288,30);
        sy0=Bits(slot,320,30); sy1=Bits(slot,352,30);
        clampLow=Bits(slot,128,10); clampHigh=Bits(slot,138,10);
        if (left>right || top>bottom || right>=dst.w || bottom>=dst.h || sx0>sx1 || sy0>sy1 ||
            (sx1>>16)>=src->w || (sy1>>16)>=src->h || clampLow>clampHigh)
            throw std::runtime_error("VIC: invalid source/destination rectangle or clamp");
    }
    auto readPixel=[&](unsigned x,unsigned y) -> Pixel {
        Pixel p{0,0,0,1023};
        if (src->IsRgb()) {
            const unsigned swap=(src->format==32 || src->format==36)?2:0;
            p={in[src->y.Offset(x*4+swap,y)]<<2,in[src->y.Offset(x*4+1,y)]<<2,
               in[src->y.Offset(x*4+(2-swap),y)]<<2,
               src->format>=35?1023:in[src->y.Offset(x*4+3,y)]<<2};
        } else {
            // VIC's 68 (Y8___V8U8_N420) stores U then V in little-endian memory.
            const unsigned swap=src->format==67?1:0;
            p={in[src->y.Offset(x,y)]<<2,inChroma[src->uv.Offset((x/2)*2+swap,y/2)]<<2,
               inChroma[src->uv.Offset((x/2)*2+(1-swap),y/2)]<<2,1023};
        }
        return p;
    };
    auto sample=[&](unsigned x,unsigned y) -> Pixel {
        Pixel p{static_cast<int>(Bits(output,16,10)),static_cast<int>(Bits(output,26,10)),
                static_cast<int>(Bits(output,36,10)),static_cast<int>(Bits(output,6,10))};
        if (src && x>=left && x<=right && y>=top && y<=bottom) {
            const auto fx=sx0+(right==left?0:(std::uint64_t(x-left)*(sx1-sx0)/(right-left)));
            const auto fy=sy0+(bottom==top?0:(std::uint64_t(y-top)*(sy1-sy0)/(bottom-top)));
            const auto ix=static_cast<unsigned>(fx>>16), iy=static_cast<unsigned>(fy>>16);
            const auto a=readPixel(ix,iy), b=readPixel(std::min(ix+1,src->w-1),iy);
            const auto c=readPixel(ix,std::min(iy+1,src->h-1)), d=readPixel(std::min(ix+1,src->w-1),std::min(iy+1,src->h-1));
            for (unsigned channel=0;channel<4;++channel) {
                const auto upper=std::uint64_t(a[channel])*(65536-(fx&65535))+std::uint64_t(b[channel])*(fx&65535);
                const auto lower=std::uint64_t(c[channel])*(65536-(fx&65535))+std::uint64_t(d[channel])*(fx&65535);
                p[channel]=static_cast<int>((upper*(65536-(fy&65535))+lower*(fy&65535)+(1ULL<<31))>>32);
            }
            p=Matrix(slot.subspan(0x60,32),p);
            p=Matrix(slot.subspan(0x80,32),p);
            for (unsigned i=0;i<3;++i) p[i]=std::clamp(p[i],static_cast<int>(clampLow),static_cast<int>(clampHigh));
            if (Bits(slot,170,1)) p[3]=static_cast<int>(Bits(slot,160,10));
        }
        return Matrix(bytes.subspan(0x30,32),p);
    };
    auto pixel=[&](unsigned x,unsigned y) {
        return sample(Bits(output,48,1)?dst.w-1-x:x,Bits(output,49,1)?dst.h-1-y:y);
    };
    for (unsigned y=0;y<dst.h;++y) for (unsigned x=0;x<dst.w;++x) {
        const auto p=pixel(x,y);
        if (dst.IsRgb()) {
            const unsigned swap=(dst.format==32 || dst.format==36)?2:0;
            out[dst.y.Offset(x*4+swap,y)]=static_cast<std::uint8_t>(p[0]>>2);
            out[dst.y.Offset(x*4+1,y)]=static_cast<std::uint8_t>(p[1]>>2);
            out[dst.y.Offset(x*4+(2-swap),y)]=static_cast<std::uint8_t>(p[2]>>2);
            out[dst.y.Offset(x*4+3,y)]=dst.format>=35?255:static_cast<std::uint8_t>(p[3]>>2);
        } else out[dst.y.Offset(x,y)]=static_cast<std::uint8_t>(p[0]>>2);
    }
    if (!dst.IsRgb()) for (unsigned y=0;y<(dst.h+1)/2;++y) for (unsigned x=0;x<(dst.w+1)/2;++x) {
        unsigned u{},v{},count{};
        for (unsigned dy=0;dy<2 && y*2+dy<dst.h;++dy) for (unsigned dx=0;dx<2 && x*2+dx<dst.w;++dx) {
            auto p=pixel(x*2+dx,y*2+dy); u+=static_cast<unsigned>(p[1]); v+=static_cast<unsigned>(p[2]); ++count;
        }
        const unsigned swap=dst.format==67?1:0;
        outChroma[dst.uv.Offset(x*2+swap,y)]=static_cast<std::uint8_t>((u/count)>>2);
        outChroma[dst.uv.Offset(x*2+(1-swap),y)]=static_cast<std::uint8_t>((v/count)>>2);
    }
    memory.Write(outY,out);
    if (!dst.IsRgb()) memory.Write(outUV,outChroma);
}
}
