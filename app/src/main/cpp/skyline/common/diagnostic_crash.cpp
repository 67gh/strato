// SPDX-License-Identifier: MPL-2.0
#include "diagnostic_crash.h"
#include "diagnostic_recorder.h"
#include "diagnostic_session.h"
#include <atomic>
#include <array>
#include <cstring>
#include <link.h>
#include <sstream>
#include <sys/syscall.h>
#include <time.h>

namespace skyline::diagnostics {
namespace {
std::atomic<int> crashFd{-1};
std::atomic_flag captured = ATOMIC_FLAG_INIT;
static_assert(std::atomic<int>::is_always_lock_free);
struct Record { std::uint64_t magic, version, signal, tid, ns, fault, pc, lr, sp, fp, guest, reserved; };
static_assert(sizeof(Record) == 96);
int Module(dl_phdr_info *info, size_t, void *opaque) noexcept {
    try {
        auto &out = *static_cast<std::ostringstream *>(opaque);
        std::string buildId;
        std::uint64_t start = UINT64_MAX, end = 0;
        for (size_t i=0;i<info->dlpi_phnum;i++) {
            const auto &h=info->dlpi_phdr[i];
            if(h.p_type==PT_LOAD) {start=std::min(start,static_cast<std::uint64_t>(info->dlpi_addr+h.p_vaddr));end=std::max(end,static_cast<std::uint64_t>(info->dlpi_addr+h.p_vaddr+h.p_memsz));}
            if(h.p_type!=PT_NOTE || h.p_memsz>1024*1024) continue;
            bool readable=false;
            for(size_t j=0;j<info->dlpi_phnum;j++) {
                const auto &l=info->dlpi_phdr[j];
                if(l.p_type==PT_LOAD && (l.p_flags&PF_R) && h.p_vaddr>=l.p_vaddr && h.p_vaddr-l.p_vaddr<=l.p_memsz && h.p_memsz<=l.p_memsz-(h.p_vaddr-l.p_vaddr)) readable=true;
            }
            if(!readable)continue;
            auto bytes=reinterpret_cast<const unsigned char *>(info->dlpi_addr+h.p_vaddr);
            size_t offset=0;
            while(h.p_memsz-offset>=sizeof(ElfW(Nhdr))) {
                ElfW(Nhdr) n{};std::memcpy(&n,bytes+offset,sizeof(n));offset+=sizeof(n);
                size_t names=(static_cast<size_t>(n.n_namesz)+3)&~size_t(3), desc=(static_cast<size_t>(n.n_descsz)+3)&~size_t(3);
                if(names>h.p_memsz-offset || desc>h.p_memsz-offset-names)break;
                if(n.n_type==NT_GNU_BUILD_ID && n.n_namesz==4 && std::memcmp(bytes+offset,"GNU",4)==0 && n.n_descsz<=64) {
                    static constexpr char hex[]="0123456789abcdef";
                    for(size_t k=0;k<n.n_descsz;k++){auto b=bytes[offset+names+k];buildId+=hex[b>>4];buildId+=hex[b&15];}
                }
                offset+=names+desc;
            }
        }
        if(start==UINT64_MAX)return 0;
        if(out.tellp()>0)out<<',';
        out<<"{\"path\":\""<<JsonEscape(info->dlpi_name ? info->dlpi_name : "")<<"\",\"base\":"<<info->dlpi_addr
           <<",\"start\":"<<start<<",\"end\":"<<end<<",\"build_id\":\""<<buildId<<"\"}";
    } catch (...) { return 1; }
    return 0;
}
}
void SnapshotModules() noexcept {
    try {std::ostringstream modules;int result=dl_iterate_phdr(Module,&modules);
        if (result != 0) Event("native_modules_incomplete");
        if (!WriteJson("native_modules.json","{\"schema_version\":1,\"complete\":"+std::string(result==0?"true":"false")+",\"modules\":["+modules.str()+"]}"))
            Event("native_modules_write_failed");
    } catch (...) { Event("native_modules_capture_failed"); }
}
void InitializeCrashCapture() noexcept {
    CloseCrashCapture();captured.clear();
    auto path=OutputPath("native_crash.bin",{});
    if(!path.empty())crashFd.store(open(path.c_str(),O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0600));
    Feature("native_crash",crashFd.load()>=0?"armed; first unhandled host/guest record; not a full Android tombstone":"unavailable; crash file could not be opened");
    SnapshotModules();
}
void CloseCrashCapture() noexcept {int fd=crashFd.exchange(-1);if(fd>=0)close(fd);}
void RecordRawCrash(int signal,std::uint64_t fault,std::uint64_t pc,std::uint64_t lr,std::uint64_t sp,std::uint64_t fp,bool guest) noexcept {
    int fd=crashFd.load(std::memory_order_relaxed);
    if(fd<0 || captured.test_and_set(std::memory_order_relaxed))return;
    int savedErrno=errno;timespec ts{};clock_gettime(CLOCK_MONOTONIC,&ts);
    Record record{0x3152434f54415253ULL,1,static_cast<std::uint64_t>(signal),static_cast<std::uint64_t>(syscall(SYS_gettid)),
        static_cast<std::uint64_t>(ts.tv_sec)*1000000000ULL+static_cast<std::uint64_t>(ts.tv_nsec),fault,pc,lr,sp,fp,guest,0};
    const auto *bytes=reinterpret_cast<const char *>(&record);size_t done=0;
    while(done<sizeof(record)) {auto n=write(fd,bytes+done,sizeof(record)-done);if(n<0&&errno==EINTR)continue;if(n<=0)break;done+=static_cast<size_t>(n);}
    errno=savedErrno;
}
}
