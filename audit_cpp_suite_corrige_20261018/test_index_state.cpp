
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
namespace vk {using DeviceSize=u64;using PipelineStageFlags=u32;enum class PipelineStageFlagBits:u32{eVertexInput=1};enum class IndexType{eUint32,eUint16,eUint8EXT};}
namespace skyline::gpu::interconnect {
namespace engine {struct IndexBuffer{enum class IndexSize{OneByte,TwoBytes,FourBytes};IndexSize indexSize{IndexSize::FourBytes};u64 address{0x1000},limit{0x10ff};};}
struct DirtyManager{template<class... A>void Bind(A&&...){};};
namespace dirty {using Handle=int;struct RefreshableManualDirty{};struct CachedManualDirty{};template<class T>struct BoundSubresource{const T& value;BoundSubresource(DirtyManager&,int,const T& v):value(v){};const T* operator->()const{return &value;}};}
struct BufferBinding {int buffer{};size_t offset{},size{};explicit operator bool()const{return buffer!=0;}bool operator==(const BufferBinding&)const=default;};
struct FakeBuffer {bool dirty{};void PopulateReadBarrier(vk::PipelineStageFlagBits,u32&,u32&){};bool IsCurrentExecutionGpuDirty(){return dirty;}};
struct BufferView {
 FakeBuffer* buffer{};u8* data{};u64 size{};
 explicit operator bool()const{return buffer!=nullptr;}
 FakeBuffer* GetBuffer(){return buffer;}
 std::span<u8> GetReadOnlyBackingSpan(bool,const std::function<void()>& callback){if(buffer->dirty)callback();return {data,static_cast<size_t>(size)};}
 void Read(bool,const std::function<void()>& callback,std::vector<u8>& dest,size_t offset){if(buffer->dirty)callback();std::copy_n(data+offset,dest.size(),dest.begin());}
 template<class... A>BufferBinding TryMegaBuffer(A&&...){return {};}
};
struct FakeMega {std::vector<u8> bytes;struct Allocation{int buffer{1};size_t offset{};std::span<u8> region;};Allocation Allocate(int,u64 size){bytes.resize(size);return {1,0,{bytes}};}};
struct FakeExecutor {int cycle{},executionTag{};void AttachBuffer(BufferView&){};};
struct FakeGpu {FakeMega megaBufferAllocator;};
struct InterconnectContext {FakeExecutor executor;FakeGpu gpu;FakeBuffer buffer;std::vector<u8> mapping;u64 requested{};};
struct CachedMappedBufferView {
 BufferView view;
 void Update(InterconnectContext& ctx,u64,u64 size,bool){ctx.requested=size;view={size&&ctx.mapping.size()?&ctx.buffer:nullptr,ctx.mapping.data(),std::min<u64>(size,ctx.mapping.size())};}
 void PurgeCaches(){view={};}BufferView& operator*(){return view;}BufferView* operator->(){return &view;}
};
struct StateUpdateBuilder {unsigned binds{};void SetIndexBuffer(const BufferBinding&,vk::IndexType){++binds;}void SetIndexBuffer(const BufferView&,vk::IndexType){++binds;}};
struct BufferReadNeedsSubmission {};
struct ContextLock{template<class... A>ContextLock(A&&...){};bool IsFirstUsage(){return false;}};
struct CommandExecutor{int tag{};};
struct ConstantBuffer{BufferView view;void Read(CommandExecutor&,span<u8>,size_t);};
}
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
namespace skyline::gpu::interconnect::maxwell3d {
class IndexBufferState : dirty::RefreshableManualDirty, dirty::CachedManualDirty {
      public:
        struct EngineRegisters {
            const engine::IndexBuffer &indexBuffer;

            void DirtyBind(DirtyManager &manager, dirty::Handle handle) const;
        };

      private:
        dirty::BoundSubresource<EngineRegisters> engine;
        CachedMappedBufferView view{};
        BufferBinding megaBufferBinding{};
        vk::IndexType indexType{};
        bool didEstimateSize{};
        u32 usedElementCount{};
        u32 usedFirstIndex{};
        bool usedQuadConversion{};
        bool valid{};

      public:
        IndexBufferState(dirty::Handle dirtyHandle, DirtyManager &manager, const EngineRegisters &engine);

        bool IsValid() const { return valid; }

        void Flush(InterconnectContext &ctx, StateUpdateBuilder &builder, vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask, bool quadConversion, bool estimateSize, u32 firstIndex, u32 elementCount);

        bool Refresh(InterconnectContext &ctx, StateUpdateBuilder &builder, vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask, bool quadConversion, bool estimateSize, u32 firstIndex, u32 elementCount);

        void PurgeCaches();
    };
    static vk::DeviceSize GetIndexBufferSize(engine::IndexBuffer::IndexSize indexSize, u64 elementCount) {
        switch (indexSize) {
            case engine::IndexBuffer::IndexSize::OneByte:
                return elementCount * 1;
            case engine::IndexBuffer::IndexSize::TwoBytes:
                return elementCount * 2;
            case engine::IndexBuffer::IndexSize::FourBytes:
                return elementCount * 4;
            default:
                throw exception("Unsupported index size enum value: {}", static_cast<u32>(indexSize));
        }
    }

    static vk::IndexType ConvertIndexType(engine::IndexBuffer::IndexSize indexSize) {
        switch (indexSize) {
            case engine::IndexBuffer::IndexSize::OneByte:
                return vk::IndexType::eUint8EXT;
            case engine::IndexBuffer::IndexSize::TwoBytes:
                return vk::IndexType::eUint16;
            case engine::IndexBuffer::IndexSize::FourBytes:
                return vk::IndexType::eUint32;
            default:
                throw exception("Unsupported index size enum value: {}", static_cast<u32>(indexSize));
        }
    }

    static BufferBinding GenerateQuadConversionIndexBuffer(InterconnectContext &ctx, engine::IndexBuffer::IndexSize indexType, BufferView &view, u32 firstIndex, u32 elementCount) {
        const auto offset{GetIndexBufferSize(indexType, firstIndex)};
        const auto bytes{GetIndexBufferSize(indexType, elementCount - elementCount % conversion::quads::QuadVertexCount)};
        if (!view || offset > view.size || bytes > view.size - offset)
            throw exception("Quad index source outside mapped buffer: offset {}, bytes {}, available {}", offset, bytes, view.size);
        auto viewSpan{view.GetReadOnlyBackingSpan(false /* We attach above so always false */, [&view]() {
            if (view.GetBuffer()->IsCurrentExecutionGpuDirty())
                throw BufferReadNeedsSubmission{};
        })};

        size_t indexSize{1U << static_cast<u32>(indexType)};
        vk::DeviceSize indexBufferSize{conversion::quads::GetRequiredBufferSize(elementCount, indexSize)};
        auto quadConversionAllocation{ctx.gpu.megaBufferAllocator.Allocate(ctx.executor.cycle, indexBufferSize)};

        conversion::quads::GenerateIndexedQuadConversionBuffer(quadConversionAllocation.region.data(), viewSpan.subspan(offset, bytes).data(), elementCount, ConvertIndexType(indexType));

        return {quadConversionAllocation.buffer, quadConversionAllocation.offset, indexBufferSize};
    }

    /* Index Buffer */
    void IndexBufferState::EngineRegisters::DirtyBind(DirtyManager &manager, dirty::Handle handle) const {
        manager.Bind(handle, indexBuffer.indexSize, indexBuffer.address, indexBuffer.limit);
    }

    IndexBufferState::IndexBufferState(dirty::Handle dirtyHandle, DirtyManager &manager, const EngineRegisters &engine) : engine{manager, dirtyHandle, engine} {}

    void IndexBufferState::Flush(InterconnectContext &ctx, StateUpdateBuilder &builder, vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask, bool quadConversion, bool estimateSize, u32 firstIndex, u32 elementCount) {
        valid = false;
        megaBufferBinding = {};
        didEstimateSize = estimateSize;
        usedElementCount = elementCount;
        usedFirstIndex = firstIndex;
        usedQuadConversion = quadConversion;

        u64 size{[&] () -> u64 {
            if (estimateSize) {
                if (engine->indexBuffer.limit < engine->indexBuffer.address)
                    return 0;
                auto distance{engine->indexBuffer.limit - engine->indexBuffer.address};
                return distance == std::numeric_limits<u64>::max() ? 0 : distance + 1;
            }
            else
                return GetIndexBufferSize(engine->indexBuffer.indexSize, static_cast<u64>(firstIndex) + elementCount);
        }()};

        view.Update(ctx, engine->indexBuffer.address, size, !estimateSize);
        if (!size || !*view || view->size < size || (quadConversion && !conversion::quads::GetIndexCount(elementCount))) {
            LOGW("Invalid index buffer at 0x{:X}: requested {}, mapped {}", engine->indexBuffer.address, size, view->size);
            skyline::diagnostics::Event("index_buffer_invalid", engine->indexBuffer.address, size, view->size);
            return;
        }

        ctx.executor.AttachBuffer(*view);
        view->GetBuffer()->PopulateReadBarrier(vk::PipelineStageFlagBits::eVertexInput, srcStageMask, dstStageMask);

        indexType = ConvertIndexType(engine->indexBuffer.indexSize);

        if (quadConversion)
            megaBufferBinding = GenerateQuadConversionIndexBuffer(ctx, engine->indexBuffer.indexSize, *view, firstIndex, elementCount);
        else
            megaBufferBinding = view->TryMegaBuffer(ctx.executor.cycle, ctx.gpu.megaBufferAllocator, ctx.executor.executionTag);

        if (megaBufferBinding)
            builder.SetIndexBuffer(megaBufferBinding, indexType);
        else
            builder.SetIndexBuffer(*view, indexType);
        valid = true;
    }

    bool IndexBufferState::Refresh(InterconnectContext &ctx, StateUpdateBuilder &builder, vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask, bool quadConversion, bool estimateSize, u32 firstIndex, u32 elementCount) {
        if (!valid || !*view)
            return true;
        if (*view)
            view->GetBuffer()->PopulateReadBarrier(vk::PipelineStageFlagBits::eVertexInput, srcStageMask, dstStageMask);

        if (didEstimateSize != estimateSize || (static_cast<u64>(elementCount) + firstIndex > static_cast<u64>(usedElementCount) + usedFirstIndex) || quadConversion != usedQuadConversion)
            return true;

        // TODO: optimise this to use buffer sequencing to avoid needing to regenerate the quad buffer every time. We can't use as it is rn though because sequences aren't globally unique and may conflict after buffer recreation
        if (usedQuadConversion) {
            megaBufferBinding = GenerateQuadConversionIndexBuffer(ctx, engine->indexBuffer.indexSize, *view, firstIndex, elementCount);
            builder.SetIndexBuffer(megaBufferBinding, indexType);
        } else if (megaBufferBinding) {
            if (auto newMegaBufferBinding{view->TryMegaBuffer(ctx.executor.cycle, ctx.gpu.megaBufferAllocator, ctx.executor.executionTag)};
                newMegaBufferBinding != megaBufferBinding) {

                megaBufferBinding = newMegaBufferBinding;
                if (megaBufferBinding)
                    builder.SetIndexBuffer(megaBufferBinding, indexType);
                else
                    builder.SetIndexBuffer(*view, indexType);
            }
        }

        return false;
    }

    void IndexBufferState::PurgeCaches() {
        valid = false;
        view.PurgeCaches();
        megaBufferBinding = {};
    }


}

namespace skyline::gpu::interconnect {
void ConstantBuffer::Read(CommandExecutor &executor, span<u8> dstBuffer, size_t srcOffset) {
        ContextLock lock{executor.tag, view};
        view.Read(lock.IsFirstUsage(), [this] {
            if (view.GetBuffer()->IsCurrentExecutionGpuDirty())
                throw BufferReadNeedsSubmission{};
            // Writes from an older, already submitted batch can be waited on directly.
        }, dstBuffer, srcOffset);
    }
}

using namespace skyline::gpu::interconnect;using namespace skyline::gpu::interconnect::maxwell3d;
int main(){
 engine::IndexBuffer registers;IndexBufferState::EngineRegisters binding{registers};DirtyManager manager;
 IndexBufferState state(0,manager,binding);InterconnectContext ctx;StateUpdateBuilder builder;u32 src{},dst{};
 ctx.mapping.resize(24);state.Flush(ctx,builder,src,dst,false,false,2,4);assert(state.IsValid()&&builder.binds==1&&ctx.requested==24);
 ctx.mapping.resize(20);state.Flush(ctx,builder,src,dst,false,false,2,4);assert(!state.IsValid()&&builder.binds==1);
 assert(state.Refresh(ctx,builder,src,dst,false,false,2,4));
 ctx.mapping.clear();state.Flush(ctx,builder,src,dst,false,false,2,4);assert(!state.IsValid());
 ctx.mapping.resize(24);state.Flush(ctx,builder,src,dst,true,false,2,4);assert(state.IsValid()&&builder.binds==2);
 state.PurgeCaches();assert(!state.IsValid());
 state.Flush(ctx,builder,src,dst,false,false,UINT32_MAX,4);assert(!state.IsValid()&&ctx.requested==17179869196ULL);
 ctx.mapping.resize(256);state.Flush(ctx,builder,src,dst,false,true,0,0);assert(state.IsValid()&&ctx.requested==256);
 registers.limit=0xfff;state.Flush(ctx,builder,src,dst,false,true,0,0);assert(!state.IsValid()&&ctx.requested==0);
 assert(GetIndexBufferSize(engine::IndexBuffer::IndexSize::FourBytes,1073741824ULL)==4294967296ULL);
 BufferView small{&ctx.buffer,ctx.mapping.data(),4};bool rejected=false;
 try{GenerateQuadConversionIndexBuffer(ctx,engine::IndexBuffer::IndexSize::FourBytes,small,1,4);}catch(const std::exception&){rejected=true;}assert(rejected);
 ctx.buffer.dirty=true;BufferView enough{&ctx.buffer,ctx.mapping.data(),16};bool retry=false;
 try{GenerateQuadConversionIndexBuffer(ctx,engine::IndexBuffer::IndexSize::FourBytes,enough,0,4);}catch(const BufferReadNeedsSubmission&){retry=true;}assert(retry);
 ConstantBuffer cbuf{enough};CommandExecutor executor;std::vector<u8> dest(4);retry=false;
 try{cbuf.Read(executor,dest,0);}catch(const BufferReadNeedsSubmission&){retry=true;}assert(retry);
 ctx.buffer.dirty=false;cbuf.Read(executor,dest,0);
 std::cout<<"PASS index state host shims: exact/partial/missing mapping, retry, purge, large arithmetic, inclusive range, bounds, dirty readback request\n";
}
