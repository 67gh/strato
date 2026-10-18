// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <common/diagnostic_recorder.h>

namespace skyline::soc::host1x {
    // Diagnostic snapshots only; this class does not decode or complete video jobs.
    class VideoCapture {
      public:
        enum class Engine : std::uint32_t { Nvdec = 1, Vic = 2 };
        static constexpr std::uint32_t ExecuteMethod = 0xc0;
      private:
        std::array<std::uint32_t, 0x446> registers{};
        diagnostics::Id channel{diagnostics::NewId()}, lastRequest{};
        std::uint32_t sequence{}, captures{};
        Engine engine;

        static void Append(std::vector<std::uint8_t> &out, std::uint32_t word) {
            for (unsigned shift = 0; shift < 32; shift += 8)
                out.push_back(static_cast<std::uint8_t>(word >> shift));
        }
        static std::uint32_t Word(const std::vector<std::uint8_t> &bytes, std::size_t offset) {
            if (offset > bytes.size() || bytes.size() - offset < 4)
                throw std::runtime_error("video picture header too short");
            std::uint32_t value{};
            for (unsigned i = 0; i < 4; ++i)
                value |= std::uint32_t{bytes[offset + i]} << (8 * i);
            return value;
        }
      public:
        explicit VideoCapture(Engine engine) : engine{engine} {}

        // The reader must read the SMMU, including across mapping boundaries.
        template<typename Reader>
        void Method(std::uint32_t method, std::uint32_t argument, Reader &&read) {
            const auto component{engine == Engine::Nvdec ? "nvdec" : "vic"};
            if (method >= registers.size()) {
                diagnostics::Unsupported(component, method, "video method outside register bank");
                return;
            }
            registers[method] = argument;
            if (method != ExecuteMethod)
                return;
            ++sequence;
            diagnostics::Unsupported(component, ExecuteMethod, "video engine not implemented; no output image produced");
            diagnostics::Event("video_execute", channel, static_cast<std::uint32_t>(engine), sequence, component);
            if (!diagnostics::ShaderCaptureEnabled())
                return;
            const auto request{diagnostics::CaptureRequest()};
            if (captures >= 8 || (captures >= 2 && request == lastRequest))
                return;
            ++captures;
            lastRequest = request;
            const auto codec{engine == Engine::Nvdec ? registers[0x80] : 0U};
            std::vector<std::uint8_t> chunks;
            std::uint32_t chunkCount{}, status{};
            auto capture = [&](std::uint32_t tag, std::uint64_t address, std::uint32_t size) {
                constexpr std::uint64_t End{1ULL << 32};
                constexpr std::uint32_t MaxChunk{1024 * 1024};
                if (!size || size > MaxChunk || address >= End || size > End - address)
                    throw std::runtime_error("video capture range invalid or exceeds 1 MiB");
                std::vector<std::uint8_t> bytes(size);
                read(bytes.data(), address, size);
                Append(chunks, tag);Append(chunks, static_cast<std::uint32_t>(address));Append(chunks, size);
                chunks.insert(chunks.end(), bytes.begin(), bytes.end());
                ++chunkCount;
                return bytes;
            };
            try {
                if (engine == Engine::Nvdec) {
                    // NVDEC methods are word indices; addresses are in 256-byte units.
                    const std::uint32_t pictureSize{codec == 3 ? 0x2fcU : codec == 5 ? 0xc0U : codec == 9 ? 0x100U : 0U};
                    if (!pictureSize)
                        throw std::runtime_error("video capture: unknown codec picture layout");
                    const auto picture{capture(1, std::uint64_t{registers[0x101]} << 8, pictureSize)};
                    const auto streamSize{Word(picture, codec == 9 ? 0x30 : 0x48)};
                    capture(2, std::uint64_t{registers[0x102]} << 8, streamSize);
                    if (codec == 9)
                        capture(4, std::uint64_t{registers[0x170]} << 8, 0xea0);
                } else {
                    capture(3, std::uint64_t{registers[0x1c2]} << 8, 0x610);
                }
            } catch (const std::exception &error) {
                status = 1;
                diagnostics::Event("video_capture_incomplete", channel, codec, sequence, error.what());
            }
            std::vector<std::uint8_t> snapshot;
            for (auto word : {0x43563148U, 1U, static_cast<std::uint32_t>(engine), sequence, codec,
                              static_cast<std::uint32_t>(registers.size()), chunkCount, status})
                Append(snapshot, word);
            for (auto word : registers)
                Append(snapshot, word);
            snapshot.insert(snapshot.end(), chunks.begin(), chunks.end());
            diagnostics::SaveArtifact(component, channel, snapshot.data(), snapshot.size(), "bin");
        }
    };
}
