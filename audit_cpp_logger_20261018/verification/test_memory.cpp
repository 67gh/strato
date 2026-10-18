#include "interval.h"
#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
namespace vk {using Buffer=unsigned;using Image=unsigned;using DeviceSize=size_t;}
using VmaAllocator=void*;using VmaAllocation=void*;
std::map<unsigned,int> buffers,images,unmaps;
void vmaDestroyBuffer(void*,unsigned id,void*){++buffers[id];}
void vmaDestroyImage(void*,unsigned id,void*){++images[id];}
void vmaUnmapMemory(void*,void* id){++unmaps[static_cast<unsigned>(reinterpret_cast<uintptr_t>(id))];}
namespace skyline::gpu::memory {
    struct Buffer : public span<u8> {
        VmaAllocator vmaAllocator;
        VmaAllocation vmaAllocation;
        vk::Buffer vkBuffer;

        constexpr Buffer(u8 *pointer, size_t size, VmaAllocator vmaAllocator, vk::Buffer vkBuffer, VmaAllocation vmaAllocation)
            : vmaAllocator(vmaAllocator),
              vkBuffer(vkBuffer),
              vmaAllocation(vmaAllocation),
              span<u8>(pointer, size) {}

        Buffer(const Buffer &) = delete;

        constexpr Buffer(Buffer &&other)
            : vmaAllocator(std::exchange(other.vmaAllocator, nullptr)),
              vmaAllocation(std::exchange(other.vmaAllocation, nullptr)),
              vkBuffer(std::exchange(other.vkBuffer, {})),
              span<u8>(other) {}

        Buffer &operator=(const Buffer &) = delete;

        Buffer &operator=(Buffer &&other) noexcept;

        ~Buffer();
    };

    struct Image {
      private:
        u8 *pointer{};

      public:
        VmaAllocator vmaAllocator;
        VmaAllocation vmaAllocation;
        vk::Image vkImage;

        constexpr Image(VmaAllocator vmaAllocator, vk::Image vkImage, VmaAllocation vmaAllocation)
            : vmaAllocator(vmaAllocator),
              vkImage(vkImage),
              vmaAllocation(vmaAllocation) {}

        constexpr Image(u8 *pointer, VmaAllocator vmaAllocator, vk::Image vkImage, VmaAllocation vmaAllocation)
            : pointer(pointer),
              vmaAllocator(vmaAllocator),
              vkImage(vkImage),
              vmaAllocation(vmaAllocation) {}

        Image(const Image &) = delete;

        constexpr Image(Image &&other)
            : pointer(std::exchange(other.pointer, nullptr)),
              vmaAllocator(std::exchange(other.vmaAllocator, nullptr)),
              vmaAllocation(std::exchange(other.vmaAllocation, nullptr)),
              vkImage(std::exchange(other.vkImage, {})) {}

        Image &operator=(const Image &) = delete;

        Image &operator=(Image &&other) noexcept;

        ~Image();

        /**
         * @return A pointer to a mapping of the image on the CPU
         * @note If the image isn't already mapped on the CPU, this creates a mapping for it
         */
        u8 *data();
    };

    Buffer &Buffer::operator=(Buffer &&other) noexcept {
        if (this != &other) {
            Buffer replacement{std::move(other)};
            std::swap(static_cast<span<u8> &>(*this), static_cast<span<u8> &>(replacement));
            std::swap(vmaAllocator, replacement.vmaAllocator);
            std::swap(vmaAllocation, replacement.vmaAllocation);
            std::swap(vkBuffer, replacement.vkBuffer);
        }
        return *this;
    }

    Image &Image::operator=(Image &&other) noexcept {
        if (this != &other) {
            Image replacement{std::move(other)};
            std::swap(pointer, replacement.pointer);
            std::swap(vmaAllocator, replacement.vmaAllocator);
            std::swap(vmaAllocation, replacement.vmaAllocation);
            std::swap(vkImage, replacement.vkImage);
        }
        return *this;
    }

    Buffer::~Buffer() {
        if (vmaAllocator && vmaAllocation && vkBuffer)
            vmaDestroyBuffer(vmaAllocator, vkBuffer, vmaAllocation);
    }

    Image::~Image() {
        if (vmaAllocator && vmaAllocation && vkImage) {
            if (pointer)
                vmaUnmapMemory(vmaAllocator, vmaAllocation);
            vmaDestroyImage(vmaAllocator, vkImage, vmaAllocation);
        }
    }


}
namespace fixture {
using skyline::u8;using skyline::span;
enum class DirtyState{GpuDirty,CpuDirty,Clean};
struct Buffer{
 std::mutex stateMutex;DirtyState dirtyState{DirtyState::Clean};bool blocked{},poll{true},reads{true},isDirect{true},directTrackedShadowActive{true};
 std::vector<u8> mirror=std::vector<u8>(32,1),directTrackedShadow=std::vector<u8>(32,9),storage=std::vector<u8>(32,2);
 std::vector<u8>*backing{&storage};skyline::IntervalList<size_t> directTrackedWrites;
 bool SequencedCpuBackingWritesBlocked(){return blocked;}
 void SynchronizeHost(){dirtyState=DirtyState::Clean;}
 bool PollFence(){return poll;}
 void RefreshGpuWritesActiveDirect(bool,const std::function<void()>&){}
 bool RefreshGpuReadsActiveDirect(){return reads;}
 void SynchronizeGuestImmediate(bool,const std::function<void()>&){}
 bool WriteImplStaged(span<u8>,vk::DeviceSize,const std::function<void()>&);
 span<u8> GetReadOnlyBackingSpan(bool,const std::function<void()>&);
 void ReadImplDirect(const std::function<void()>&,span<u8>,vk::DeviceSize);
};
bool Buffer::WriteImplStaged(span<u8> data, vk::DeviceSize offset, const std::function<void()> &gpuCopyCallback) {
        // We cannot have *ANY* state changes for the duration of this function, if the buffer became CPU dirty partway through the GPU writes would mismatch the CPU writes
        std::scoped_lock lock{stateMutex};

        // If the buffer is GPU dirty do the write on the GPU and we're done
        if (dirtyState == DirtyState::GpuDirty) {
            if (!gpuCopyCallback)
                return true;
            gpuCopyCallback();
            std::memcpy(mirror.data() + offset, data.data(), data.size());
            return false; // Exactly one GPU copy; never write the in-flight backing on CPU.
        }

        if (dirtyState == DirtyState::CpuDirty && SequencedCpuBackingWritesBlocked())
            // If the buffer is used in sequence directly on the GPU, SynchronizeHost before modifying the mirror contents to ensure proper sequencing. This write will then be sequenced on the GPU instead (the buffer will be kept clean for the rest of the execution due to gpuCopyCallback blocking all writes)
            SynchronizeHost();

        std::memcpy(mirror.data() + offset, data.data(), data.size()); // Always copy to mirror since any CPU side reads will need the up-to-date contents

        if (dirtyState == DirtyState::CpuDirty && !SequencedCpuBackingWritesBlocked())
            // Skip updating backing if the changes are gonna be updated later by SynchroniseHost in executor anyway
            return false;

        if (!SequencedCpuBackingWritesBlocked() && PollFence()) {
            // We can write directly to the backing as long as this resource isn't being actively used by a past workload (in the current context or another)
            std::memcpy(backing->data() + offset, data.data(), data.size());
        } else {
            // If this buffer is host immutable, perform a GPU-side inline update for the buffer contents since we can't directly modify the backing
            // If no copy callback is supplied, return true to indicate that the caller should repeat the write with an appropriate callback
            if (gpuCopyCallback)
                gpuCopyCallback();
            else
                return true;
        }

        return false;
    }
span<u8> Buffer::GetReadOnlyBackingSpan(bool isFirstUsage, const std::function<void()> &flushHostCallback) {
        if (!isDirect) {
            std::unique_lock lock{stateMutex};
            if (dirtyState == DirtyState::GpuDirty)
                SynchronizeGuestImmediate(isFirstUsage, flushHostCallback);
        } else {
            RefreshGpuWritesActiveDirect(true, flushHostCallback);
            if (directTrackedShadowActive && RefreshGpuReadsActiveDirect()) {
                // Keep GPU-owned mirror bytes untouched. Complete only the gaps in
                // the CPU shadow, retaining the newer sequenced writes already there.
                size_t offset{};
                while (offset < mirror.size()) {
                    auto part{directTrackedWrites.Query(offset)};
                    auto remaining{mirror.size() - offset};
                    auto size{part.size ? std::min(part.size, remaining) : remaining};
                    if (!part.enclosed)
                        std::memcpy(directTrackedShadow.data() + offset, mirror.data() + offset, size);
                    offset += size;
                }
                return {directTrackedShadow.data(), directTrackedShadow.size()};
            }
        }

        return mirror;
    }
void Buffer::ReadImplDirect(const std::function<void()> &flushHostCallback, span<u8> data, vk::DeviceSize offset) {
        // If GPU writes are active then wait until that's no longer the case
        RefreshGpuWritesActiveDirect(true, flushHostCallback);

        if (directTrackedShadowActive && RefreshGpuReadsActiveDirect()) {
            size_t dstOffset{};
            while (dstOffset != data.size()) {
                auto srcOffset{dstOffset + offset};
                auto dstRemaining{data.size() - dstOffset};
                auto result{directTrackedWrites.Query(srcOffset)};
                auto size{result.size ? std::min(result.size, dstRemaining) : dstRemaining};
                auto srcData{result.enclosed ? directTrackedShadow.data() : mirror.data()};
                std::memcpy(data.data() + dstOffset, srcData + srcOffset, size);
                dstOffset += size;
            }
        } else [[likely]] {
            std::memcpy(data.data(), mirror.data() + offset, data.size());
        }
    }
}
int main(){
 using skyline::gpu::memory::Buffer;using skyline::gpu::memory::Image;
 auto allocator=reinterpret_cast<void*>(1);unsigned char data[32]{};
 {Buffer a(data,8,allocator,1,reinterpret_cast<void*>(1));Buffer b(data+8,8,allocator,2,reinterpret_cast<void*>(2));a=std::move(b);assert(buffers[1]==1&&buffers[2]==0);assert(a.data()==data+8&&!b.vkBuffer);a=std::move(a);assert(buffers[2]==0);}
 assert(buffers[2]==1);
 {Image a(data,allocator,3,reinterpret_cast<void*>(3));Image b(allocator,4,reinterpret_cast<void*>(4));a=std::move(b);assert(images[3]==1&&unmaps[3]==1);a=std::move(a);assert(images[4]==0);}
 assert(images[4]==1&&unmaps[4]==0);
 {Image a(allocator,5,reinterpret_cast<void*>(5));Image b(data,allocator,6,reinterpret_cast<void*>(6));a=std::move(b);assert(images[5]==1);}
 assert(images[6]==1&&unmaps[6]==1);
 fixture::Buffer f;f.directTrackedWrites.Insert({4,8});f.directTrackedWrites.Insert({16,20});auto before=f.mirror;
 auto view=f.GetReadOnlyBackingSpan(false,{});assert(f.mirror==before);for(size_t i=0;i<32;i++)assert(view[i]==((i>=4&&i<8)||(i>=16&&i<20)?9:1));
 std::vector<unsigned char> read(24);f.ReadImplDirect({},read,8);for(size_t i=0;i<24;i++)assert(read[i]==(i>=8&&i<12?9:1));
 f.reads=false;assert(f.GetReadOnlyBackingSpan(false,{}).data()==f.mirror.data());
 std::vector<unsigned char> write(4,7);f.dirtyState=fixture::DirtyState::GpuDirty;auto old=f.mirror;assert(f.WriteImplStaged(write,4,{}));assert(f.mirror==old);int copies=0;
 assert(!f.WriteImplStaged(write,4,[&]{++copies;}));assert(copies==1);for(size_t i=0;i<32;i++){assert(f.storage[i]==2);assert(f.mirror[i]==(i>=4&&i<8?7:1));}
 std::cout<<"PASS extracted production functions: buffer/image move ownership, self move, mapped/unmapped destruction; coherent shadow gaps, half-open read, staged GPU-dirty copy exactly once\n";
}
