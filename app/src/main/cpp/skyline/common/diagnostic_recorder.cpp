// SPDX-License-Identifier: MPL-2.0
#include "diagnostic_recorder.h"
#include "diagnostic_session.h"
#include "thread_activity.h"
#include <thread>
#include <deque>
#include <condition_variable>
#include <chrono>
#include <array>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>
#include <sys/syscall.h>
#include <time.h>

namespace skyline::diagnostics {
static void WriteArtifact(std::string_view kind, Id shader, const void *bytes, std::size_t size, std::string_view extension) noexcept;
namespace {
    struct Histogram { std::array<std::atomic<Id>, 64> buckets{}; std::atomic<Id> samples{}, sum{}; };
    std::array<Histogram, static_cast<unsigned>(Timing::Count)> histograms{};
    std::atomic<Id> lastPresent{};
    struct Artifact { std::string kind, extension; Id shader; std::vector<unsigned char> bytes; };
    std::mutex pendingMutex;
    std::deque<Artifact> pendingArtifacts;
    size_t pendingBytes{};
    constexpr size_t PendingLimit{8 * 1024 * 1024};
    void DrainArtifacts() noexcept;

    constexpr std::size_t Capacity = 8192, IpcCapacity = 2048, MaxMappings = 65536, MaxUnsupported = 512;
    constexpr std::size_t ArtifactBudget = 32 * 1024 * 1024, ArtifactLimit = 4 * 1024 * 1024;
    struct Entry {
        Id sequence{}, ns{}, tid{}, frame{}, operation{}, submission{}, pipeline{}, resource{}, address{}, size{};
        char kind[32]{}, text[192]{};
    };
    ThreadActivities activities;
    struct CpuSample { Id tid{}, ticks{}, ns{}; };
    std::array<CpuSample,256> cpuSamples{};
    constexpr size_t TraceCapacity=16384, TraceBudget=32*1024*1024;
    std::array<Entry,TraceCapacity> traceQueue{};
    size_t traceHead{},traceCount{},traceBytes{};
    std::atomic<Id> traceDrops{},traceWritten{},traceIoErrors{},traceBudgetDrops{},threadSnapshotFailures{},traceStatusFailures{};
    bool traceEnabled{true};
    void DrainTrace() noexcept;
    void SnapshotThreads() noexcept;
    std::array<Entry, Capacity> ring{};
    std::array<Entry, IpcCapacity> ipcRing{}; // IPC traffic cannot evict rendering evidence.
    std::atomic_flag ringLock = ATOMIC_FLAG_INIT;
    std::atomic<Id> dropped{}, total{}, captures{}, artifactDrops{}, mappingDrops{}, registryDrops{};
    void DrainArtifacts() noexcept {
        // Snapshot the bounded queue, so a busy producer cannot starve periodic exports.
        try {
            std::deque<Artifact> batch;
            { std::lock_guard lock(pendingMutex); batch.swap(pendingArtifacts); pendingBytes=0; }
            for (auto &a : batch) WriteArtifact(a.kind,a.shader,a.bytes.data(),a.bytes.size(),a.extension);
        } catch (...) { ++artifactDrops; }
    }
    std::atomic<Id> draws{}, dispatches{}, submitted{}, completed{}, frames{};
    std::atomic<bool> enabled{}, paused{}, frameCapture{};
    int validationMode{};
    bool shaderCapture{true};
    std::size_t head{}, count{}, ipcHead{}, ipcCount{}, artifactBytes{}, artifactCount{};
    std::mutex stateMutex, artifactMutex, flushMutex;
    struct Pump {
        std::thread worker;
        std::mutex mutex;
        std::condition_variable cv;
        bool stop{true};
        void Stop() noexcept {
            {std::lock_guard lock(mutex);stop=true;}cv.notify_all();
            if(worker.joinable())worker.join();
        }
        ~Pump(){Stop();}
        void Start() {
            stop=false;
            worker=std::thread([this] {
                Id lastRequest{};
                auto nextFlush=std::chrono::steady_clock::now();
                auto nextThreads=nextFlush;
                std::unique_lock lock(mutex);
                while(!cv.wait_for(lock,std::chrono::milliseconds(250),[this]{return stop;})) {
                    DrainTrace();
                    DrainArtifacts();
                    const auto request=CaptureRequest();const auto now=std::chrono::steady_clock::now();
                    if(now>=nextThreads){SnapshotThreads();nextThreads=now+std::chrono::seconds(1);}
                    if(request!=lastRequest || now>=nextFlush) {
                        if(FlushRecorder(request!=lastRequest?"manual_evidence":"periodic"))lastRequest=request;
                        nextFlush=now+std::chrono::seconds(10);
                    }
                }
            });
        }
    };
    struct Region { Id end{}, host{}, generation{}; bool sparse{}; };
    std::map<std::pair<Id, Id>, Region> regions;
    struct Missing { Id count{}, first{}, last{}, opcode{}; std::string component, detail; Correlation context; };
    std::map<std::string, Missing> missing;
    std::map<std::string, std::string> features;
    Pump pump; // Destroy before the maps and their mutexes.
    Id Now() noexcept {
        timespec ts{};clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<Id>(ts.tv_sec) * 1000000000ULL + static_cast<Id>(ts.tv_nsec);
    }
    template<std::size_t N> void Copy(char (&dst)[N], std::string_view text) noexcept {
        auto n = std::min(text.size(), N - 1);if(n) std::memcpy(dst, text.data(), n);dst[n] = 0;
    }
    std::string Quote(std::string_view s) { return "\"" + JsonEscape(s) + "\""; }
}
namespace {
    void SerializeEntry(std::ostream &out,const Entry &e) {
        out<<"{\"seq\":"<<e.sequence<<",\"ns\":"<<e.ns<<",\"tid\":"<<e.tid<<",\"frame\":"<<e.frame
           <<",\"operation\":"<<e.operation<<",\"submission\":"<<e.submission<<",\"pipeline\":"<<e.pipeline
           <<",\"kind\":"<<Quote(e.kind)<<",\"resource\":"<<e.resource<<",\"address\":"<<e.address
           <<",\"size\":"<<e.size<<",\"text\":"<<Quote(e.text)<<"}\n";
    }
    void DrainTrace() noexcept {
        try {
            std::vector<Entry> batch(TraceCapacity);
            if(ringLock.test_and_set(std::memory_order_acquire))return;
            const auto n=traceCount;
            for(size_t i=0;i<n;++i)batch[i]=traceQueue[(traceHead+i)%TraceCapacity];
            traceHead=(traceHead+n)%TraceCapacity;traceCount=0;
            ringLock.clear(std::memory_order_release);batch.resize(n);
            std::string text;
            size_t accepted{};
            for(const auto &e:batch){
                std::ostringstream line;SerializeEntry(line,e);auto value=line.str();
                if(value.size()>TraceBudget-traceBytes-text.size()){++traceBudgetDrops;continue;}
                text+=value;++accepted;
            }
            if(!text.empty()) {
                std::ofstream out(OutputPath("session_trace.jsonl",{}),std::ios::app|std::ios::binary);
                out.write(text.data(),static_cast<std::streamsize>(text.size()));out.close();
                // Count attempted bytes too: never exceed budget after a partial disk write.
                traceBytes+=text.size();
                if(out)traceWritten+=accepted;else traceIoErrors+=accepted;
            }
            std::ostringstream status;
            status<<"{\"schema_version\":1,\"enabled\":"<<(traceEnabled?"true":"false")
                  <<",\"queued_capacity\":"<<TraceCapacity<<",\"budget_bytes\":"<<TraceBudget
                  <<",\"attempted_bytes\":"<<traceBytes<<",\"written_events\":"<<traceWritten.load()
                  <<",\"thread_snapshot_failures\":"<<threadSnapshotFailures.load()<<",\"status_write_failures\":"<<traceStatusFailures.load()
                  <<",\"queue_drops\":"<<traceDrops.load()<<",\"producer_contention_drops\":"<<dropped.load()
                  <<",\"budget_drops\":"<<traceBudgetDrops.load()<<",\"io_failed_events\":"<<traceIoErrors.load()
                  <<",\"known_loss\":"<<((traceDrops||dropped||traceBudgetDrops||traceIoErrors)?"true":"false")
                  <<",\"scope\":\"instrumented native dispatch only; not every C++ call or guest instruction; undrained tail may be lost on abrupt process death; 250ms is a nominal drain interval\"}";
            if(!WriteJson("session_trace_status.json",status.str()))++traceStatusFailures;
        } catch (...) {++traceIoErrors;}
    }
    std::string ProcText(Id tid,const char *file) {
        std::ifstream in("/proc/self/task/"+std::to_string(tid)+"/"+file);
        if(!in)return "unavailable";
        std::string value;std::getline(in,value);return value;
    }
    void SnapshotThreads() noexcept {
        try {
            const auto now=Now();Id busy{},lost{};bool comma=false;
            std::ostringstream out;
            out<<"{\"schema_version\":1,\"ns\":"<<now<<",\"paused\":"<<(IsPaused()?"true":"false")
               <<",\"certainty\":\"observed calls and waits only; a long wait alone does not prove deadlock\",\"threads\":[";
            for(auto &slot:activities.slots) {
                if(!slot.tid.load(std::memory_order_acquire))continue;
                ThreadActivities::State s;Id missed{};
                {
                    std::unique_lock lock(slot.mutex,std::try_to_lock);
                    if(!lock){++busy;continue;}
                    s=slot.state;missed=slot.lost.load();
                }
                if(!s.tid)continue;
                lost+=missed;
                const auto stat=ProcText(s.tid,"stat");
                const auto endName=stat.rfind(')');
                char hostState='?';Id userTicks{},systemTicks{};bool cpuValid=false;double cpuPercent{};
                auto &previous=cpuSamples[static_cast<size_t>(&slot-activities.slots.data())];
                if(endName!=std::string::npos) {
                    std::istringstream fields(stat.substr(endName+1));std::string skip;
                    fields>>hostState;
                    for(int field=4;field<=13;++field)fields>>skip;
                    if(fields>>userTicks>>systemTicks){
                        const auto ticks=userTicks+systemTicks;
                        const auto hz=sysconf(_SC_CLK_TCK);
                        if(hz>0 && previous.tid==s.tid && now>previous.ns && ticks>=previous.ticks){
                            cpuPercent=100.0*static_cast<double>(ticks-previous.ticks)*1e9/
                                (static_cast<double>(hz)*static_cast<double>(now-previous.ns));cpuValid=true;
                        }
                        previous={s.tid,ticks,now};
                    }
                }
                if(comma)out<<',';
                comma=true;
                out<<"{\"tid\":"<<s.tid<<",\"guest\":"<<(s.isGuest?"true":"false")
                   <<",\"guest_id\":"<<s.guest<<",\"guest_handle\":"<<s.handle<<",\"exited\":"<<(s.exited?"true":"false")
                   <<",\"name\":"<<Quote(ProcText(s.tid,"comm"))<<",\"host_stat\":"<<Quote(stat)<<",\"host_state\":"<<Quote(std::string(1,hostState))
                   <<",\"cpu_percent_valid\":"<<(cpuValid?"true":"false")<<",\"cpu_percent_one_core\":"<<cpuPercent
                   <<",\"host_wchan\":"<<Quote(ProcText(s.tid,"wchan"))<<",\"lost_updates\":"<<missed
                   <<",\"completed_calls\":"<<s.completed<<",\"last_change_ns\":"<<s.changed
                   <<",\"last_call\":"<<Quote(s.last.kind)<<",\"last_resource\":"<<s.last.resource
                   <<",\"last_result_raw\":"<<s.lastResult<<",\"active\":[";
                for(unsigned i=0;i<s.depth;++i){
                    const auto &c=s.calls[i];if(i)out<<',';
                    const auto age=now>=c.started?now-c.started:0;
                    out<<"{\"id\":"<<c.id<<",\"kind\":"<<Quote(c.kind)<<",\"label\":"<<Quote(c.label)
                       <<",\"resource\":"<<c.resource<<",\"arg0\":"<<c.address<<",\"arg1\":"<<c.value
                       <<",\"arg2\":"<<c.extra[0]<<",\"arg3\":"<<c.extra[1]<<",\"arg4\":"<<c.extra[2]<<",\"arg5\":"<<c.extra[3]
                       <<",\"started_ns\":"<<c.started<<",\"age_ns\":"<<age
                       <<",\"long_running_candidate\":"<<(!IsPaused()&&age>=10000000000ULL?"true":"false")<<'}';
                }
                out<<"]}";
            }
            out<<"],\"busy_slots\":"<<busy<<",\"lost_updates\":"<<lost<<",\"untracked_updates\":"<<activities.overflow.load()<<'}';
            if(!WriteJson("thread_activity.json",out.str()))++threadSnapshotFailures;
        } catch (...) {++threadSnapshotFailures;}
    }
}

Operation::Operation(const char *name, Id resource, Timing timing, std::string_view label) noexcept
    : kind{name}, resource{resource}, id{NewId()}, timing{timing}, start{std::chrono::steady_clock::now()} {
    BeginActivity(id,kind,resource,0,0,label);
    Event(kind,id,resource,0,"begin");
}
Operation::~Operation() { if(!finished) Finish(UINT64_MAX,"failed_or_abandoned"); }
void Operation::Finish(Id result, std::string_view detail) noexcept {
    if(finished)return;
    finished=true;
    EndActivity(id,result);
    const auto ns=static_cast<Id>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count());
    RecordDuration(timing,ns);Event(kind,id,ns,result,detail.empty()?"end":detail);
}
void RecordDuration(Timing kind, Id ns) noexcept {
    if(!RecorderEnabled())return;
    auto index=static_cast<unsigned>(kind);if(index>=histograms.size())return;
    unsigned bucket=0;for(Id v=ns;v>1 && bucket<63;v>>=1)++bucket;
    auto &h=histograms[index];++h.buckets[bucket];h.sum.fetch_add(ns,std::memory_order_relaxed);++h.samples;
}
bool SessionTraceEnabled() noexcept { return RecorderEnabled() && traceEnabled; }
void GuestThread(Id guest,Id handle,bool exited) noexcept {
    if(!RecorderEnabled())return;
    activities.Guest(static_cast<Id>(syscall(SYS_gettid)),guest,handle,Now(),exited);
    Event(exited?"guest_thread_exit":"guest_thread_start",guest,handle);
}
void BeginActivity(Id id,std::string_view kind,Id resource,Id address,Id value,std::string_view label) noexcept {
    if(!RecorderEnabled())return;
    activities.Begin(static_cast<Id>(syscall(SYS_gettid)),id,Now(),kind,resource,address,value,label);
    if(SessionTraceEnabled())Event("native_call_begin",id,resource,0,kind);
}
void ActivityArguments(Id id,Id x2,Id x3,Id x4,Id x5) noexcept {
    if(RecorderEnabled())activities.Arguments(static_cast<Id>(syscall(SYS_gettid)),id,x2,x3,x4,x5);
}
void EndActivity(Id id,Id result) noexcept {
    if(!RecorderEnabled())return;
    activities.End(static_cast<Id>(syscall(SYS_gettid)),id,Now(),result);
    if(SessionTraceEnabled())Event("native_call_end",id,result);
}
bool RecorderEnabled() noexcept { return enabled.load(std::memory_order_relaxed); }
int ValidationMode() noexcept { return validationMode; }
bool ShaderCaptureEnabled() noexcept { return RecorderEnabled() && shaderCapture; }
void Event(std::string_view kind, Id resource, Id address, Id size, std::string_view text) noexcept {
    if (!RecorderEnabled()) return;
    if (ringLock.test_and_set(std::memory_order_acquire)) { ++dropped;return; }
    const bool ipc = kind.starts_with("ipc_") || kind.starts_with("svc_");
    auto &targetHead = ipc ? ipcHead : head;
    auto &targetCount = ipc ? ipcCount : count;
    const auto capacity = ipc ? IpcCapacity : Capacity;
    auto &e = ipc ? ipcRing[targetHead] : ring[targetHead];e = {};
    e.sequence = ++total;e.ns = Now();e.tid = static_cast<Id>(syscall(SYS_gettid));e.frame = frames.load();
    e.operation = correlation.operation;e.submission = correlation.submission;e.pipeline = correlation.pipeline;
    e.resource = resource;e.address = address;e.size = size;Copy(e.kind,kind);Copy(e.text,text);
    if(traceEnabled) {
        if(traceCount<traceQueue.size()) {
            traceQueue[(traceHead+traceCount)%TraceCapacity]=e;++traceCount;
        } else ++traceDrops;
    }
    targetHead = (targetHead + 1) % capacity;targetCount = std::min(targetCount + 1, capacity);
    ringLock.clear(std::memory_order_release);
}
void CountDraw(bool compute) noexcept { if (compute) ++dispatches;else ++draws; }
void Submitted(Id cycle) noexcept { ++submitted;Event("submit_accepted",cycle); }
void Completed(Id cycle) noexcept { ++completed;Event("fence_completed",cycle); }
Progress GetProgress() noexcept { return {draws.load(), dispatches.load(), submitted.load(), completed.load(), frames.load()}; }
void Presented() noexcept {
    ++frames;Event("present_accepted");
    if (RecorderEnabled() && !IsPaused()) { auto now=Now();auto previous=lastPresent.exchange(now);if(previous && now>=previous)RecordDuration(Timing::Frame,now-previous); }
}
void SetPaused(bool value) noexcept { paused = value;lastPresent=0;Event(value ? "paused" : "resumed"); }
bool IsPaused() noexcept { return paused.load(); }
Id RequestCapture() noexcept { auto id = ++captures;Event("capture_requested",id);return id; }
Id CaptureRequest() noexcept { return captures.load(); }
void RequestFrameCapture() noexcept { frameCapture = true;Event("frame_capture_requested"); }
bool ConsumeFrameCapture() noexcept { return frameCapture.exchange(false); }
void Feature(std::string_view name, std::string_view status) noexcept {
    try { std::lock_guard lock(stateMutex);features[std::string(name.substr(0,64))] = status.substr(0,1024); } catch (...) { ++registryDrops; }
}

void Mapping(Id space, Id address, Id size, Id host, bool sparse, bool unmap) noexcept {
    if (!RecorderEnabled() || !size) return;
    const Id generation = NewId();
    Event(unmap ? "unmap" : (sparse ? "map_sparse" : "map"),space,address,size);
    if (size > UINT64_MAX - address) { ++mappingDrops;return; }
    try {
        std::unique_lock lock(stateMutex,std::try_to_lock);
        if (!lock) { ++mappingDrops;return; }
        const Id end = address + size;
        auto it = regions.lower_bound({space,address});
        if (it != regions.begin()) { auto prev = std::prev(it);if (prev->first.first == space && prev->second.end > address) it = prev; }
        std::vector<std::pair<std::pair<Id,Id>,Region>> tails;
        while (it != regions.end() && it->first.first == space && it->first.second < end) {
            auto start = it->first.second;auto r = it->second;
            if (r.end <= address) { ++it;continue; }
            if (start < address) tails.push_back({{space,start},{address,r.host,r.generation,r.sparse}});
            if (r.end > end) tails.push_back({{space,end},{r.end,r.sparse ? r.host : r.host + end - start,r.generation,r.sparse}});
            it = regions.erase(it);
        }
        for (auto &tail : tails) regions.insert(tail);
        if (!unmap && (host || sparse)) regions[{space,address}] = {end,host,generation,sparse};
        if (regions.size() > MaxMappings) { regions.erase(regions.begin());++mappingDrops; }
    } catch (...) { ++mappingDrops; }
}
void DestroyAddressSpace(Id space) noexcept {
    try { std::lock_guard lock(stateMutex);auto it=regions.lower_bound({space,0});while(it!=regions.end()&&it->first.first==space)it=regions.erase(it); } catch (...) { ++mappingDrops; }
    Event("address_space_destroy",space);
}
void Unsupported(std::string_view component, Id opcode, std::string_view detail) noexcept {
    if (!RecorderEnabled()) return;
    try {
        std::unique_lock lock(stateMutex,std::try_to_lock);
        if (!lock) { ++registryDrops;return; }
        std::string key = std::string(component.substr(0,128)) + ":" + std::to_string(opcode) + ":" + std::string(detail.substr(0,192));
        auto it=missing.find(key);
        if(it==missing.end()) {
            if(missing.size()>=MaxUnsupported){++registryDrops;return;}
            it=missing.emplace(key,Missing{0,Now(),0,opcode,std::string(component.substr(0,128)),std::string(detail.substr(0,1024)),correlation}).first;
        }
        auto &m=it->second;++m.count;m.last=Now();
        if(m.count==1) Event("unsupported",opcode,0,0,detail);
    } catch (...) { ++registryDrops; }
}
void ObserveLog(std::string_view component,std::string_view text) noexcept {
    if(text.find("[STUB]")!=text.npos || text.find("STUBBED")!=text.npos || text.find("stubbed")!=text.npos || text.find("unimplemented")!=text.npos || text.find("Unimplemented")!=text.npos || text.find("unsupported")!=text.npos || text.find("Unsupported")!=text.npos)
        Unsupported(component,0,text);
}
void SaveArtifact(std::string_view kind, Id shader, const void *bytes, std::size_t size, std::string_view extension) noexcept {
    if (!RecorderEnabled() || (kind != "validation" && !ShaderCaptureEnabled()) || !bytes || !size) return;
    try {
        if (size > ArtifactLimit || !detail::IsFileName(kind) || !detail::IsFileName(extension)) { ++artifactDrops;return; }
        std::unique_lock lock(pendingMutex,std::try_to_lock);
        if (!lock || pendingArtifacts.size()>=64 || size>PendingLimit-pendingBytes) { ++artifactDrops;return; }
        auto data=static_cast<const unsigned char *>(bytes);
        pendingArtifacts.push_back({std::string(kind),std::string(extension),shader,{data,data+size}});
        pendingBytes+=size;
    } catch (...) { ++artifactDrops; }
}
static void WriteArtifact(std::string_view kind, Id shader, const void *bytes, std::size_t size, std::string_view extension) noexcept {
    if (!RecorderEnabled() || (kind != "validation" && !ShaderCaptureEnabled()) || !bytes || !size) return;
    try {
        std::lock_guard lock(artifactMutex);
        if(size>ArtifactLimit) { ++artifactDrops;return; }
        if(!detail::IsFileName(kind) || !detail::IsFileName(extension)) return;
        Id hash=14695981039346656037ULL;auto data=static_cast<const unsigned char*>(bytes);
        for(std::size_t i=0;i<size;i++){hash^=data[i];hash*=1099511628211ULL;}
        std::ostringstream name;name<<"shader-"<<kind<<'-'<<std::hex<<shader<<'-'<<hash<<'.'<<extension;
        auto path=OutputPath(name.str(),{});if(path.empty())return;
        if(std::filesystem::exists(path)){Event("shader_artifact_reuse",shader,hash,size,name.str());return;}
        if(size>ArtifactBudget-artifactBytes || artifactCount>=512) { ++artifactDrops;return; }
        auto temp=path+".tmp";
        {std::ofstream file(temp,std::ios::binary|std::ios::trunc);file.write(static_cast<const char*>(bytes),static_cast<std::streamsize>(size));file.close();if(!file){++artifactDrops;return;}}
        if(rename(temp.c_str(),path.c_str())!=0){++artifactDrops;return;}
        artifactBytes+=size;++artifactCount;Event("shader_artifact",shader,hash,size,name.str());
    } catch (...) { ++artifactDrops; }
}

bool FlushRecorder(std::string_view reason) noexcept {
    if(!RecorderEnabled()) return false;
    try {
        std::unique_lock flushLock(flushMutex,std::try_to_lock);if(!flushLock)return false;
        std::vector<Entry> entries(Capacity + IpcCapacity);
        if(ringLock.test_and_set(std::memory_order_acquire)) return false;
        const auto mainCount=count;
        const auto capturedIpcCount=ipcCount;
        const auto n=mainCount+capturedIpcCount;
        for(std::size_t i=0;i<mainCount;i++)entries[i]=ring[(head+Capacity-mainCount+i)%Capacity];
        for(std::size_t i=0;i<capturedIpcCount;i++)entries[mainCount+i]=ipcRing[(ipcHead+IpcCapacity-capturedIpcCount+i)%IpcCapacity];
        const auto snapshotTotal=total.load();
        ringLock.clear(std::memory_order_release);entries.resize(n);
        std::sort(entries.begin(),entries.end(),[](const Entry &a,const Entry &b){return a.sequence<b.sequence;});
        std::ostringstream events;
        for(auto &e:entries) events<<"{\"seq\":"<<e.sequence<<",\"ns\":"<<e.ns<<",\"tid\":"<<e.tid<<",\"frame\":"<<e.frame<<",\"operation\":"<<e.operation<<",\"submission\":"<<e.submission<<",\"pipeline\":"<<e.pipeline<<",\"kind\":"<<Quote(e.kind)<<",\"resource\":"<<e.resource<<",\"address\":"<<e.address<<",\"size\":"<<e.size<<",\"text\":"<<Quote(e.text)<<"}\n";
        bool ok=WriteJson("flight_recorder.jsonl",events.str());
        std::ostringstream map,unsupported,feature;
        map<<"{\"schema_version\":1,\"regions\":[";
        unsupported<<"{\"entries\":[";feature<<'{';
        {
            std::lock_guard lock(stateMutex);bool comma=false;
            for(auto &[key,r]:regions){if(comma)map<<',';comma=true;map<<"{\"space\":"<<key.first<<",\"start\":"<<key.second<<",\"end\":"<<r.end<<",\"host\":"<<r.host<<",\"generation\":"<<r.generation<<",\"sparse\":"<<(r.sparse?"true":"false")<<'}';}
            comma=false;for(auto &[key,m]:missing){if(comma)unsupported<<',';comma=true;unsupported<<"{\"component\":"<<Quote(m.component)<<",\"opcode\":"<<m.opcode<<",\"detail\":"<<Quote(m.detail)<<",\"count\":"<<m.count<<",\"first_ns\":"<<m.first<<",\"last_ns\":"<<m.last<<",\"first_operation\":"<<m.context.operation<<",\"first_submission\":"<<m.context.submission<<'}';}
            comma=false;for(auto &[name,status]:features){if(comma)feature<<',';comma=true;feature<<Quote(name)<<':'<<Quote(status);}
        }
        map<<"],\"dropped_updates\":"<<mappingDrops.load()<<",\"observed_updates_complete\":"<<(mappingDrops.load()==0?"true":"false")<<",\"scope\":\"observed mapping hooks only; not a validation of all guest memory\"}";
        unsupported<<"],\"dropped_updates\":"<<registryDrops.load()<<'}';feature<<'}';
        ok=WriteJson("gpu_mappings.json",map.str())&&ok;ok=WriteJson("unsupported.json",unsupported.str())&&ok;ok=WriteJson("diagnostic_features.json",feature.str())&&ok;
        std::ostringstream timings;timings<<"{\"units\":\"nanoseconds\",\"clock\":\"CPU monotonic wall time\",\"percentiles\":\"log2 bucket upper estimates\",\"metrics\":[";
        constexpr const char *names[]={"frame_interval","shader","pipeline","fence_wait","ipc","svc"};
        for(size_t k=0;k<histograms.size();++k) {
            auto &h=histograms[k];std::array<Id,64> buckets{};Id samples=0;
            for(size_t i=0;i<64;++i){buckets[i]=h.buckets[i].load();samples+=buckets[i];}
            auto percentile=[&](Id percent){if(!samples)return Id{};Id target=(samples*percent+99)/100,seen=0;for(unsigned i=0;i<64;++i){seen+=buckets[i];if(seen>=target)return i==63?UINT64_MAX:((Id{1}<<(i+1))-1);}return UINT64_MAX;};
            if(k)timings<<',';
            timings<<"{\"name\":\""<<names[k]<<"\",\"samples\":"<<samples<<",\"total_ns\":"<<h.sum.load()<<",\"p50_ns_upper\":"<<percentile(50)<<",\"p95_ns_upper\":"<<percentile(95)<<",\"p99_ns_upper\":"<<percentile(99)<<'}';
        }
        timings<<"]}";ok=WriteJson("performance_summary.json",timings.str())&&ok;
        auto p=GetProgress();std::ostringstream status;
        status<<"{\"schema_version\":1,\"reason\":"<<Quote(reason)<<",\"capture_request\":"<<CaptureRequest()<<",\"ns\":"<<Now()<<",\"draws\":"<<p.draws<<",\"dispatches\":"<<p.dispatches<<",\"submissions\":"<<p.submitted<<",\"completions\":"<<p.completed<<",\"presents\":"<<p.frames<<",\"paused\":"<<(IsPaused()?"true":"false")<<",\"ring_capacity\":"<<(Capacity+IpcCapacity)<<",\"render_ring_capacity\":"<<Capacity<<",\"ipc_ring_capacity\":"<<IpcCapacity<<",\"retained_ipc_events\":"<<capturedIpcCount<<",\"total_events\":"<<total.load()<<",\"dropped_events\":"<<dropped.load()<<",\"overwritten_events\":"<<(snapshotTotal>n?snapshotTotal-n:0)<<",\"artifact_drops\":"<<artifactDrops.load()<<",\"frame_capture_pending\":"<<(frameCapture.load()?"true":"false")<<",\"files_written\":"<<(ok?"true":"false")<<'}';
        return WriteJson("recorder_status.json",status.str())&&ok;
    } catch (...) { return false; }
}
void InitializeRecorder() noexcept {
    // Called before emulation workers start; the session directory is immutable afterwards.
    pump.Stop();
    activities.Reset();cpuSamples={};traceHead=traceCount=traceBytes=0;traceDrops=traceWritten=traceIoErrors=traceBudgetDrops=threadSnapshotFailures=traceStatusFailures=0;traceEnabled=true;
    {std::lock_guard lock(pendingMutex);pendingArtifacts.clear();pendingBytes=0;}
    lastPresent=0;for(auto &h:histograms){h.samples=0;h.sum=0;for(auto &b:h.buckets)b=0;}
    enabled=false;head=count=ipcHead=ipcCount=artifactBytes=artifactCount=0;total=dropped=captures=artifactDrops=mappingDrops=registryDrops=0;
    draws=dispatches=submitted=completed=frames=0;paused=false;frameCapture=false;validationMode=0;shaderCapture=true;
    try {
        regions.clear();missing.clear();features.clear();
        std::ifstream options(OutputPath("diagnostic_options.cfg",{}));int captureShaders=1;
        int requestedMode{};
        if(options>>requestedMode>>captureShaders){validationMode=std::clamp(requestedMode,0,3);shaderCapture=captureShaders!=0;}
        int requestedTrace=1;
        if(options>>requestedTrace)traceEnabled=requestedTrace!=0;
        {std::ofstream trace(OutputPath("session_trace.jsonl",{}),std::ios::trunc);}
        enabled=!OutputPath("flight_recorder.jsonl",{}).empty();
        Feature("thread_activity","256 thread identities; 8 nested native activities; separate snapshot every second; lost updates invalidate certainty; normal waits are not deadlocks");
        Feature("session_trace",traceEnabled?"enabled from native JNI entry; 16384 queued events; async 250ms drain; first 32 MiB preserved; newest events in flight_recorder; losses counted; no guest instruction or every C++ function tracing":"disabled via third diagnostic_options.cfg integer");
        Feature("recorder","active; separate bounded rings: 8192 general and 2048 IPC events; nonblocking producers; dropped counts explicit");
        Feature("mapping","incremental mapped-region registry; holes are absent ranges; dropped updates invalidate completeness");
        Feature("unsupported","active; explicit IPC/compute/texel hooks plus observed logs; bounded 512 distinct entries");
        Feature("watchdog","not_started; guest sampler starts after game loading; recorder background export remains independent");
        Feature("renderdoc","not_checked");Feature("device_fault","not_checked");
        Feature("shader_capture",shaderCapture?"enabled; shared shader/validation quota 32 MiB, 4 MiB per artifact, 512 files":"disabled; validation artifacts still allowed within shared quota");
        Event("session_start");
        if(RecorderEnabled())pump.Start();
    } catch (...) { enabled=false; }
}
void ShutdownRecorder() noexcept { pump.Stop();DrainTrace();SnapshotThreads();DrainArtifacts();FlushRecorder("shutdown");enabled=false; }
}
