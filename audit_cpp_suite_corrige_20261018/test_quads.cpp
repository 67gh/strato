
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using u8=uint8_t; using u16=uint16_t; using u32=uint32_t; using u64=uint64_t;
namespace fmt { template<class... A> std::string format(const char* s,A&&...){return s;} template<class T> T ptr(T x){return x;} }
#define LOGE(...) ((void)0)
#define LOGW(...) ((void)0)
#define TRACE_EVENT(...) ((void)0)
#define TRACE_EVENT_BEGIN(...) ((void)0)
#define TRACE_EVENT_END(...) ((void)0)
namespace skyline {
using SpinLock=std::mutex;
using SharedSpinLock=std::shared_mutex;
template<class T> using span=std::vector<T>;
struct exception:std::runtime_error { template<class... A> exception(const char* s,A&&...):std::runtime_error(s){} };
namespace diagnostics {
using Id=u64; inline std::atomic<u64> completions{}, events{};
inline Id NewId(){ static std::atomic<Id> id{};return ++id;}
inline void Completed(Id){++completions;}
inline void Submitted(Id){}
template<class... A> void Event(A&&...){++events;}
}
}
#include <span>
namespace vk {using DeviceSize=u64;enum class IndexType {eUint32,eUint16,eUint8EXT}; }
// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)




namespace skyline::gpu::interconnect::conversion::quads {
    constexpr u32 EmittedIndexCount{6}; //!< The number of indices needed to draw a quad with two triangles
    constexpr u32 QuadVertexCount{4}; //!< The amount of vertices a quad is composed of

    /**
     * @return The amount of indices emitted converting a buffer with the supplied element count
     */
    constexpr u64 GetIndexCount(u32 count) {
        // Incomplete primitives emit nothing. Widen before multiplying.
        return static_cast<u64>(count / QuadVertexCount) * EmittedIndexCount;
    }

    /**
     * @return The minimum size (in bytes) required to store the quad index buffer of the given type after conversion
     * @param indexSize The size of an element in the index buffer in bytes
     */
    constexpr size_t GetRequiredBufferSize(u32 count, size_t indexSize) {
        auto indices{GetIndexCount(count)};
        if (indexSize && indices > std::numeric_limits<size_t>::max() / indexSize)
            throw std::length_error("Quad conversion buffer size overflow");
        return static_cast<size_t>(indices) * indexSize;
    }

    /**
     * @brief Create an index buffer that repeats quad vertices to generate a triangle list
     * @note The size of the supplied buffer should be at least the size returned by GetRequiredBufferSize()
     */
    void GenerateQuadListConversionBuffer(u32 *dest, u32 vertexCount, u32 firstVertex = 0);

    /**
     * @brief Create an index buffer that repeats quad vertices from the source buffer to generate a triangle list
     * @note The size of the destination buffer should be at least the size returned by GetRequiredBufferSize()
     */
    void GenerateIndexedQuadConversionBuffer(u8 *dest, u8 *source, u32 indexCount, vk::IndexType type);
}
// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)


namespace skyline::gpu::interconnect::conversion::quads {
    void GenerateQuadListConversionBuffer(u32 *dest, u32 vertexCount, u32 firstVertex) {
        const u32 completeCount{vertexCount - vertexCount % QuadVertexCount};
        if (completeCount && firstVertex > std::numeric_limits<u32>::max() - (completeCount - 1))
            throw std::out_of_range("Quad vertex range overflow");
        #pragma clang loop vectorize(enable) interleave(enable) unroll(enable)
        for (u32 offset{}; offset < completeCount; offset += QuadVertexCount) {
            const u32 i{firstVertex + offset};
            // Given a quad ABCD, we want to generate triangles ABC & CDA
            // Triangle ABC
            *(dest++) = i + 0;
            *(dest++) = i + 1;
            *(dest++) = i + 2;

            // Triangle CDA
            *(dest++) = i + 2;
            *(dest++) = i + 3;
            *(dest++) = i + 0;
        }
    }

    template<typename S>
    static void GenerateQuadIndexConversionBufferImpl(S *__restrict__ dest, S *__restrict__ source, u32 indexCount) {
        const u32 completeCount{indexCount - indexCount % QuadVertexCount};
        #pragma clang loop vectorize(enable) interleave(enable) unroll(enable)
        for (size_t i{}; i < completeCount; i += QuadVertexCount, source += QuadVertexCount) {
            // Given a quad ABCD, we want to generate triangles ABC & CDA
            // Triangle ABC
            *(dest++) = *(source + 0);
            *(dest++) = *(source + 1);
            *(dest++) = *(source + 2);

            // Triangle CDA
            *(dest++) = *(source + 2);
            *(dest++) = *(source + 3);
            *(dest++) = *(source + 0);
        }
    }

    void GenerateIndexedQuadConversionBuffer(u8 *dest, u8 *source, u32 indexCount, vk::IndexType type) {
        switch (type) {
            case vk::IndexType::eUint32:
                GenerateQuadIndexConversionBufferImpl(reinterpret_cast<u32 *>(dest), reinterpret_cast<u32 *>(source), indexCount);
                break;
            case vk::IndexType::eUint16:
                GenerateQuadIndexConversionBufferImpl(reinterpret_cast<u16 *>(dest), reinterpret_cast<u16 *>(source), indexCount);
                break;
            case vk::IndexType::eUint8EXT:
                GenerateQuadIndexConversionBufferImpl(dest, source, indexCount);
                break;
            default:
                throw std::invalid_argument("Unsupported quad index type");
        }
    }
}
using namespace skyline::gpu::interconnect::conversion::quads;
template<class T> void indexed(vk::IndexType type){
 for(u32 n=0;n<=19;++n){
  std::vector<T> src(n),out(GetIndexCount(n));
  for(u32 i=0;i<n;++i)src[i]=static_cast<T>(i+11);
  GenerateIndexedQuadConversionBuffer(reinterpret_cast<u8*>(out.data()),reinterpret_cast<u8*>(src.data()),n,type);
  const u32 pattern[]{0,1,2,2,3,0};
  for(size_t i=0;i<out.size();++i)assert(out[i]==(i/6)*4+pattern[i%6]+11);
 }
}

namespace skyline::gpu::interconnect::maxwell3d {
constexpr size_t PAGE_SIZE=4096;
namespace util{inline u64 AlignUp(u64 n,u64 a){return (n+a-1)/a*a;}}
namespace memory{struct Buffer{std::vector<u32> storage;int vkBuffer{1};Buffer(size_t n):storage(n/4){};template<class T>std::span<T> cast(){return {reinterpret_cast<T*>(storage.data()),storage.size()*4/sizeof(T)};}};}
struct FakeMemory{memory::Buffer AllocateBuffer(u64 bytes){return memory::Buffer(bytes);}};
struct CacheContext{struct{FakeMemory memory;}gpu;struct{std::vector<std::shared_ptr<void>> deps;void AttachDependency(std::shared_ptr<void> p){deps.push_back(std::move(p));}}executor;};
struct Maxwell3D{CacheContext ctx;std::shared_ptr<memory::Buffer> quadConversionBuffer;bool quadConversionBufferAttached{};u32 quadConversionFirstVertex{},quadConversionVertexCount{};vk::DeviceSize UpdateQuadConversionBuffer(u32,u32);};
vk::DeviceSize Maxwell3D::UpdateQuadConversionBuffer(u32 count, u32 firstVertex) {
        vk::DeviceSize size{conversion::quads::GetRequiredBufferSize(count, sizeof(u32))};

        if (!quadConversionBuffer || quadConversionVertexCount < count || quadConversionFirstVertex != firstVertex) {
            quadConversionBuffer = std::make_shared<memory::Buffer>(ctx.gpu.memory.AllocateBuffer(util::AlignUp(size, PAGE_SIZE)));
            conversion::quads::GenerateQuadListConversionBuffer(quadConversionBuffer->cast<u32>().data(), count, firstVertex);
            quadConversionFirstVertex = firstVertex;
            quadConversionVertexCount = count;
            quadConversionBufferAttached = false;
        }

        if (!quadConversionBufferAttached) {
            ctx.executor.AttachDependency(quadConversionBuffer);
            quadConversionBufferAttached = true;
        }

        return 0;
    }
}
int main(){
 {skyline::gpu::interconnect::maxwell3d::Maxwell3D renderer;
  renderer.UpdateQuadConversionBuffer(4,8);auto old=renderer.quadConversionBuffer;
  renderer.UpdateQuadConversionBuffer(8,8);auto grown=renderer.quadConversionBuffer;
  assert(old!=grown&&grown->storage[10]==15);
  renderer.UpdateQuadConversionBuffer(4,3);assert(renderer.quadConversionBuffer->storage[0]==3);
  assert(old->storage[0]==8); // prior queued allocation stays immutable
 }

 for(u32 n=0;n<=19;++n){
  assert(GetIndexCount(n)==u64{n/4}*6);
  std::vector<u32> out(GetIndexCount(n));
  GenerateQuadListConversionBuffer(out.data(),n,8);
  const u32 pattern[]{0,1,2,2,3,0};
  for(size_t i=0;i<out.size();++i)assert(out[i]==(i/6)*4+pattern[i%6]+8);
 }
 indexed<u8>(vk::IndexType::eUint8EXT);indexed<u16>(vk::IndexType::eUint16);indexed<u32>(vk::IndexType::eUint32);
 assert(GetIndexCount(715827884u)==1073741826ULL);
 assert(GetIndexCount(UINT32_MAX)==6442450938ULL);
 assert(GetRequiredBufferSize(UINT32_MAX,4)==25769803752ULL);
 bool overflow=false;try{GetRequiredBufferSize(8,SIZE_MAX);}catch(const std::length_error&){overflow=true;}assert(overflow);
 u32 out[6];GenerateQuadListConversionBuffer(out,4,UINT32_MAX-3);assert(out[4]==UINT32_MAX);
 overflow=false;try{GenerateQuadListConversionBuffer(out,4,UINT32_MAX-2);}catch(const std::out_of_range&){overflow=true;}assert(overflow);
 bool rejected=false;try{GenerateIndexedQuadConversionBuffer(nullptr,nullptr,0,static_cast<vk::IndexType>(9));}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
 std::cout<<"PASS quads: counts 0..19, uint8/16/32, first=8, overflow, invalid type\n";
}
