// SPDX-License-Identifier: MPL-2.0
#include "diagnostic_recorder.h"
#include "diagnostic_session.h"
#include <thread>
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
namespace {
    constexpr std::size_t Capacity = 8192, MaxMappings = 65536, MaxUnsupported = 512;
    constexpr std::size_t ArtifactBudget = 32 * 1024 * 1024, ArtifactLimit = 4 * 1024 * 1024;
    struct Entry {
        Id sequence{}, ns{}, tid{}, frame{}, operation{}, submission{}, pipeline{}, resource{}, address{}, size{};
        char kind[32]{}, text[192]{};
    };
    std::array<Entry, Capacity> ring{};
    std::atomic_flag ringLock = ATOMIC_FLAG_INIT;
    std::atomic<Id> dropped{}, total{}, captures{}, artifactDrops{}, mappingDrops{}, registryDrops{};
    std::atomic<Id> draws{}, dispatches{}, submitted{}, completed{}, frames{};
    std::atomic<bool> enabled{}, paused{}, frameCapture{};
    int validationMode{};
    bool shaderCapture{true};
    std::size_t head{}, count{}, artifactBytes{}, artifactCount{};
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
                std::unique_lock lock(mutex);
                while(!cv.wait_for(lock,std::chrono::seconds(1),[this]{return stop;})) {
                    const auto request=CaptureRequest();const auto now=std::chrono::steady_clock::now();
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
        return static_cast<Id>(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
    }
    template<std::size_t N> void Copy(char (&dst)[N], std::string_view text) noexcept {
        auto n = std::min(text.size(), N - 1);if(n) std::memcpy(dst, text.data(), n);dst[n] = 0;
    }
    std::string Quote(std::string_view s) { return "\"" + JsonEscape(s) + "\""; }
}

bool RecorderEnabled() noexcept { return enabled.load(std::memory_order_relaxed); }
int ValidationMode() noexcept { return validationMode; }
bool ShaderCaptureEnabled() noexcept { return RecorderEnabled() && shaderCapture; }
void Event(std::string_view kind, Id resource, Id address, Id size, std::string_view text) noexcept {
    if (!RecorderEnabled()) return;
    if (ringLock.test_and_set(std::memory_order_acquire)) { ++dropped;return; }
    auto &e = ring[head];e = {};
    e.sequence = ++total;e.ns = Now();e.tid = syscall(SYS_gettid);e.frame = frames.load();
    e.operation = correlation.operation;e.submission = correlation.submission;e.pipeline = correlation.pipeline;
    e.resource = resource;e.address = address;e.size = size;Copy(e.kind,kind);Copy(e.text,text);
    head = (head + 1) % Capacity;count = std::min(count + 1, Capacity);
    ringLock.clear(std::memory_order_release);
}
void CountDraw(bool compute) noexcept { if (compute) ++dispatches;else ++draws; }
void Submitted(Id cycle) noexcept { ++submitted;Event("submit_accepted",cycle); }
void Completed(Id cycle) noexcept { ++completed;Event("fence_completed",cycle); }
Progress GetProgress() noexcept { return {draws.load(), dispatches.load(), submitted.load(), completed.load(), frames.load()}; }
void Presented() noexcept { ++frames;Event("present_accepted"); }
void SetPaused(bool value) noexcept { paused = value;Event(value ? "paused" : "resumed"); }
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
        {std::ofstream file(temp,std::ios::binary|std::ios::trunc);file.write(static_cast<const char*>(bytes),size);file.close();if(!file){++artifactDrops;return;}}
        if(rename(temp.c_str(),path.c_str())!=0){++artifactDrops;return;}
        artifactBytes+=size;++artifactCount;Event("shader_artifact",shader,hash,size,name.str());
    } catch (...) { ++artifactDrops; }
}

bool FlushRecorder(std::string_view reason) noexcept {
    if(!RecorderEnabled()) return false;
    try {
        std::unique_lock flushLock(flushMutex,std::try_to_lock);if(!flushLock)return false;
        std::vector<Entry> entries(Capacity);
        if(ringLock.test_and_set(std::memory_order_acquire)) return false;
        auto n=count;for(std::size_t i=0;i<n;i++)entries[i]=ring[(head+Capacity-n+i)%Capacity];
        const auto snapshotTotal=total.load();
        ringLock.clear(std::memory_order_release);entries.resize(n);
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
        map<<"],\"dropped_updates\":"<<mappingDrops.load()<<'}';
        unsupported<<"],\"dropped_updates\":"<<registryDrops.load()<<'}';feature<<'}';
        ok=WriteJson("gpu_mappings.json",map.str())&&ok;ok=WriteJson("unsupported.json",unsupported.str())&&ok;ok=WriteJson("diagnostic_features.json",feature.str())&&ok;
        auto p=GetProgress();std::ostringstream status;
        status<<"{\"schema_version\":1,\"reason\":"<<Quote(reason)<<",\"capture_request\":"<<CaptureRequest()<<",\"ns\":"<<Now()<<",\"draws\":"<<p.draws<<",\"dispatches\":"<<p.dispatches<<",\"submissions\":"<<p.submitted<<",\"completions\":"<<p.completed<<",\"presents\":"<<p.frames<<",\"paused\":"<<(IsPaused()?"true":"false")<<",\"ring_capacity\":"<<Capacity<<",\"total_events\":"<<total.load()<<",\"dropped_events\":"<<dropped.load()<<",\"overwritten_events\":"<<(snapshotTotal>n?snapshotTotal-n:0)<<",\"artifact_drops\":"<<artifactDrops.load()<<",\"frame_capture_pending\":"<<(frameCapture.load()?"true":"false")<<",\"files_written\":"<<(ok?"true":"false")<<'}';
        return WriteJson("recorder_status.json",status.str())&&ok;
    } catch (...) { return false; }
}
void InitializeRecorder() noexcept {
    // Called before emulation workers start; the session directory is immutable afterwards.
    pump.Stop();
    enabled=false;head=count=artifactBytes=artifactCount=0;total=dropped=captures=artifactDrops=mappingDrops=registryDrops=0;
    draws=dispatches=submitted=completed=frames=0;paused=false;frameCapture=false;validationMode=0;shaderCapture=true;
    try {
        regions.clear();missing.clear();features.clear();
        std::ifstream options(OutputPath("diagnostic_options.cfg",{}));int captureShaders=1;
        int requestedMode{};
        if(options>>requestedMode>>captureShaders){validationMode=std::clamp(requestedMode,0,3);shaderCapture=captureShaders!=0;}
        enabled=!OutputPath("flight_recorder.jsonl",{}).empty();
        Feature("recorder","active; bounded 8192 events; nonblocking producers; dropped counts explicit");
        Feature("mapping","incremental mapped-region registry; holes are absent ranges; dropped updates invalidate completeness");
        Feature("unsupported","active; explicit IPC/compute/texel hooks plus observed logs; bounded 512 distinct entries");
        Feature("watchdog","not_started; guest sampler starts after game loading; recorder background export remains independent");
        Feature("renderdoc","not_checked");Feature("device_fault","not_checked");
        Feature("shader_capture",shaderCapture?"enabled; shared shader/validation quota 32 MiB, 4 MiB per artifact, 512 files":"disabled; validation artifacts still allowed within shared quota");
        Event("session_start");
        if(RecorderEnabled())pump.Start();
    } catch (...) { enabled=false; }
}
void ShutdownRecorder() noexcept { pump.Stop();FlushRecorder("shutdown");enabled=false; }
}
