#!/usr/bin/env python3
"""Compile production texture bookkeeping/layout code with ASan/UBSan.

Uses the bundled Vulkan types, but no Vulkan device. These are CPU regression
tests, not proof of correct rendering in a game. --baseline layout/mip uses
HEAD's old implementation of that part and must fail its regression test.
"""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'app/src/main/cpp/skyline'


def block(text, marker):
    start = text.index(marker)
    pos = text.index('{', start + len(marker)) + 1
    depth = 1
    while depth:
        depth += (text[pos] == '{') - (text[pos] == '}')
        pos += 1
    return text[start:pos]


PREFIX = r'''
#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>
using u32=uint32_t;
namespace util {
template<class T> T DivideCeil(T a,T b){return a/b+(a%b!=0);}
template<class A,class B> auto AlignUp(A a,B b){using T=std::common_type_t<A,B>;return (T(a)+T(b)-1)/T(b)*T(b);}
}
struct Dimensions {u32 width,height,depth;};
struct MipLevelLayout {
 Dimensions dimensions;size_t linearSize,targetLinearSize,blockLinearSize,blockHeight,blockDepth;
};
constexpr size_t GobWidth=64,GobHeight=8;
namespace texture {enum class RenderPassUsage {None,Sampled,RenderTarget};}
struct Format {vk::ImageAspectFlags vkAspect=vk::ImageAspectFlagBits::eColor;};
struct Texture {
 bool guest=true,everGpuWritten=false;u32 lastRenderPassIndex=0;
 texture::RenderPassUsage lastRenderPassUsage=texture::RenderPassUsage::None;
 Format fmt;Format*format=&fmt;
 vk::PipelineStageFlags writeStageMask{},pendingStageMask{},readStageMask{};
 void UpdateRenderPassUsage(u32,texture::RenderPassUsage);
 void PrepareShaderWrite(vk::PipelineStageFlagBits,vk::PipelineStageFlags&,vk::PipelineStageFlags&);
 void PopulateReadBarrier(vk::PipelineStageFlagBits,vk::PipelineStageFlags&,vk::PipelineStageFlags&);
};
'''

TESTS = r'''
int main(){
 using S=vk::PipelineStageFlagBits;
 Texture t;vk::PipelineStageFlags src{},dst{};
 t.PrepareShaderWrite(S::eComputeShader,src,dst);
 assert(t.everGpuWritten && !src && !dst);
 t.PopulateReadBarrier(S::eFragmentShader,src,dst);
 assert(src==S::eComputeShader && dst==S::eFragmentShader);
 src={};dst={};t.PopulateReadBarrier(S::eFragmentShader,src,dst);
 assert(!src && !dst); // same read visibility need not be flushed again
 t.PrepareShaderWrite(S::eComputeShader,src,dst);
 assert((src&S::eFragmentShader) && (src&S::eComputeShader) && dst==S::eComputeShader);
 src={};dst={};t.PopulateReadBarrier(S::eFragmentShader,src,dst);
 assert(src==S::eComputeShader && dst==S::eFragmentShader); // new write invalidates it
 t.PrepareShaderWrite(S::eVertexShader,src,dst);
 assert((t.writeStageMask&S::eComputeShader) && (t.writeStageMask&S::eVertexShader));
 t.UpdateRenderPassUsage(0,texture::RenderPassUsage::None);
 assert(!t.writeStageMask && !t.readStageMask && !t.pendingStageMask && t.everGpuWritten);
 Texture sampled;src={};dst={};sampled.PopulateReadBarrier(S::eComputeShader,src,dst);
 assert(!sampled.everGpuWritten && !src && !dst);
 sampled.PrepareShaderWrite(S::eFragmentShader,src,dst);
 assert(src==S::eComputeShader && dst==S::eFragmentShader); // read -> write
 for(auto aspect:{vk::ImageAspectFlagBits::eColor,vk::ImageAspectFlagBits::eDepth}){
  Texture rt;rt.fmt.vkAspect=aspect;rt.UpdateRenderPassUsage(1,texture::RenderPassUsage::RenderTarget);
  src={};dst={};rt.PopulateReadBarrier(S::eComputeShader,src,dst);
  const auto writer=aspect==vk::ImageAspectFlagBits::eColor?vk::PipelineStageFlags(S::eColorAttachmentOutput):S::eEarlyFragmentTests|S::eLateFragmentTests;
  assert(src==writer && dst==S::eComputeShader && rt.everGpuWritten);
 }
 std::cout<<"PASS: shader-image RAW/WAR/WAW, repeated writes, multiple producers, RT/depth, submission reset\n";
 size_t cases=0;
 for(u32 w:{1U,17U,65U,129U,257U})for(u32 h:{1U,9U,33U,65U})for(u32 d:{1U,3U,17U})
 for(size_t bw:{1U,4U})for(size_t bpb:{4U,8U,16U})for(size_t bh:{1U,2U,8U}){
  const size_t bd=d==1?1:4,levels=8;
  const auto actual=GetBlockLinearMipLayout({w,h,d},bw,bw,bpb,1,1,4,bh,bd,levels);
  size_t total=0,blockH=bh,blockD=bd;
  for(size_t i=0;i<levels;++i){
   const size_t x=std::max(w>>i,1U),y=std::max(h>>i,1U),z=std::max(d>>i,1U);
   const size_t rows=(y+bw-1)/bw,bytes=(x+bw-1)/bw*bpb;
   if(i){while(blockH>1 && blockH/2*8>=rows)blockH/=2;while(blockD>1 && blockD/2>=z)blockD/=2;}
   const auto expected=((bytes+63)/64)*512*blockH*blockD*((rows+8*blockH-1)/(8*blockH))*((z+blockD-1)/blockD);
   assert(actual[i].blockLinearSize==expected);
   assert(actual[i].linearSize==bytes*rows*z);
   assert(actual[i].targetLinearSize==x*y*z*4);
   assert(actual[i].blockHeight==blockH && actual[i].blockDepth==blockD);
   total+=expected;
  }
  assert((GetBlockLinearLayerSize({w,h,d},bw,bw,bpb,bh,bd,levels,false)==total));
  const size_t alignment=512*bh*bd;
  assert((GetBlockLinearLayerSize({w,h,d},bw,bw,bpb,bh,bd,levels,true)==(total+alignment-1)/alignment*alignment));
  ++cases;
 }
 std::cout<<"PASS: "<<cases<<" NPOT/compressed/3D mip chains vs independent byte-size oracle\n";
 for(u32 layer=0;layer<3;++layer)for(u32 mip=0;mip<3;++mip){
  const size_t sizes[]={4096,1024,512},offsets[]={0,4096,5120};
  auto found=FindMip(layer*8192+offsets[mip],sizes[mip]);
  assert(found && found->first==layer && found->second==mip);
 }
 assert(!FindMip(4097,1024));assert(!FindMip(4096,512));assert(!FindMip(3*8192,4096));
 std::cout<<"PASS: all layer/mip aliases, wrong offsets/sizes rejected\n";
 PassFixture pass;
 assert(!pass.Split({},{}));
 assert(pass.Split(S::eComputeShader,S::eFragmentShader));
 assert(pass.Split(S::eFragmentShader,S::eFragmentShader));
 assert(!pass.Split({},S::eFragmentShader));
 pass.attachmentsMatch=false;assert(pass.Split({},{}));pass.attachmentsMatch=true;
 pass.renderArea=2;assert(pass.Split({},{}));pass.renderArea=1;
 pass.texture.compatible=false;assert(pass.Split({},{}));
 std::cout<<"PASS: production pass-splitting condition keeps dependent draws in separate passes\n";
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', choices=['layout', 'mip'])
    args = parser.parse_args()

    def read(path):
        if (args.baseline == 'layout' and path.name == 'layout.cpp') or (args.baseline == 'mip' and path.name == 'texture_manager.cpp'):
            return subprocess.check_output(['git', 'show', 'HEAD:' + str(path.relative_to(ROOT))], cwd=ROOT, text=True)
        return path.read_text()

    cpp = (SRC / 'gpu/texture/texture.cpp').read_text()
    code = PREFIX + '\n'.join(block(cpp, m) for m in [
        'void Texture::UpdateRenderPassUsage(', 'void Texture::PrepareShaderWrite(',
        'void Texture::PopulateReadBarrier('])
    layout = read(SRC / 'gpu/texture/layout.cpp')
    code += '\n' + block(layout, 'template<typename Type>')
    code += '\n' + block(layout, 'size_t GetBlockLinearLayerSize(Dimensions dimensions, size_t formatBlockHeight')
    code += '\n' + block(layout, 'std::vector<MipLevelLayout> GetBlockLinearMipLayout(')
    lookup = block(read(SRC / 'gpu/texture_manager.cpp'), 'for (u32 layer{};')
    code += r'''
std::optional<std::pair<u32,u32>> FindMip(size_t memOffset,size_t wanted){
 struct Guest{size_t stride;size_t GetLayerStride(){return stride;}size_t CalculateLayerSize(){return stride;}};
 struct Mip{size_t blockLinearSize;};
 struct Tex{u32 layerCount=3;std::vector<Mip>mipLayouts{{4096},{1024},{512}};}tex;
 struct Mapping{Tex*texture;}mapping{&tex};auto*hostMapping=&mapping;
 Guest matchGuestTexture{8192},guestTexture{wanted};
 size_t layerMemOffset=0;bool matched=false;u32 matchLevel=99,matchLayer=99;
''' + lookup + r'''
 if(matched)return std::pair{matchLayer,matchLevel};return {};
}
'''
    split = block((SRC/'gpu/interconnect/command_executor.cpp').read_text(), 'bool splitRenderPass')
    code += r'''
namespace ranges=std::ranges;
struct PassFixture {
 struct Pass{int renderArea=1;}p;Pass*renderPass=&p;
 int renderArea=1;bool attachmentsMatch=true;u32 renderPassIndex=1;
 struct T{bool compatible=true;bool ValidateRenderPassUsage(u32,texture::RenderPassUsage){return compatible;}}texture;
 struct V{T*texture;}view{&texture};
 std::vector<V*>outputAttachmentViews{&view},sampledImages;
 bool Split(vk::PipelineStageFlags srcStageMask,vk::PipelineStageFlags dstStageMask){
''' + split + '; return splitRenderPass; } };\n' + TESTS
    with tempfile.TemporaryDirectory(prefix='strato-image-coherence-') as tmp:
        cpp_path = Path(tmp) / 'test.cpp'
        exe = Path(tmp) / 'test'
        cpp_path.write_text(code)
        subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++20', '-O1', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                        '-I' + str(ROOT / 'app/libraries/vkhpp'),
                        '-I' + str(ROOT / 'app/libraries/vkhpp/Vulkan-Headers/include'),
                        str(cpp_path), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=30,
                       env={**os.environ, 'ASAN_OPTIONS':'detect_leaks=0', 'UBSAN_OPTIONS':'halt_on_error=1'})


if __name__ == '__main__':
    main()
