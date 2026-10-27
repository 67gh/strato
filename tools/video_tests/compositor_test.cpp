// SPDX-License-Identifier: MPL-2.0
#include "compositor.h"
#include <cassert>
#include <iostream>
using namespace skyline::soc::host1x::video;
void Field(Bytes &b,unsigned off,unsigned bit,unsigned count,unsigned v){for(unsigned i=0;i<count;++i){auto &x=b.at(off+(bit+i)/8);auto m=1U<<((bit+i)%8);x=static_cast<std::uint8_t>((x&~m)|((v>>i&1)?m:0));}}
int main(){
 Bytes ram(1<<20);unsigned writes{};Memory m{
 [&](auto a,auto b){std::copy_n(ram.begin()+static_cast<std::ptrdiff_t>(a),b.size(),b.begin());},
 [&](auto a,auto b){++writes;std::copy(b.begin(),b.end(),ram.begin()+static_cast<std::ptrdiff_t>(a));},
 [&](auto a,auto n){if(a>ram.size()||n>ram.size()-a)throw std::runtime_error("range");}};
 unsigned count{};
 for(unsigned inf:{31U,32U,35U,36U})for(unsigned outf:{31U,32U,35U,36U})for(unsigned flags=0;flags<4;++flags)for(unsigned bh:{0U,1U,3U}){
  const bool linear=bh==0;Plane in(8,2,linear,bh),out(12,3,!linear,bh);
  std::fill(ram.begin(),ram.end(),0xe7);
  const unsigned iswap=(inf==32||inf==36)?2:0,oswap=(outf==32||outf==36)?2:0;
  for(unsigned y=0;y<2;++y)for(unsigned x=0;x<2;++x){ram[0x10000+in.Offset(x*4+iswap,y)]=static_cast<std::uint8_t>(x*100+y*50);ram[0x10000+in.Offset(x*4+1,y)]=40;ram[0x10000+in.Offset(x*4+2-iswap,y)]=200;ram[0x10000+in.Offset(x*4+3,y)]=120;}
  Bytes c(0x610);
  auto surface=[&](unsigned off,unsigned fmt,unsigned w,unsigned h,bool lin){Field(c,off,0,7,fmt);Field(c,off,11,4,lin?0:1);Field(c,off,15,4,bh);Field(c,off,32,14,w-1);Field(c,off,46,14,h-1);Field(c,off,64,14,w-1);Field(c,off,78,14,h-1);};
  surface(0x20,outf,3,3,!linear);surface(0xd0,inf,2,2,linear);
  Field(c,0x10,48,1,flags&1);Field(c,0x10,49,1,flags>>1);Field(c,0x10,80,14,2);Field(c,0x10,112,14,2);
  Field(c,0x90,0,1,1);Field(c,0x90,138,10,1023);Field(c,0x90,288,30,1<<16);Field(c,0x90,352,30,1<<16);Field(c,0x90,400,14,2);Field(c,0x90,432,14,2);
  std::copy(c.begin(),c.end(),ram.begin()+0x1000);
  Compositor vic;vic.Method(0x1c2,0x10,m);vic.Method(0x100,0x100,m);vic.Method(0x1c8,0x200,m);vic.Method(0xc0,1,m);
  for(unsigned y=0;y<3;++y)for(unsigned x=0;x<3;++x){unsigned sx=flags&1?2-x:x,sy=flags&2?2-y:y;
   assert(ram[0x20000+out.Offset(x*4+oswap,y)]==sx*50+sy*25);assert(ram[0x20000+out.Offset(x*4+1,y)]==40);assert(ram[0x20000+out.Offset(x*4+2-oswap,y)]==200);assert(ram[0x20000+out.Offset(x*4+3,y)]==((inf>=35||outf>=35)?255:120));}
  // Padding survives writes. No transpose fallback or second-slot silent success.
  assert(ram[0x20000+out.Offset(15,0)]==0xe7);
  auto before=writes;ram[0x1000+0x140]=1;bool fail=false;try{vic.Method(0xc0,1,m);}catch(const std::exception&){fail=true;}assert(fail&&writes==before);
  ++count;
 }
 std::cout<<"PASS VIC "<<count<<" format/linear/block-linear/flip/bilinear scaling cases; padding preserved; unsupported multi-slot rejected\n";
}
