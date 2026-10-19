// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <array>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string_view>

namespace skyline::diagnostics {
// No guest pointers retained or dereferenced. Reset only after producers stop.
class ThreadActivities {
public:
    using Word = std::uint64_t;
    struct Call {
        Word id{}, started{}, resource{}, address{}, value{};
        std::array<Word,4> extra{};
        char kind[32]{}, label[64]{};
    };
    struct State {
        Word tid{}, guest{}, handle{}, changed{}, completed{}, lastResult{};
        bool isGuest{}, exited{};
        std::array<Call, 8> calls{};
        unsigned depth{};
        Call last{};
    };
    struct Slot {
        std::atomic<Word> tid{}, lost{};
        std::mutex mutex;
        State state;
    };
    std::array<Slot, 256> slots;
    std::atomic<Word> overflow{};

    template<size_t N> static void Copy(char (&out)[N], std::string_view value) noexcept {
        const auto n=std::min(value.size(),N-1);
        if(n) std::memcpy(out,value.data(),n);
        out[n]=0;
    }
    Slot *Find(Word tid) noexcept {
        if(!tid) return nullptr;
        // Never recycle slots during a session: old records remain explicit.
        for(auto &slot:slots) if(slot.tid.load(std::memory_order_acquire)==tid) return &slot;
        for(auto &slot:slots) {
            Word empty{};
            if(slot.tid.compare_exchange_strong(empty,tid,std::memory_order_acq_rel)) return &slot;
        }
        ++overflow; return nullptr;
    }
    template<class F> void Update(Word tid,F &&fn) noexcept {
        try {
            auto *slot=Find(tid); if(!slot)return;
            std::unique_lock lock(slot->mutex,std::try_to_lock);
            if(!lock){++slot->lost;return;}
            slot->state.tid=tid; fn(slot->state,*slot);
        } catch (...) {++overflow;}
    }
    void Guest(Word tid,Word guest,Word handle,Word now,bool exit) noexcept {
        Update(tid,[&](State &s,Slot &){
            if(!exit) s={}; // Handles reuse of a host TID by a new guest thread.
            s.tid=tid;s.guest=guest;s.handle=handle;s.isGuest=true;s.exited=exit;s.changed=now;
            if(exit)s.depth=0; // longjmp may bypass nested scope destructors.
        });
    }
    void Begin(Word tid,Word id,Word now,std::string_view kind,Word resource,
               Word address,Word value,std::string_view label) noexcept {
        Update(tid,[&](State &s,Slot &slot){
            if(s.depth==s.calls.size()){++slot.lost;return;}
            auto &c=s.calls[s.depth++]; c={id,now,resource,address,value};
            Copy(c.kind,kind);Copy(c.label,label);s.changed=now;
        });
    }
    void Arguments(Word tid,Word id,Word x2,Word x3,Word x4,Word x5) noexcept {
        Update(tid,[&](State &s,Slot &slot){
            for(unsigned i=s.depth;i>0;--i)if(s.calls[i-1].id==id){s.calls[i-1].extra={x2,x3,x4,x5};return;}
            ++slot.lost;
        });
    }
    void End(Word tid,Word id,Word now,Word result) noexcept {
        Update(tid,[&](State &s,Slot &slot){
            for(unsigned i=s.depth;i>0;--i) if(s.calls[i-1].id==id){
                s.last=s.calls[i-1];s.lastResult=result;++s.completed;s.changed=now;
                if(i!=s.depth)++slot.lost; // Unwinding or missing end: not a proven wait.
                s.depth=i-1;return;
            }
            ++slot.lost;
        });
    }
    void Reset() noexcept {
        overflow=0;for(auto &s:slots){s.state={};s.lost=0;s.tid=0;}
    }
};
}
