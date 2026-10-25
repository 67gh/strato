import os
from pathlib import Path
import subprocess
r=Path(__file__).resolve().parents[1];out=r/'results';out.mkdir(exist_ok=True)
s=(r/'app/src/main/cpp/skyline/gpu/graphics_pipeline_assembler.cpp').read_text();a=s.index('    static constexpr u64 MaxPipeline');b=s.index('    GraphicsPipelineAssembler::GraphicsPipelineAssembler',a)
pre=r'''
#include <filesystem>
#include <fstream>
#include <vector>
#include <string_view>
#include <stdexcept>
#include <cstdint>
#include <cassert>
#include <iostream>
#include <span>
#include <memory>
#include <limits>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <thread>
using u8=uint8_t;using u64=uint64_t;template<class T>using span=std::span<T>;
struct exception:std::runtime_error{template<class...T>exception(const char*s,T...):std::runtime_error(s){}};
#define LOGW(...) ((void)0)
#define LOGI(...) ((void)0)
namespace vk{struct PipelineCacheCreateInfo{size_t initialDataSize{};const void*pInitialData{};};namespace raii{struct PipelineCache{std::vector<u8>data;PipelineCache(int,PipelineCacheCreateInfo i){if(i.initialDataSize){auto p=static_cast<const u8*>(i.pInitialData);if(*p==0xee)throw std::runtime_error("driver rejects");data.assign(p,p+i.initialDataSize);}}};}}
struct GPU{int traits{},vkDevice{};};struct PipelineCacheFileNameHeader{PipelineCacheFileNameHeader(int){}std::string HexDump(){return "test-cache";}};
struct PipelineCacheFileDataHeader{u64 size{},hash{};u8 data[];};
u64 XXH64(const void*p,size_t n,u64){u64 v=0;auto b=static_cast<const u8*>(p);while(n--)v=v*131+*b++;return v;}
int failWrite{},interruptWrite{},failRename{},failSync{};
ssize_t testWrite(int fd,const void*p,size_t n){if(failWrite){errno=ENOSPC;return -1;}if(interruptWrite){--interruptWrite;errno=EINTR;return -1;}return ::write(fd,p,std::min(n,size_t(3)));}
int testSync(int fd){if(failSync){errno=EIO;return -1;}return ::fsync(fd);}
int testRename(const char*a,const char*b){if(failRename){errno=EACCES;return -1;}return std::rename(a,b);}
#define write testWrite
#define fsync testSync
'''
body=s[a:b].replace('std::rename(', 'testRename(')
main=r'''
#undef write
#undef fsync
int main(int argc,char**argv){assert(argc==2);std::filesystem::path dir=argv[1];std::filesystem::create_directories(dir);auto path=dir/"test-cache";GPU gpu;
 auto load=[&]{return DeserialisePipelineCache(gpu,dir.string()).data;};
 auto set=[&](u64 n,u64 hash,std::vector<u8>bytes,size_t head=16){std::ofstream f(path,std::ios::binary|std::ios::trunc);PipelineCacheFileDataHeader h{n,hash};f.write(reinterpret_cast<char*>(&h),head);f.write(reinterpret_cast<char*>(bytes.data()),bytes.size());};
 std::filesystem::remove(path);assert(load().empty());
 set(UINT64_MAX,0,{});assert(load().empty());set(8,0,{},8);assert(load().empty());set(3,0,{1,2});assert(load().empty());set(1,0,{1,2});assert(load().empty());set(2,999,{1,2});assert(load().empty());set(0,0,{});assert(load().empty());
 set(MaxPipelineCacheBytes+1,0,{});std::filesystem::resize_file(path,16+MaxPipelineCacheBytes+1);assert(load().empty());
 std::vector<u8>old{1,2,3},next{4,5,6,7},reject{0xee};
 SerialisePipelineCache(gpu,dir.string(),old);assert(load()==old);
 for(int* failure:{&failWrite,&failRename,&failSync}){*failure=1;SerialisePipelineCache(gpu,dir.string(),next);*failure=0;assert(load()==old);}
 interruptWrite=1;SerialisePipelineCache(gpu,dir.string(),next);assert(load()==next);
 SerialisePipelineCache(gpu,dir.string(),reject);assert(load().empty());
 std::vector<std::thread>threads;for(int i=0;i<8;i++)threads.emplace_back([&]{SerialisePipelineCache(gpu,dir.string(),next);});for(auto&t:threads)t.join();assert(load()==next);
 for(auto &entry:std::filesystem::directory_iterator(dir))assert(entry.path().filename()=="test-cache");
 std::cout<<"Cache: invalid header/length/hash, 128 MiB cap, driver rejection, partial writes/EINTR, write/fsync/rename failures preserve old cache, 8 concurrent atomic saves OK\n";
}
'''
(out/'cache.cpp').write_text(pre+body+main)
p=subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-fsanitize=address,undefined','-no-pie','-pthread',str(out/'cache.cpp'),'-o',str(out/'cache')],capture_output=True,text=True,env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"));(out/'cache_compile.log').write_text(p.stdout+p.stderr);assert p.returncode==0,p.stderr
p=subprocess.run([str(out/'cache'),str(out/'cache_fixture')],capture_output=True,text=True,env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"));(out/'cache.log').write_text(p.stdout+p.stderr);print(p.stdout+p.stderr);assert p.returncode==0
