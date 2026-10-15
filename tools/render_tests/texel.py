#!/usr/bin/env python3
"""Compile actual changed functions with Vulkan-Hpp and narrow test doubles.
No Android/GPU execution is claimed. Run from any directory with g++ installed.
"""
from pathlib import Path
import tempfile
import subprocess, os
project=Path(__file__).resolve().parents[2]
_temp=tempfile.TemporaryDirectory(prefix='strato-texel-')
root=Path(_temp.name)
src=project/'app/src/main/cpp/skyline/gpu/interconnect'
vkhpp=project/'app/libraries/vkhpp'

def extract(text, marker):
 a=text.index(marker);b=text.index('{',a);d=1;e=b+1
 while d:
  d+=(text[e]=='{')-(text[e]=='}');e+=1
 return text[a:e]

def run(name,code,syntax=False):
 path=root/(name+'.cpp');path.write_text(code)
 args=['g++','-std=c++20','-DVULKAN_HPP_NO_CONSTRUCTORS','-DVULKAN_HPP_NO_SETTERS','-I'+str(vkhpp),'-I'+str(vkhpp/'Vulkan-Headers/include')]
 args+=['-fsyntax-only',str(path)] if syntax else ['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',str(path),'-ldl','-o',str(root/name)]
 c=subprocess.run(args,capture_output=True,text=True);(root/(name+'.build.log')).write_text(c.stdout+c.stderr)
 if c.returncode:raise RuntimeError(name+' compile failed: '+c.stderr[:5000])
 if not syntax:
  c=subprocess.run([str(root/name)],capture_output=True,text=True,timeout=30,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
  (root/(name+'.log')).write_text(c.stdout+c.stderr)
  if c.returncode:raise RuntimeError(name+' failed: '+c.stderr[:3000])
 print(name+': PASS'+(' (API syntax)' if syntax else ' (ASan/UBSan)'))

header='''#include <utility>
#include <vulkan/vulkan_raii.hpp>
#include <array>
#include <span>
#include <vector>
#include <memory>
#include <variant>
#include <stdexcept>
#include <cassert>
#include <algorithm>
#include <cstdint>
#include <bit>
using u8=uint8_t; using u16=uint16_t; using u32=uint32_t; using u64=uint64_t;
template<class T>using span=std::span<T>;
template<class... Args>std::runtime_error exception(const char*s,Args...){return std::runtime_error(s);}
namespace util {template<class T,class U>T BitCast(U v){return std::bit_cast<T>(v);}}
'''
tic=(src/'common/tic.h').read_text().replace('#include <common/base.h>','')
header+=tic+'\nusing skyline::gpu::interconnect::TextureImageControl;\n'
common=(src/'common/common.h').read_text()
structs=extract(common,'struct TexelBufferBinding')+';\n'+extract(common,'struct TexelBufferDescriptors')+';\n'
mock='''
namespace skyline::diagnostics {template<class...T>void SaveArtifact(T...){}template<class...T>void Event(T...){} }

namespace Shader {enum class ImageFormat {Typeless,R8_UINT,R16_UINT,R32_UINT,R32G32_UINT,R32G32B32A32_UINT,R8_SINT};enum class ImageNumericType {UnsignedInt,SignedInt,Float};}
struct F {u8 bpb;vk::Format vkFormat;vk::ImageAspectFlags vkAspect=vk::ImageAspectFlagBits::eColor;vk::ComponentMapping swizzleMapping{vk::ComponentSwizzle::eR,vk::ComponentSwizzle::eG,vk::ComponentSwizzle::eB,vk::ComponentSwizzle::eA};bool IsCompressed()const{return false;}};
namespace format {const F R8Uint{1,vk::Format::eR8Uint},R16Uint{2,vk::Format::eR16Uint},R32Uint{4,vk::Format::eR32Uint},R32G32Uint{8,vk::Format::eR32G32Uint},R32G32B32A32Uint{16,vk::Format::eR32G32B32A32Uint},R32Float{4,vk::Format::eR32Sfloat},R32Sint{4,vk::Format::eR32Sint};}
struct Format {const F*p;Format(const F&f):p(&f){};Format():p(nullptr){}explicit operator bool()const{return p;}const F*operator->()const{return p;}};
Format ConvertTicFormat(TextureImageControl::FormatWord f,bool){if(f.format!=TextureImageControl::ImageFormat::R32)return {};if(f.componentR==TextureImageControl::ImageComponent::Sint)return format::R32Sint;return f.componentR==TextureImageControl::ImageComponent::Uint?format::R32Uint:format::R32Float;}
struct GPU;
struct BufferBinding {vk::Buffer buffer;vk::DeviceSize offset,size;};
struct Buffer {bool blocked=false,dirty=false;int reads=0;void PopulateReadBarrier(vk::PipelineStageFlagBits,vk::PipelineStageFlags&,vk::PipelineStageFlags&){reads++;}bool SequencedCpuBackingWritesBlocked(){return blocked;}void MarkGpuDirty(int){dirty=true;}void BlockSequencedCpuBackingWrites(){blocked=true;}};
static Buffer buffer;
struct BufferView {u64 size=0;explicit operator bool()const{return size!=0;}Buffer*GetBuffer(){return &buffer;}BufferBinding GetBinding(GPU&)const{return {vk::Buffer(reinterpret_cast<VkBuffer>(1)),256,size};}};
'''
mock+=structs+'''
struct Physical {vk::PhysicalDeviceLimits limits{.maxTexelBufferElements=1000000,.minTexelBufferOffsetAlignment=16};vk::FormatFeatureFlags features=vk::FormatFeatureFlagBits::eUniformTexelBuffer|vk::FormatFeatureFlagBits::eStorageTexelBuffer;vk::PhysicalDeviceProperties getProperties(){return {.limits=limits};}vk::FormatProperties getFormatProperties(vk::Format){return {.bufferFeatures=features};}};
struct GPU {Physical vkPhysicalDevice;struct {bool supportsImageReadWithoutFormat=true,supportsShaderStorageImageWriteWithoutFormat=true;}traits;};
struct Executor {int attached=0,usageTracker=0;void AttachBuffer(BufferView){attached++;}};
struct InterconnectContext {GPU gpu;Executor executor;};
static u64 mappedAddress=0,mappedSize=0;static bool split=false;
struct CachedMappedBufferView {BufferView view;void Update(InterconnectContext&,u64 address,u64 size,bool){mappedAddress=address;mappedSize=size;view.size=split?size-1:size;}};
struct Textures {struct Pool {std::vector<TextureImageControl>textureHeaders;Pool&UpdateGet(InterconnectContext&){return *this;}}texturePool;TexelBufferBinding GetTexelBuffer(InterconnectContext&,u32,bool,Shader::ImageFormat,Shader::ImageNumericType=Shader::ImageNumericType::UnsignedInt);Shader::ImageNumericType GetTexelBufferNumericType(InterconnectContext&,u32);};
union BindlessHandle {u32 raw;struct{u32 textureIndex:20;u32 samplerIndex:12;};};
'''
get=extract((src/'common/textures.cpp').read_text(),'Shader::ImageNumericType Textures::GetTexelBufferNumericType')+'\n'+extract((src/'common/textures.cpp').read_text().replace(') try {',') {'),'TexelBufferBinding Textures::GetTexelBuffer')
binding=extract((src/'common/pipeline.inc').read_text(),'static TexelBufferBinding GetTexelBufferBinding')
# Extract the production descriptors so a missing member in the graphics copy
# cannot be hidden by a test-only storage descriptor.
shader_info=(project/'app/libraries/shader-compiler/shader_info.h').read_text()
descriptors='namespace Shader {\n'+extract(shader_info,'struct ImageBufferDescriptor')+';\n}\n'
descriptors+=extract((src/'maxwell_3d/pipeline_manager.h').read_text(),'struct StorageTexelBufferDesc')+';\n'
main='''
template<class Fn>void rejects(Fn fn){bool rejected=false;try{fn();}catch(const std::runtime_error&){rejected=true;}assert(rejected);}
int main(){Textures textures;InterconnectContext ctx;TextureImageControl tic{};
tic.textureType=TextureImageControl::TextureType::e1DBuffer;tic.headerType=TextureImageControl::HeaderType::Buffer1D;tic.addressHigh=0x1234;tic.addressLow=0x56789000;tic.widthMinusOne=7;
auto&f=tic.formatWord;f.format=TextureImageControl::ImageFormat::R32;f.componentR=TextureImageControl::ImageComponent::Float;
f.swizzleX=TextureImageControl::ImageSwizzle::R;f.swizzleY=TextureImageControl::ImageSwizzle::Zero;f.swizzleZ=TextureImageControl::ImageSwizzle::Zero;f.swizzleW=TextureImageControl::ImageSwizzle::OneFloat;
textures.texturePool.textureHeaders={tic};auto get=[&](bool storage=false,Shader::ImageFormat sf=Shader::ImageFormat::Typeless,Shader::ImageNumericType nt=Shader::ImageNumericType::UnsignedInt){return textures.GetTexelBuffer(ctx,0,storage,sf,nt);};
auto b=get();assert(mappedAddress==0x123456789000ULL && mappedSize==32 && b.format==vk::Format::eR32Sfloat && b.alignment==16 && ctx.executor.attached==1);
auto&h=textures.texturePool.textureHeaders[0];h.tileConfig.widthMinusOne_16_31=1;get();assert(mappedSize==(65536+8)*4);h.tileConfig.widthMinusOne_16_31=0;
h.widthMinusOne=65535;h.tileConfig.widthMinusOne_16_31=65535;rejects([&]{get();});h=tic;
assert(get(true,Shader::ImageFormat::R32_UINT).format==vk::Format::eR32Uint);
rejects([&]{get(true,Shader::ImageFormat::R16_UINT);});rejects([&]{get(true,Shader::ImageFormat::R8_SINT);});rejects([&]{get(true);});
using Type=Shader::ImageNumericType;using SF=Shader::ImageFormat;
assert(get(true,SF::Typeless,Type::Float).format==vk::Format::eR32Sfloat);
h.formatWord.componentR=TextureImageControl::ImageComponent::Sint;
assert(get(true,SF::Typeless,Type::SignedInt).format==vk::Format::eR32Sint);
rejects([&]{get(true,SF::Typeless,Type::Float);});
h.formatWord.componentR=TextureImageControl::ImageComponent::Unorm;
assert(textures.GetTexelBufferNumericType(ctx,0)==Type::Float);
h=tic;
ctx.gpu.traits.supportsImageReadWithoutFormat=false;rejects([&]{get(true,SF::Typeless,Type::Float);});ctx.gpu.traits.supportsImageReadWithoutFormat=true;
ctx.gpu.traits.supportsShaderStorageImageWriteWithoutFormat=false;rejects([&]{get(true,SF::Typeless,Type::Float);});ctx.gpu.traits.supportsShaderStorageImageWriteWithoutFormat=true;
h.formatWord.componentR=TextureImageControl::ImageComponent::Uint;assert(get(true).format==vk::Format::eR32Uint);rejects([&]{get();});h=tic;
split=true;rejects([&]{get();});split=false;
ctx.gpu.vkPhysicalDevice.features={};rejects([&]{get();});ctx.gpu.vkPhysicalDevice.features=vk::FormatFeatureFlagBits::eUniformTexelBuffer|vk::FormatFeatureFlagBits::eStorageTexelBuffer;
h.formatWord.swizzleX=TextureImageControl::ImageSwizzle::G;rejects([&]{get();});h=tic;
h.textureType=TextureImageControl::TextureType::e2D;rejects([&]{get();});h=tic;
rejects([&]{textures.GetTexelBuffer(ctx,1,false,Shader::ImageFormat::Typeless);});
struct Sample{};
vk::PipelineStageFlags source{},dest{};
GetTexelBufferBinding(ctx,Sample{},textures,{.raw=0},vk::PipelineStageFlagBits::eFragmentShader,source,dest);assert(buffer.reads==1 && buffer.blocked && !buffer.dirty);
const std::array types{Type::UnsignedInt,Type::SignedInt,Type::Float};
const std::array components{TextureImageControl::ImageComponent::Uint,TextureImageControl::ImageComponent::Sint,TextureImageControl::ImageComponent::Float};
const std::array formats{vk::Format::eR32Uint,vk::Format::eR32Sint,vk::Format::eR32Sfloat};
for (std::size_t i=0;i<types.size();++i) {
    Shader::ImageBufferDescriptor original{};
    original.format=SF::Typeless;original.numeric_type=types[i];
    original.cbuf_index=3;original.cbuf_offset=64;original.count=1;original.size_shift=2;
    original.is_read=true;
    h=tic;h.formatWord.componentR=components[i];
    for (bool writable : {false,true}) {
        original.is_written=writable;
        StorageTexelBufferDesc graphics{original};
        assert(graphics.numeric_type==original.numeric_type && graphics.format==original.format);
        assert(graphics.cbuf_index==original.cbuf_index && graphics.cbuf_offset==original.cbuf_offset);
        assert(graphics.count==original.count && graphics.size_shift==original.size_shift);
        assert(graphics.is_read==original.is_read && graphics.is_written==original.is_written);
        auto changed=original;changed.numeric_type=types[(i+1)%types.size()];
        assert(graphics==StorageTexelBufferDesc{original});
        assert(graphics!=StorageTexelBufferDesc{changed});
        auto check=[&](const auto& desc,vk::PipelineStageFlagBits stage) {
            buffer=Buffer{};buffer.blocked=writable;source={};dest={};
            auto result=GetTexelBufferBinding(ctx,desc,textures,{.raw=0},stage,source,dest);
            assert(result.format==formats[i] && buffer.reads==1);
            assert(buffer.dirty==writable);
            if (writable) assert(source && dest);
            else assert(buffer.blocked);
        };
        check(original,vk::PipelineStageFlagBits::eComputeShader);
        check(graphics,vk::PipelineStageFlagBits::eFragmentShader);
    }
}

}
'''
run('tic_and_hazards',header+mock+descriptors+get+'\n'+binding+main)


_temp.cleanup()
