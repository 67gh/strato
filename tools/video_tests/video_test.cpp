// SPDX-License-Identifier: MPL-2.0
#include "decoder.h"
#include "compositor.h"
#include "h264.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <set>
using namespace skyline::soc::host1x::video;
static void Field(Bytes &b,unsigned offset,unsigned bit,unsigned count,std::uint32_t value) {
 for(unsigned i=0;i<count;++i) {
  auto &byte=b.at(offset+(bit+i)/8);const auto mask=static_cast<std::uint8_t>(1U<<((bit+i)%8));
  byte=static_cast<std::uint8_t>((byte&~mask)|(((value>>i)&1)?mask:0));
 }
}
template<class F> void Reject(F f) { bool threw=false;try{f();}catch(const std::exception&){threw=true;}assert(threw); }
int main(int argc,char **argv) {
 assert(argc==3);
 Bytes ram(4*1024*1024,0x7e);unsigned writes=0;
 Memory mem{[&](auto a,auto b){std::copy_n(ram.begin()+static_cast<std::ptrdiff_t>(a),b.size(),b.begin());},
            [&](auto a,auto b){++writes;std::copy(b.begin(),b.end(),ram.begin()+static_cast<std::ptrdiff_t>(a));},
            [&](auto a,auto s){if(a>ram.size() || s>ram.size()-a)throw std::runtime_error("unmapped");}};
 for (unsigned width : {1U,15U,63U,64U,65U,127U,191U,257U}) for (unsigned height : {1U,7U,8U,15U,16U,17U,65U}) for(unsigned bh=0;bh<=5;++bh) {
  Plane p(width,height,false,bh);std::set<std::size_t> offsets;
  for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){auto o=p.Offset(x,y);assert(o<p.Size());assert(offsets.insert(o).second);}
 }
 Reject([&]{mem.Check(0xfffffff0ULL,32);});Reject([&]{Plane p(0,1,false,0);});
 unsigned total{};
 for(int arg=1;arg<argc;++arg) {
  std::ifstream f(argv[arg],std::ios::binary);assert(f);
  auto read32=[&](){std::uint32_t v{};f.read(reinterpret_cast<char *>(&v),4);assert(f);return v;};
  auto bytes=[&](unsigned n){Bytes b(n);f.read(reinterpret_cast<char *>(b.data()),n);assert(f);return b;};
  auto codec=read32(),w=read32(),h=read32(),jobs=read32();Decoder dec;
  dec.Method(0x80,codec,mem);dec.Method(0x101,0x1000>>8,mem);dec.Method(0x102,0x4000>>8,mem);
  for(unsigned j=0;j<jobs;++j) {
   auto ps=read32(),bs=read32(),ys=read32();auto pic=bytes(ps),stream=bytes(bs),oracle=bytes(ys);
   std::copy(pic.begin(),pic.end(),ram.begin()+0x1000);std::copy(stream.begin(),stream.end(),ram.begin()+0x4000);
   auto index=codec==3?unsigned((U64(pic,0xb0)>>34)&127):3;
   dec.Method(0x10c+index,0x20000>>8,mem);dec.Method(0x11d+index,0x40000>>8,mem);
   dec.Method(0x109,0x60000>>8,mem);
   dec.Method(0xc0,1,mem);
   assert(U32(std::span<const std::uint8_t>(ram).subspan(0x60000,56),0)==((w+15)/16)*((h+15)/16));
   for(unsigned k=4;k<56;++k) assert(ram[0x60000+k]==0);
   Plane yp(w,h,false,1),cp(w,h/2,false,1);
   for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)assert(ram[0x20000+yp.Offset(x,y)]==oracle[y*w+x]);
   for(unsigned y=0;y<h/2;++y)for(unsigned x=0;x<w/2;++x) {
    assert(ram[0x40000+cp.Offset(x*2,y)]==oracle[w*h+y*w/2+x]);
    assert(ram[0x40000+cp.Offset(x*2+1,y)]==oracle[w*h+w*h/4+y*w/2+x]);
   }
   // Feed the actual decoded Tegra planes into VIC, compare RGB with integer BT.601.
   Bytes config(0x610);
   auto surface=[&](unsigned off,unsigned fmt,unsigned width,unsigned height,bool linear,unsigned bh){
    Field(config,off,0,7,fmt);Field(config,off,11,4,linear?0:1);Field(config,off,15,4,bh);
    Field(config,off,32,14,width-1);Field(config,off,46,14,height-1);
    Field(config,off,64,14,width-1);Field(config,off,78,14,height-1);
    Field(config,off,96,14,width/2-1);Field(config,off,110,14,height/2-1);
   };
   surface(0x20,31,w,h,true,0);surface(0xd0,68,w,h,false,1);
   Field(config,0x10,80,14,w-1);Field(config,0x10,112,14,h-1);
   Field(config,0x90,0,1,1);Field(config,0x90,138,10,1023);Field(config,0x90,160,10,1023);
   Field(config,0x90,288,30,(w-1)<<16);Field(config,0x90,352,30,(h-1)<<16);
   Field(config,0x90,400,14,w-1);Field(config,0x90,432,14,h-1);
   // Matrix inputs and outputs are 10-bit values. Column-major 20-bit coefficients.
   const int matrix[3][4]={{298,0,409,-(298*16+409*128)*4},{298,-100,-208,(-298*16+100*128+208*128)*4},{298,516,0,-(298*16+516*128)*4}};
   for(unsigned row=0;row<3;++row)for(unsigned col=0;col<4;++col)Field(config,0xf0,col*64+row*20,20,static_cast<std::uint32_t>(matrix[row][col])&0xfffff);
   Field(config,0xf0,127,1,1);
   std::copy(config.begin(),config.end(),ram.begin()+0x100000);
   Compositor vic;vic.Method(0x1c2,0x100000>>8,mem);vic.Method(0x100,0x20000>>8,mem);vic.Method(0x101,0x40000>>8,mem);vic.Method(0x1c8,0x200000>>8,mem);
   vic.Method(0xc0,1,mem);
   Plane rgb(w*4,h,true,0);
   for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
    const int Y=oracle[y*w+x],U=oracle[w*h+(y/2)*(w/2)+x/2],V=oracle[w*h+w*h/4+(y/2)*(w/2)+x/2];
    for(unsigned c=0;c<3;++c){int val=(matrix[c][0]*Y+matrix[c][1]*U+matrix[c][2]*V+matrix[c][3]/4)>>8;
     assert(ram[0x200000+rgb.Offset(x*4+c,y)]==std::clamp(val,0,255));}
   }
   // Faults must not write output; missing input mapping and unsupported slot modes.
   auto before=writes;vic.Method(0x100,0xffff00,mem);Reject([&]{vic.Method(0xc0,1,mem);});assert(writes==before);
   vic.Method(0x100,0x20000>>8,mem);ram[0x100000+0x90]|=2;Reject([&]{vic.Method(0xc0,1,mem);});assert(writes==before);
   ++total;
  }
  unsigned before=writes;dec.Method(0x80,9,mem);Reject([&]{dec.Method(0xc0,1,mem);});assert(before==writes);
 }
 std::cout<<"PASS: "<<total<<" real decoded pictures equal FFmpeg oracle; NVDEC -> Tegra -> VIC BT.601 pixels exact; 336 GOB geometries; failure paths do not write\n";
}
