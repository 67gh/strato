import os
from pathlib import Path
import subprocess
r=Path(__file__).resolve().parents[1];out=r/'results';out.mkdir(exist_ok=True)
s=(r/'app/src/main/cpp/skyline/gpu/texture/texture.cpp').read_text()
def block(key):
 a=s.index(key);b=s.index('{',a);i=b+1;d=1
 while d:d+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[a:i]
copy=block('void Texture::CopyToGuest(')
sync=block('void Texture::SynchronizeGuest(bool')
# Extract the single-level upload loop without changing its statements.
a=s.index('        auto guestLayerStride{',s.index('Texture::SynchronizeHostImpl'))
b=s.index('        } else if (levelCount > 1',a)
upload='void Texture::Upload(u8 *pointer,u8 *deswizzleOutput) {\n'+s[a:b]+'        }\n}\n'
pre=r'''
#include <vector>
#include <span>
#include <memory>
#include <variant>
#include <mutex>
#include <optional>
#include <cstring>
#include <cassert>
#include <iostream>
using u8=unsigned char;
struct exception:std::runtime_error{template<class...T>exception(const char*s,T...):std::runtime_error(s){}};
#define LOGE(...) ((void)0)
#define TRACE_EVENT(...) ((void)0)
namespace vk{enum class ImageLayout{eUndefined,eGeneral};enum class ImageTiling{eOptimal,eLinear,eOther};template<class T>int to_string(T){return 0;}namespace raii{struct CommandBuffer{};}}
namespace memory{struct Image{std::vector<u8>b;u8*data(){return b.data();}};using StagingBuffer=Image;}
struct Fmt{int vkFormat=1,blockWidth=1,blockHeight=1,bpb=1;};
namespace texture{enum class TileMode{Block,Pitch,Linear};struct Guest{struct{TileMode mode=TileMode::Linear;}tileConfig;size_t stride=12;Fmt*format;size_t GetLayerStride(){return stride;}};
void CopyLinearToBlockLinear(Guest&,u8*,u8*){throw exception("mock tile");}void CopyLinearToPitchLinear(Guest&,u8*,u8*){throw exception("mock tile");}void CopyBlockLinearToLinear(Guest&,u8*,u8*){throw exception("mock tile");}void CopyPitchLinearToLinear(Guest&,u8*,u8*){throw exception("mock tile");}template<class...T>void CopyLinearToBlockLinear(int,T...){throw exception("mock tile");}}
int failure{},copied{},deleted{},trapped{},waited{};
struct Cycle{void Wait(){if(failure==2)throw exception("fence");++waited;}};
struct GPU{struct Scheduler{template<class F>std::shared_ptr<Cycle>Submit(F f){if(failure==1)throw exception("submit");vk::raii::CommandBuffer c;f(c);return std::make_shared<Cycle>();}}scheduler;
struct Mem{std::shared_ptr<memory::StagingBuffer>AllocateStagingBuffer(size_t n){auto b=std::make_shared<memory::StagingBuffer>();b->b.resize(n,0x5a);return b;}}memory;
struct Trap{void DeleteTrap(int){++deleted;}void TrapRegions(int,bool){++trapped;}};
struct Process{Trap trap;}process;
struct State{Process*process;}state{&process};};
struct Texture{
enum class DirtyState{GpuDirty,CpuDirty,Clean};DirtyState dirtyState=DirtyState::GpuDirty;
GPU gpu;std::optional<texture::Guest>guest;Fmt*format;vk::ImageLayout layout=vk::ImageLayout::eGeneral;vk::ImageTiling tiling=vk::ImageTiling::eOptimal;
std::recursive_mutex stateMutex;bool memoryFreed=true;std::optional<int>trapHandle=1;size_t levelCount=1,layerCount=2,layerStride=8,deswizzledLayerStride=8,surfaceSize=16;
std::span<u8>mirror;struct Level{int dimensions;size_t blockHeight,blockDepth,linearSize,blockLinearSize;};std::vector<Level>mipLayouts;
std::variant<int,memory::Image>backing;std::shared_ptr<memory::StagingBuffer>downloadStagingBuffer;
void WaitOnBacking(){if(failure==3)throw exception("backing");}void WaitOnFence(){}void CopyIntoStagingBuffer(vk::raii::CommandBuffer&,std::shared_ptr<memory::StagingBuffer>){++copied;}
void CopyToGuest(u8*);void SynchronizeGuest(bool,bool=false);void Upload(u8*,u8*);
};
'''
main=r'''
int main(){Fmt f1,f2;for(size_t n:{size_t(1),size_t(2),size_t(8)}){
 Texture t;t.format=&f1;t.guest=texture::Guest{};t.guest->format=&f1;t.layerCount=n;t.surfaceSize=n*8;
 std::vector<u8>guest(n*12+16,0xa5),host(n*8+16,0x5a);t.mirror={guest.data()+8,n*12};auto before=host;t.CopyToGuest(host.data()+8);assert(host==before);
 for(size_t i=0;i<guest.size();i++){bool pixel=i>=8&&i<8+n*12&&(i-8)%12<8;assert(guest[i]==(pixel?0x5a:0xa5));}
 std::fill(host.begin(),host.end(),0x33);auto guestBefore=guest;t.Upload(guest.data()+8,host.data()+8);assert(guest==guestBefore);
 for(size_t i=0;i<host.size();i++)assert(host[i]==(i>=8&&i<8+n*8?0x5a:0x33));
 }
 for(bool cpuDirty:{false,true})for(bool skip:{false,true}){
 Texture t;t.format=&f1;t.guest=texture::Guest{};t.guest->format=&f2;std::vector<u8>data(24,0x33);t.mirror=data;bool threw=false;try{t.SynchronizeGuest(cpuDirty,skip);}catch(...){threw=true;}assert(threw);assert(t.dirtyState==Texture::DirtyState::GpuDirty&&t.memoryFreed);assert(!copied&&!deleted&&!trapped);
 t.guest->format=&f1;
 for(failure=1;failure<=3;failure++){threw=false;try{t.SynchronizeGuest(cpuDirty,skip);}catch(...){threw=true;}assert(threw&&t.dirtyState==Texture::DirtyState::GpuDirty&&t.memoryFreed);assert(!deleted&&!trapped);}
 failure=0;t.SynchronizeGuest(cpuDirty,skip);assert(!t.memoryFreed);assert(t.dirtyState==(cpuDirty?Texture::DirtyState::CpuDirty:Texture::DirtyState::Clean));assert(deleted==(!skip&&cpuDirty));assert(trapped==(!skip&&!cpuDirty));assert(data[0]==0x5a&&data[12]==0x5a);deleted=trapped=copied=0;
 }
 Texture undefined;undefined.format=&f1;undefined.guest=texture::Guest{};undefined.guest->format=&f2;undefined.layout=vk::ImageLayout::eUndefined;undefined.SynchronizeGuest(false);assert(undefined.dirtyState==Texture::DirtyState::Clean);
 Texture badMip;badMip.format=&f1;badMip.guest=texture::Guest{};badMip.guest->format=&f1;badMip.levelCount=2;bool mipThrew=false;try{badMip.SynchronizeGuest(false);}catch(...){mipThrew=true;}assert(mipThrew&&badMip.memoryFreed&&badMip.dirtyState==Texture::DirtyState::GpuDirty);
 Texture empty;empty.SynchronizeGuest(false);assert(empty.dirtyState==Texture::DirtyState::GpuDirty);
 Texture linear;linear.format=&f1;linear.guest=texture::Guest{};linear.guest->format=&f1;linear.tiling=vk::ImageTiling::eLinear;linear.backing=memory::Image{std::vector<u8>(16,0x9b)};std::vector<u8>data(24);linear.mirror=data;linear.SynchronizeGuest(false);assert(data[0]==0x9b&&data[12]==0x9b&&!linear.memoryFreed);
 std::cout<<"Texture: 1/2/8 layers, upload/readback direction, guard bytes, source unchanged; unsupported format and backing/submit/fence failure preserve dirty state; successful staging/linear readback, skip-trap/CPU-dirty and undefined/no-guest paths OK\n";
}
'''
(out/'texture.cpp').write_text(pre+copy+sync+upload+main)
p=subprocess.run(['g++','-std=c++20','-pthread','-fsanitize=address,undefined','-no-pie',str(out/'texture.cpp'),'-o',str(out/'texture')],capture_output=True,text=True);(out/'texture_compile.log').write_text(p.stdout+p.stderr);assert p.returncode==0,p.stderr
p=subprocess.run([str(out/'texture')],capture_output=True,text=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'));(out/'texture.log').write_text(p.stdout+p.stderr);print(p.stdout+p.stderr);assert p.returncode==0
