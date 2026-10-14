// SPDX-License-Identifier: MPL-2.0
#include <cassert>
#include <fstream>
#include <iostream>
#include <map>
#include <shader_compiler/backend/spirv/spirv_emit_context.h>
#include <shader_compiler/backend/spirv/emit_spirv_instructions.h>
#include <shader_compiler/frontend/ir/modifiers.h>
namespace Shader::Log {
void Debug(const std::string&) {}
void Warn(const std::string&) {}
void Error(const std::string&) {}
}
using namespace Shader;
using namespace Shader::Backend::SPIRV;
using Sirit::Id;
void test(ImageNumericType numeric, ImageFormat format, bool atomic, const char* path) {
    Profile profile{};
    profile.supported_spirv=0x00010300;
    profile.support_typeless_image_loads=true;
    RuntimeInfo runtime{}; Backend::Bindings bindings{}; IR::Program program{};
    program.stage=Stage::Compute;
    program.workgroup_size={1,1,1};
    program.info.uses_image_buffers=true;
    program.info.uses_atomic_image_u32=atomic;
    for(int i=0;i<2;++i)
        program.info.image_buffer_descriptors.push_back({.format=format,.is_written=true,.is_read=true,.count=1,.numeric_type=numeric});
    EmitContext ctx(profile,runtime,program,bindings);
    const Id scalar=format==ImageFormat::R8_SINT || format==ImageFormat::R16_SINT ? ctx.S32[1] :
        format!=ImageFormat::Typeless ? ctx.U32[1] :
        numeric==ImageNumericType::Float ? ctx.F32[1] : numeric==ImageNumericType::SignedInt ? ctx.S32[1] : ctx.U32[1];
    const Id vector=format==ImageFormat::R8_SINT || format==ImageFormat::R16_SINT ? ctx.S32[4] :
        format!=ImageFormat::Typeless ? ctx.U32[4] :
        numeric==ImageNumericType::Float ? ctx.F32[4] : numeric==ImageNumericType::SignedInt ? ctx.S32[4] : ctx.U32[4];
    assert(ctx.image_buffers.at(1).vector_type.value==vector.value);
    auto voidType=ctx.TypeVoid();auto fn=ctx.OpFunction(voidType,spv::FunctionControlMask::MaskNone,ctx.TypeFunction(voidType));
    ctx.AddLabel();
    IR::TextureInstInfo info{};info.type.Assign(TextureType::Buffer);info.image_format.Assign(format);info.descriptor_index.Assign(1);
    IR::Inst read(IR::Opcode::ImageRead,0);read.SetFlags(info);
    auto result=EmitImageRead(ctx,&read,IR::Value{},ctx.Const(0U));
    IR::Inst write(IR::Opcode::ImageWrite,0);write.SetFlags(info);
    EmitImageWrite(ctx,&write,IR::Value{},ctx.Const(0U),result);
    if(atomic){
        IR::Inst inst(IR::Opcode::ImageAtomicIAdd32,0);inst.SetFlags(info);
        (void)EmitImageAtomicIAdd32(ctx,&inst,IR::Value{},ctx.Const(0U),ctx.Const(1U));
    }
    ctx.OpReturn();ctx.OpFunctionEnd();
    ctx.AddEntryPoint(spv::ExecutionModel::GLCompute,fn,"main");
    ctx.AddExecutionMode(fn,spv::ExecutionMode::LocalSize,1U,1U,1U);
    ctx.AddCapability(spv::Capability::ImageBuffer);
    ctx.AddCapability(spv::Capability::StorageImageExtendedFormats);
    ctx.AddCapability(spv::Capability::StorageImageReadWithoutFormat);
    ctx.AddCapability(spv::Capability::StorageImageWriteWithoutFormat);
    auto words=ctx.Assemble();
    std::map<u32,u32> resultTypes;
    bool sawRead=false,sawWrite=false,sawImage=false,sawAtomic=false;
    int bitcasts=0;
    for(size_t p=5;p<words.size();){
        auto n=words[p]>>16;auto op=static_cast<spv::Op>(words[p]&0xffff);
        assert(n && p+n<=words.size());
        const auto* w=words.data()+p;
        if(op==spv::Op::OpTypeImage && w[1]==ctx.image_buffers[1].image_type.value){assert(w[2]==scalar.value);sawImage=true;}
        if(op==spv::Op::OpImageRead){assert(w[1]==vector.value);resultTypes[w[2]]=w[1];sawRead=true;}
        if(op==spv::Op::OpBitcast){resultTypes[w[2]]=w[1];++bitcasts;}
        if(op==spv::Op::OpImageWrite){assert(resultTypes.at(w[3])==vector.value);sawWrite=true;}
        if(op==spv::Op::OpImageTexelPointer){assert(w[3]==ctx.image_buffers[1].id.value);sawAtomic=true;}
        p+=n;
    }
    assert(sawImage && sawRead && sawWrite && sawAtomic==atomic);
    assert(bitcasts==(vector.value==ctx.U32[4].value?0:2));
    std::ofstream output(path,std::ios::binary);output.write(reinterpret_cast<const char*>(words.data()),words.size()*4);
}
int main(int argc,char**argv){
    assert(argc==2);std::string base=argv[1];
    test(ImageNumericType::UnsignedInt,ImageFormat::Typeless,false,(base+"/uint.spv").c_str());
    test(ImageNumericType::SignedInt,ImageFormat::Typeless,false,(base+"/sint.spv").c_str());
    test(ImageNumericType::Float,ImageFormat::Typeless,false,(base+"/float.spv").c_str());
    test(ImageNumericType::UnsignedInt,ImageFormat::R8_SINT,false,(base+"/r8sint.spv").c_str());
    test(ImageNumericType::UnsignedInt,ImageFormat::R32_UINT,true,(base+"/atomic.spv").c_str());
    std::cout<<"PASS: real SPIR-V emitter numeric types, read/write bitcasts, atomic descriptor 1 with empty IR index\n";
}
