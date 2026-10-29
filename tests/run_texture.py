from pathlib import Path
import subprocess,os,re
r=Path(__file__).resolve().parents[1];src=r/'app/src/main/cpp/skyline/gpu/texture';out=r/'results';out.mkdir(exist_ok=True)
s=(src/'texture.cpp').read_text();layout=(src/'layout.cpp').read_text()
def block(s,key):
 a=s.index(key);b=s.index('{',a);i=b+1;d=1
 while d:d+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[a:i]
copy=block(s,'void Texture::CopyToGuest(');helper=block(s,'static std::optional<bcn::readback::Format> BcReadbackFormat(')
a=s.index('        if (!deswizzleBuffer.empty())');b=s.index('        return stagingBuffer;',a)
upload='void Texture::DecodeUpload(std::vector<u8>&deswizzleBuffer,u8*bufferData){auto *deswizzleOutput=deswizzleBuffer.data();\n'+s[a:b]+'}\n'
enums=sorted(set(re.findall(r'vk::Format::(\w+)',helper+upload)))
pre=r'''
#include <algorithm>
#include <bit>
#include <cassert>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <vector>
#include "bc_encoder.h"
using u8=uint8_t;using u16=uint16_t;using u32=uint32_t;using u64=uint64_t;using u128=__uint128_t;
#define TRACE_EVENT(...) ((void)0)
struct exception:std::runtime_error{template<class...T>exception(const char*s,T...):std::runtime_error(s){}};
namespace vk{enum class Format {ENUMS};template<class T>int to_string(T){return 0;}}
namespace util{
 template<class T>constexpr T DivideCeil(T n,T d){return n/d+(n%d!=0);}
 template<class A,class B>constexpr auto AlignUp(A n,B d){using T=std::common_type_t<A,B>;return (T(n)+T(d)-1)/T(d)*T(d);}
 template<class A,class B>constexpr auto AlignDown(A n,B d){using T=std::common_type_t<A,B>;return T(n)/T(d)*T(d);}
 template<class A,class B>constexpr bool IsAligned(A n,B d){return n%d==0;}
}
namespace skyline::gpu {
namespace texture{
 struct Dimensions{u32 width,height,depth;};
 struct MipLevelLayout{Dimensions dimensions;size_t linearSize,targetLinearSize,blockLinearSize,blockHeight,blockDepth;MipLevelLayout(Dimensions d,size_t l,size_t t,size_t b,size_t h,size_t z):dimensions(d),linearSize(l),targetLinearSize(t),blockLinearSize(b),blockHeight(h),blockDepth(z){}};
 enum class TileMode{Linear,Pitch,Block};
 struct Fmt{vk::Format vkFormat;size_t blockWidth=4,blockHeight=4,bpb=8;size_t GetSize(size_t w,size_t h)const{return util::DivideCeil(w,blockWidth)*util::DivideCeil(h,blockHeight)*bpb;}size_t GetSize(Dimensions d)const{return GetSize(d.width,d.height)*d.depth;}};
}
struct GuestTexture{texture::Dimensions dimensions;texture::Fmt*format;struct{texture::TileMode mode=texture::TileMode::Block;size_t blockHeight=1,blockDepth=1;u32 pitch=64;}tileConfig;u32 layerStride{},mipLevelCount{},layerCount{};u32 GetLayerStride(){return layerStride;}u32 CalculateLayerSize()const;};
}
'''.replace('ENUMS',','.join(enums))
# Real layout implementation, except unrelated subrect code is retained too.
layout=layout.replace('#include "layout.h"','')
calc=block(s,'u32 GuestTexture::CalculateLayerSize() const')
model=r'''
namespace skyline::gpu {
struct Texture{texture::Fmt*format;std::optional<GuestTexture>guest;std::span<u8>mirror;std::vector<texture::MipLevelLayout>mipLayouts;size_t layerStride{},deswizzledLayerStride{},surfaceSize{},deswizzledSurfaceSize{},layerCount{},levelCount{};int tiling{};void CopyToGuest(u8*);void DecodeUpload(std::vector<u8>&,u8*);};
'''
main=r'''
}
int main(){using namespace skyline::gpu;using namespace skyline::gpu::texture;using B=bcn::readback::Format;size_t cases{};
 struct Pair {B bc;vk::Format guest,host;};
 for(auto pair:{
 Pair{B::Bc1,vk::Format::eBc1RgbaUnormBlock,vk::Format::eR8G8B8A8Unorm},
 Pair{B::Bc1,vk::Format::eBc1RgbaSrgbBlock,vk::Format::eR8G8B8A8Srgb},
 Pair{B::Bc2,vk::Format::eBc2UnormBlock,vk::Format::eR8G8B8A8Unorm},
 Pair{B::Bc2,vk::Format::eBc2SrgbBlock,vk::Format::eR8G8B8A8Srgb},
 Pair{B::Bc3,vk::Format::eBc3UnormBlock,vk::Format::eR8G8B8A8Unorm},
 Pair{B::Bc3,vk::Format::eBc3SrgbBlock,vk::Format::eR8G8B8A8Srgb},
 Pair{B::Bc4,vk::Format::eBc4UnormBlock,vk::Format::eR8Unorm},
 Pair{B::Bc4Signed,vk::Format::eBc4SnormBlock,vk::Format::eR8Snorm},
 Pair{B::Bc5,vk::Format::eBc5UnormBlock,vk::Format::eR8G8Unorm},
 Pair{B::Bc5Signed,vk::Format::eBc5SnormBlock,vk::Format::eR8G8Snorm},
 Pair{B::Bc6,vk::Format::eBc6HUfloatBlock,vk::Format::eR16G16B16A16Sfloat},
 Pair{B::Bc6Signed,vk::Format::eBc6HSfloatBlock,vk::Format::eR16G16B16A16Sfloat},
 Pair{B::Bc7,vk::Format::eBc7UnormBlock,vk::Format::eR8G8B8A8Unorm},
 Pair{B::Bc7,vk::Format::eBc7SrgbBlock,vk::Format::eR8G8B8A8Srgb}})
 for(auto mode:{TileMode::Linear,TileMode::Pitch,TileMode::Block})for(auto dims:{Dimensions{1,1,1},Dimensions{3,5,2},Dimensions{17,9,3},Dimensions{65,33,3}})for(size_t layers:{1U,3U})for(size_t levels:{1U,3U}){
  if(mode!=TileMode::Block&&levels>1)continue;
  const auto bpb=bcn::readback::BlockBytes(pair.bc),bpp=bcn::readback::PixelBytes(pair.bc);
  Fmt guestFmt{pair.guest,4,4,bpb},hostFmt{pair.host,1,1,bpp};Texture t;t.format=&hostFmt;t.layerCount=layers;t.levelCount=levels;
  t.mipLayouts=GetBlockLinearMipLayout(dims,4,4,bpb,1,1,bpp,1,1,levels);size_t guestSize{};for(auto &l:t.mipLayouts){t.deswizzledSurfaceSize+=l.linearSize*layers;t.surfaceSize+=l.targetLinearSize*layers;guestSize+=l.blockLinearSize;}
  GuestTexture guest{dims,&guestFmt};guest.tileConfig.mode=mode;guest.mipLevelCount=static_cast<u32>(levels);guest.layerCount=static_cast<u32>(layers);guest.tileConfig.pitch=static_cast<u32>(util::AlignUp(util::DivideCeil<size_t>(dims.width,4)*bpb,64UL));
  if(mode==TileMode::Linear)guestSize=t.mipLayouts[0].linearSize;else if(mode==TileMode::Pitch)guestSize=guest.CalculateLayerSize();
  guest.layerStride=static_cast<u32>(guestSize+32);t.guest=guest;t.deswizzledLayerStride=t.mipLayouts[0].linearSize;t.layerStride=t.mipLayouts[0].targetLinearSize;
  std::vector<u8>mirror(guest.layerStride*layers+32,0xcc);t.mirror={mirror.data()+16,guest.layerStride*layers};std::vector<u8>linear(t.deswizzledSurfaceSize),host(t.surfaceSize),expected(t.deswizzledSurfaceSize);
  size_t linearOff{},hostOff{},guestOff{};
  for(size_t level=0;level<levels;++level){auto &l=t.mipLayouts[level];for(size_t layer=0;layer<layers;++layer){
   std::vector<u8>pixels(l.targetLinearSize);for(size_t i=0;i<pixels.size();++i)pixels[i]=static_cast<u8>((i+layer*67+level*29)%127);
   auto target=std::span<u8>{linear.data()+linearOff+layer*l.linearSize,l.linearSize};bcn::readback::Recompress(pair.bc,pixels,target,l.dimensions.width,l.dimensions.height,l.dimensions.depth);
   auto*dest=t.mirror.data()+layer*guest.layerStride+guestOff;
   if(mode==TileMode::Block)CopyLinearToBlockLinear(l.dimensions,4,4,bpb,l.blockHeight,l.blockDepth,target.data(),dest);
   else if(mode==TileMode::Pitch)CopyLinearToPitchLinear(guest,target.data(),dest);
   else std::memcpy(dest,target.data(),target.size());
  }linearOff+=l.linearSize*layers;guestOff+=l.blockLinearSize;}
  t.DecodeUpload(linear,host.data());auto initialMirror=mirror;t.CopyToGuest(host.data());assert(mirror==initialMirror);
  // Modifying one texel in the final level must not disturb any other layer/mip.
  host[host.size()-bpp]^=0x40;auto hostBefore=host;expected=linear;linearOff=hostOff=0;
  for(auto &l:t.mipLayouts){for(size_t layer=0;layer<layers;++layer)bcn::readback::Recompress(pair.bc,{host.data()+hostOff+layer*l.targetLinearSize,l.targetLinearSize},{expected.data()+linearOff+layer*l.linearSize,l.linearSize},l.dimensions.width,l.dimensions.height,l.dimensions.depth);linearOff+=l.linearSize*layers;hostOff+=l.targetLinearSize*layers;}
  t.CopyToGuest(host.data());assert(host==hostBefore);linearOff=guestOff=0;
  for(auto &l:t.mipLayouts){for(size_t layer=0;layer<layers;++layer){std::vector<u8>read(l.linearSize);auto*source=t.mirror.data()+layer*guest.layerStride+guestOff;if(mode==TileMode::Block)CopyBlockLinearToLinear(l.dimensions,4,4,bpb,l.blockHeight,l.blockDepth,source,read.data());else if(mode==TileMode::Pitch)CopyPitchLinearToLinear(guest,source,read.data());else std::memcpy(read.data(),source,read.size());assert(std::equal(read.begin(),read.end(),expected.begin()+static_cast<ptrdiff_t>(linearOff+layer*l.linearSize)));}linearOff+=l.linearSize*layers;guestOff+=l.blockLinearSize;}
  assert(std::all_of(mirror.begin(),mirror.begin()+16,[](u8 v){return v==0xcc;}));assert(std::all_of(mirror.end()-16,mirror.end(),[](u8 v){return v==0xcc;}));
  // Simulate reclaimed guest backing: reconstruct from the host, without relying
  // on valid original blocks. No untouched-block identity is promised in this case.
  std::fill(t.mirror.begin(),t.mirror.end(),0);t.CopyToGuest(host.data());
  assert(host==hostBefore);
  // A short guest mapping must fail before the final writeback starts.
  auto full=t.mirror;t.mirror=full.first(1);auto before=mirror;bool failed=false;try{t.CopyToGuest(host.data());}catch(const std::exception&){failed=true;}assert(failed&&mirror==before);
  ++cases;
 }
 std::cout<<cases<<" integration cases: actual BC decoder + layout/swizzle + texture copy; linear/pitch/block, mips, arrays and volumes, unchanged identity, mutation and short mapping OK\n";
}
'''
(out/'texture_integration.cpp').write_text(pre+layout+model+calc+helper+copy+upload+main)
p=subprocess.run(['g++','-std=c++20','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie','-Wno-attributes','-I'+str(src),str(out/'texture_integration.cpp'),str(out/'decoder.o'),'-o',str(out/'texture_integration')],capture_output=True,text=True);(out/'texture_compile.log').write_text(p.stdout+p.stderr);assert p.returncode==0,p.stderr
p=subprocess.run([str(out/'texture_integration')],capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0',UBSAN_OPTIONS='halt_on_error=1'));(out/'texture.log').write_text(p.stdout+p.stderr);print(p.stdout+p.stderr);assert p.returncode==0
