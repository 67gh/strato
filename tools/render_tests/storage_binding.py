#!/usr/bin/env python3
"""Exercise the production storage-image binding with CPU fakes and real Vulkan types."""
from pathlib import Path
import os
import subprocess
import tempfile
from image_coherence import block, ROOT, SRC

PREFIX = r'''
#define VULKAN_HPP_NO_STRUCT_CONSTRUCTORS
#define VULKAN_HPP_NO_STRUCT_SETTERS
#include <vulkan/vulkan.hpp>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <utility>
using u32=uint32_t;
struct TextureView;
struct Texture {
 vk::ImageLayout layout=vk::ImageLayout::eGeneral;
 int reads=0,writes=0,dirty=0,transitions=0;
 vk::ImageSubresourceRange usedRange{};vk::ComponentMapping usedMapping{};
 void TransitionLayout(vk::ImageLayout l){++transitions;layout=l;}
 void PrepareShaderWrite(vk::PipelineStageFlagBits,vk::PipelineStageFlags&,vk::PipelineStageFlags&){++writes;}
 void PopulateReadBarrier(vk::PipelineStageFlagBits,vk::PipelineStageFlags&,vk::PipelineStageFlags&){++reads;}
 void MarkGpuDirty(int){++dirty;}
 std::shared_ptr<TextureView> GetView(vk::ImageViewType,vk::ImageSubresourceRange,vk::Format,vk::ComponentMapping);
};
struct TextureView {
 Texture*texture;
 vk::ImageViewType type=vk::ImageViewType::e2DArray;
 vk::Format format=vk::Format::eR8G8B8A8Unorm;
 vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor,2,4,3,5};
 vk::ComponentMapping mapping{vk::ComponentSwizzle::eB,vk::ComponentSwizzle::eG,vk::ComponentSwizzle::eR,vk::ComponentSwizzle::eA};
 vk::ImageView GetView(){return vk::ImageView(reinterpret_cast<VkImageView>(uintptr_t{123}));}
};
std::shared_ptr<TextureView> Texture::GetView(vk::ImageViewType type,vk::ImageSubresourceRange r,vk::Format f,vk::ComponentMapping m){
 assert(type==vk::ImageViewType::e2DArray && f==vk::Format::eR8G8B8A8Unorm);
 usedRange=r;usedMapping=m;return std::make_shared<TextureView>(TextureView{this});
}
struct InterconnectContext {struct Executor{int usageTracker=0,attached=0;void AttachTexture(TextureView*){++attached;}}executor;};
struct Textures{TextureView*view;TextureView*GetTexture(InterconnectContext&,u32 index,int type){assert(index==7&&type==9);return view;}};
struct BindlessHandle{u32 textureIndex;};
struct Descriptor{int type=9;bool is_written=false;};
'''
MAIN = r'''
int main(){
 Texture texture;TextureView sampled{&texture};Textures textures{&sampled};InterconnectContext ctx;
 vk::PipelineStageFlags src{},dst{};Descriptor desc;
 auto result=GetStorageImageBinding(ctx,desc,textures,BindlessHandle{7},vk::PipelineStageFlagBits::eComputeShader,src,dst);
 assert(result.second==&sampled && result.first.imageLayout==vk::ImageLayout::eGeneral && !result.first.sampler);
 assert(result.first.imageView==sampled.GetView());
 assert(texture.usedRange.baseMipLevel==2 && texture.usedRange.levelCount==1);
 assert(texture.usedRange.baseArrayLayer==3 && texture.usedRange.layerCount==5);
 assert(texture.usedMapping==vk::ComponentMapping{});
 assert(sampled.range.levelCount==4 && sampled.mapping.r==vk::ComponentSwizzle::eB);
 assert(texture.reads==1 && !texture.writes && !texture.dirty && !texture.transitions);
 desc.is_written=true;
 GetStorageImageBinding(ctx,desc,textures,BindlessHandle{7},vk::PipelineStageFlagBits::eFragmentShader,src,dst);
 assert(texture.reads==1 && texture.writes==1 && texture.dirty==1 && !texture.transitions);
 texture.layout=vk::ImageLayout::eTransferDstOptimal;
 GetStorageImageBinding(ctx,desc,textures,BindlessHandle{7},vk::PipelineStageFlagBits::eComputeShader,src,dst);
 assert(texture.transitions==1 && texture.writes==2 && ctx.executor.attached==3);
 std::cout<<"PASS: actual storage binding, write/read tracking, no redundant layout wait, one mip/identity swizzle, sampled view unchanged\n";
}
'''


def main():
    implementation = block((SRC/'gpu/interconnect/common/pipeline.inc').read_text(),
                           'static std::pair<vk::DescriptorImageInfo, TextureView *> GetStorageImageBinding(')
    with tempfile.TemporaryDirectory(prefix='strato-storage-binding-') as tmp:
        cpp = Path(tmp)/'test.cpp'
        exe = Path(tmp)/'test'
        cpp.write_text(PREFIX + implementation + MAIN)
        subprocess.run([os.environ.get('CXX','g++'),'-std=c++20','-O1','-g',
                        '-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie',
                        '-I'+str(ROOT/'app/libraries/vkhpp'),
                        '-I'+str(ROOT/'app/libraries/vkhpp/Vulkan-Headers/include'),
                        str(cpp),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=30,
                       env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})


if __name__=='__main__':
    main()
