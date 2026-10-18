// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string_view>

namespace skyline::diagnostics {
    using Id = std::uint64_t;
    struct Correlation { Id operation{}, submission{}, pipeline{}; };
    inline thread_local Correlation correlation{};
    inline std::atomic<Id> nextDiagnosticId{1};
    inline Id NewId() noexcept { return nextDiagnosticId.fetch_add(1, std::memory_order_relaxed); }
    struct Scope {
        Correlation previous{correlation};
        Scope(Id submission, Id pipeline = 0) noexcept { correlation = {NewId(), submission, pipeline}; }
        ~Scope() { correlation = previous; }
    };
    struct Progress { Id draws{}, dispatches{}, submitted{}, completed{}, frames{}; };
    enum class Timing : unsigned { Frame, Shader, Pipeline, Fence, Ipc, Svc, Count };
    void RecordDuration(Timing kind, Id nanoseconds) noexcept;
    // CPU wall duration, not a GPU timestamp. Unfinished scopes remain visible as failures.
    class Operation {
        const char *kind;
        Id resource, id;
        Timing timing;
        std::chrono::steady_clock::time_point start;
        bool finished{};
      public:
        Operation(const char *kind, Id resource, Timing timing) noexcept;
        Operation(const Operation &) = delete;
        Operation &operator=(const Operation &) = delete;
        ~Operation();
        void Finish(Id result = 0, std::string_view detail = {}) noexcept;
    };
    void InitializeRecorder() noexcept;
    void ShutdownRecorder() noexcept;
    bool RecorderEnabled() noexcept;
    int ValidationMode() noexcept; // 0 off, 1 core, 2 synchronization, 3 GPU-assisted
    bool ShaderCaptureEnabled() noexcept;
    void Event(std::string_view kind, Id resource = 0, Id address = 0, Id size = 0, std::string_view text = {}) noexcept;
    void CountDraw(bool compute = false) noexcept;
    void Submitted(Id cycle) noexcept;
    void Completed(Id cycle) noexcept;
    Progress GetProgress() noexcept;
    void Presented() noexcept;
    void SetPaused(bool paused) noexcept;
    bool IsPaused() noexcept;
    Id RequestCapture() noexcept;
    Id CaptureRequest() noexcept;
    void RequestFrameCapture() noexcept;
    bool ConsumeFrameCapture() noexcept;
    void Feature(std::string_view name, std::string_view status) noexcept;
    void Mapping(Id space, Id address, Id size, Id host, bool sparse, bool unmap) noexcept;
    void DestroyAddressSpace(Id space) noexcept;
    void Unsupported(std::string_view component, Id opcode, std::string_view detail) noexcept;
    void ObserveLog(std::string_view component, std::string_view text) noexcept;
    void SaveArtifact(std::string_view kind, Id shader, const void *bytes, std::size_t size, std::string_view extension) noexcept;
    bool FlushRecorder(std::string_view reason) noexcept; // Call outside emulator locks, never from a signal handler
}
