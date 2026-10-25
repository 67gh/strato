#include <cassert>
#include <iostream>
#include <random>
#include <vector>
#include "bc_encoder.h"
using namespace bcn::readback;

static std::vector<uint8_t> Decode(Format f, const std::vector<uint8_t>&blocks, size_t w, size_t h, size_t d) {
    std::vector<uint8_t> result(DecodedSize(f,w,h,d));size_t offset{};
    for(size_t z=0;z<d;++z)for(size_t y=0;y<h;y+=4)for(size_t x=0;x<w;x+=4){
        alignas(8) uint8_t block[16]{},pixels[128]{};std::memcpy(block,blocks.data()+offset,BlockBytes(f));DecodeBlock(f,block,pixels);
        for(size_t dy=0;dy<4&&dy+y<h;++dy)for(size_t dx=0;dx<4&&dx+x<w;++dx)
            std::memcpy(result.data()+((z*h+y+dy)*w+x+dx)*PixelBytes(f),pixels+(dy*4+dx)*PixelBytes(f),PixelBytes(f));
        offset+=BlockBytes(f);
    }return result;
}
int main(){std::mt19937 rng(431);size_t cases{};
 for(Format f:{Format::Bc1,Format::Bc2,Format::Bc3,Format::Bc4,Format::Bc4Signed,Format::Bc5,Format::Bc5Signed,Format::Bc6,Format::Bc6Signed,Format::Bc7})
 for(auto extent: {std::array<size_t,3>{1,1,1},{3,5,2},{4,4,1},{7,9,3},{16,8,1}}){
    auto[w,h,d]=extent;auto bpp=PixelBytes(f);std::vector<uint8_t> host(DecodedSize(f,w,h,d));
    for(size_t i=0;i<host.size();i++)host[i]=static_cast<uint8_t>(rng());
    if(f==Format::Bc6||f==Format::Bc6Signed)for(size_t i=0;i<host.size();i+=8){for(size_t c=0;c<3;c++){unsigned half=0x2000+((rng()%0x3000));if(f==Format::Bc6Signed&&rng()%2)half|=0x8000;host[i+2*c]=static_cast<uint8_t>(half);host[i+2*c+1]=static_cast<uint8_t>(half>>8);}host[i+6]=0;host[i+7]=0x3c;}
    std::vector<uint8_t> blocks(EncodedSize(f,w,h,d),0),originalHost=host;
    assert(Recompress(f,host,blocks,w,h,d)>0);assert(host==originalHost);
    auto decoded=Decode(f,blocks,w,h,d);auto original=blocks;
    assert(Recompress(f,decoded,blocks,w,h,d)==0);assert(original==blocks);
    // Alter only the final texel, and verify all other blocks remain intact.
    auto changed=decoded;size_t last=changed.size()-bpp;
    for(size_t c=0;c<bpp;c++)changed[last+c]^=0x40;
    assert(Recompress(f,changed,blocks,w,h,d)==1);
    assert(std::equal(blocks.begin(),blocks.end()-BlockBytes(f),original.begin()));
    // Every produced BC1 transparent texel must still decode with alpha zero.
    if(f==Format::Bc1)for(size_t i=0;i<w*h*d;i++)assert(decoded[i*4+3]==(host[i*4+3]<128?0:255));
    ++cases;
 }
 // Known solid inputs exercise endpoint quantization, signed extrema, BC7 P-bits and BC6 mode layout.
 for(Format f:{Format::Bc1,Format::Bc2,Format::Bc3,Format::Bc4,Format::Bc4Signed,Format::Bc5,Format::Bc5Signed,Format::Bc6,Format::Bc6Signed,Format::Bc7})
 for(int value:{0,1,37,127,128,254,255}){
    std::vector<uint8_t> host(DecodedSize(f,4,4,1),static_cast<uint8_t>(value));
    bool hdr=f==Format::Bc6||f==Format::Bc6Signed;
    if(PixelBytes(f)==4)for(size_t i=3;i<host.size();i+=4)host[i]=255;
    if(hdr)for(size_t i=0;i<host.size();i+=2){host[i]=0;host[i+1]=static_cast<uint8_t>(f==Format::Bc6Signed?0xbc:0x3c);} // +/-1
    std::vector<uint8_t> blocks(EncodedSize(f,4,4,1));Recompress(f,host,blocks,4,4);auto decoded=Decode(f,blocks,4,4,1);
    if(hdr){for(size_t i=0;i<decoded.size();i+=8)for(size_t c=0;c<3;c++){int v=decoded[i+c*2]|(int(decoded[i+c*2+1])<<8);assert((v&0x8000)==(f==Format::Bc6Signed?0x8000:0));assert(std::abs((v&0x7fff)-0x3c00)<40);}}
    else for(size_t i=0;i<host.size();i++){
      int a=host[i],b=decoded[i];if(f==Format::Bc4Signed||f==Format::Bc5Signed){a=std::max(a<128?a:a-256,-127);b=std::max(b<128?b:b-256,-127);}
      assert(std::abs(a-b)<=8);
    }
    ++cases;
 }
 std::vector<uint8_t> tiny(1);bool threw=false;try{Recompress(Format::Bc1,tiny,tiny,4,4);}catch(const std::invalid_argument&){threw=true;}assert(threw);
 threw=false;try{DecodedSize(Format::Bc1,SIZE_MAX,SIZE_MAX,2);}catch(const std::overflow_error&){threw=true;}assert(threw);
 threw=false;try{Recompress(Format::Bc1,{}, {},0,4);}catch(const std::invalid_argument&){threw=true;}assert(threw);
 std::cout<<cases<<" BC format/extent/solid cases passed; unchanged block identity, single-block change, transparency, signed/HDR, invalid sizes and overflow OK\n";
}
