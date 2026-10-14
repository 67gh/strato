#!/usr/bin/env python3
"""Exercise production buffer transitions and compute pipeline selection on the host.
Only surrounding dependencies are doubles. No Android/GPU execution is claimed.
"""
from pathlib import Path
import os, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
SRC=ROOT/'app/src/main/cpp/skyline'
def extract(text,marker):
    start=text.index(marker); p=text.index('{',start); depth=1; end=p+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}'); end+=1
    return text[start:end]
HEADER=r"""
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>
using u32=uint32_t;using ContextTag=uint64_t;
"""
def buffer_test():
    cpp=(SRC/'gpu/buffer.cpp').read_text(); h=(SRC/'gpu/buffer.h').read_text()
    mocks=r"""
enum class DirtyState{Clean,CpuDirty,GpuDirty};
enum class BackingImmutability{None,SequencedWrites,AllWrites};
struct Trap{int calls=0;void TrapRegions(int,bool){++calls;}};
struct Process{Trap trap;};
struct GPU{struct {Process*process;} state;};
class Buffer{public:
    GPU gpu;std::recursive_mutex stateMutex;std::mutex mutex;
    DirtyState dirtyState=DirtyState::Clean;
    BackingImmutability backingImmutability=BackingImmutability::None;
    bool isDirect=false,currentExecutionGpuDirty=false;
    ContextTag tag=0;std::optional<int> trapHandle=1;
    int uploads=0,sequenceNumber=0,megabufferResets=0;
    explicit Buffer(Process&p):gpu{{&p}}{}
    void SynchronizeHost(bool){++uploads;assert(dirtyState==DirtyState::CpuDirty);dirtyState=DirtyState::Clean;}
    void ResetMegabufferState(){++megabufferResets;}
    void MarkGpuDirtyImplStaged();void AdvanceSequence();void unlock();
"""
    mocks+=extract(h,'void BlockAllCpuBackingWrites()')+'\n'+extract(h,'void AllowAllBackingWrites()')+'\n};\n'
    methods='\n'.join(extract(cpp,m) for m in ['void Buffer::MarkGpuDirtyImplStaged()', 'void Buffer::AdvanceSequence()', 'void Buffer::unlock()'])
    main=r"""
int main(){
 Process process;Buffer b(process);
 for(auto initial:{DirtyState::Clean,DirtyState::CpuDirty,DirtyState::GpuDirty}){
  b.dirtyState=initial;b.uploads=0;process.trap.calls=0;
  for(int pass=0;pass<3;++pass){
   b.mutex.lock();const auto sequence=b.sequenceNumber;
   b.MarkGpuDirtyImplStaged();
   assert(b.dirtyState==DirtyState::GpuDirty);
   assert(b.backingImmutability==BackingImmutability::AllWrites);
   assert(b.sequenceNumber==sequence+1);
   assert(b.megabufferResets==b.sequenceNumber);
   b.currentExecutionGpuDirty=true;b.tag=42;b.unlock();
   assert(b.backingImmutability==BackingImmutability::None);
   assert(!b.currentExecutionGpuDirty && !b.tag);
  }
  assert(b.uploads==(initial==DirtyState::CpuDirty?1:0));
  assert(process.trap.calls==(initial==DirtyState::GpuDirty?0:1));
 }
 std::cout<<"PASS: repeated GPU writes renew protection/version after unlock; upload only when CPU dirty\n";
}
"""
    return HEADER+mocks+methods+main

def pipeline_test():
    cpp=(SRC/'gpu/interconnect/kepler_compute/pipeline_manager.cpp').read_text()
    h=(SRC/'gpu/interconnect/kepler_compute/pipeline_manager.h').read_text()
    mocks=r"""
namespace Shader {enum class ImageFormat{Typeless,Typed};enum class ImageNumericType{UnsignedInt,SignedInt,Float};}
struct Desc{Shader::ImageFormat format=Shader::ImageFormat::Typeless;Shader::ImageNumericType numeric_type=Shader::ImageNumericType::UnsignedInt;u32 count=1,cbuf_offset=0;};
struct InterconnectContext{};
struct Textures{std::vector<Shader::ImageNumericType>types;auto GetTexelBufferNumericType(InterconnectContext&,u32 i){return types.at(i);}};
struct ConstantBufferSet{std::vector<u32>handles;};
struct Handle{u32 textureIndex;};
Handle ReadBindlessHandle(InterconnectContext&,ConstantBufferSet&c,const Desc&d,u32 i){return {c.handles.at(d.cbuf_offset+i)};}
using PackedPipelineState=int;using ShaderBinary=int;
struct Pipeline{struct {struct {std::vector<Desc>image_buffer_descriptors;}info;}shaderStage;
 static inline int made=0;int id=++made;
 Pipeline(InterconnectContext&,Textures&t,ConstantBufferSet&c,int,const ShaderBinary&){
  shaderStage.info.image_buffer_descriptors.push_back({.numeric_type=t.types.at(c.handles.at(0))});
 }
 bool MatchesTexelTypes(InterconnectContext&,Textures&,ConstantBufferSet&) const;
};
struct PipelineManager{std::unordered_map<int,std::vector<std::unique_ptr<Pipeline>>> map;
"""
    mocks+=extract(h,'Pipeline *FindOrCreate(')+'\n};\n'+extract(cpp,'bool Pipeline::MatchesTexelTypes(')
    main=r"""
int main(){
 using Type=Shader::ImageNumericType;InterconnectContext ctx;Textures t{{Type::UnsignedInt,Type::SignedInt,Type::Float}};ConstantBufferSet c{{0}};PipelineManager m;
 auto*u=m.FindOrCreate(ctx,t,c,7,0);c.handles[0]=1;auto*s=m.FindOrCreate(ctx,t,c,7,0);c.handles[0]=2;auto*f=m.FindOrCreate(ctx,t,c,7,0);
 assert(u!=s && s!=f && f!=u && Pipeline::made==3);
 c.handles[0]=0;assert(m.FindOrCreate(ctx,t,c,7,0)==u);
 // Change the TIC in-place, keeping the same shader state and handle.
 t.types[0]=Type::Float;assert(m.FindOrCreate(ctx,t,c,7,0)==f);
 assert(u->id==1 && s->id==2 && f->id==3);
 assert(m.FindOrCreate(ctx,t,c,8,0)!=f);
 // Check every element and ignore non-formatless descriptors.
 c.handles={0,1};f->shaderStage.info.image_buffer_descriptors[0].count=2;
 assert(!f->MatchesTexelTypes(ctx,t,c));t.types[1]=Type::Float;assert(f->MatchesTexelTypes(ctx,t,c));
 f->shaderStage.info.image_buffer_descriptors[0].format=Shader::ImageFormat::Typed;c.handles.clear();assert(f->MatchesTexelTypes(ctx,t,c));
 std::cout<<"PASS: numeric variants, TIC mutation, variant reuse, lifetime, all descriptor elements\n";
}
"""
    return HEADER+mocks+main

def main():
 with tempfile.TemporaryDirectory(prefix='strato-render-') as tmp:
  base=Path(tmp)
  flags=[os.environ.get('CXX','g++'),'-std=c++20','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']
  for name,source in [('buffer',buffer_test()),('pipelines',pipeline_test())]:
   cpp=base/(name+'.cpp');exe=base/name;cpp.write_text(source)
   subprocess.run(flags+[str(cpp),'-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True,timeout=20,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
   if name=='buffer':
    old=source.replace('if (dirtyState != DirtyState::GpuDirty) {','if (dirtyState == DirtyState::GpuDirty) return;\n        if (dirtyState != DirtyState::GpuDirty) {')
    cpp.write_text(old);subprocess.run(flags+[str(cpp),'-o',str(exe)],check=True)
    failure=subprocess.run([str(exe)],capture_output=True,timeout=20)
    assert failure.returncode!=0 and b'Assertion' in failure.stderr
    print('PASS: former early-return bug fails this regression test')
if __name__=='__main__':main()
