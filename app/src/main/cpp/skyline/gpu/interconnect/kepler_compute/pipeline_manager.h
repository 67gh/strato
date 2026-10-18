// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <vector>
#include <tsl/robin_map.h>
#include <shader_compiler/frontend/ir/program.h>
#include <gpu/interconnect/common/samplers.h>
#include <gpu/interconnect/common/textures.h>
#include "packed_pipeline_state.h"
#include "constant_buffers.h"

namespace skyline::gpu {
    class TextureView;
}

namespace skyline::gpu::interconnect::kepler_compute {
    class Pipeline {
      public:
        struct ShaderStage {
            vk::ShaderModule module;
            Shader::Info info;
        };

        struct DescriptorInfo {
            std::vector<vk::DescriptorSetLayoutBinding> descriptorSetLayoutBindings;

            u32 totalWriteDescCount;
            u32 totalBufferDescCount;
            u32 totalTexelBufferDescCount;
            u32 totalImageDescCount;

            bool hasUnsupportedDescriptors; //!< Storage images or texel buffers are declared by the shader but never written to the descriptor set
        };

        struct CompiledPipeline {
            vk::raii::DescriptorSetLayout descriptorSetLayout;
            vk::raii::PipelineLayout pipelineLayout;
            vk::raii::Pipeline pipeline;
        };

      private:
        ShaderStage shaderStage;
        DescriptorInfo descriptorInfo;
        std::vector<CachedMappedBufferView> storageBufferViews;
        ContextTag lastExecutionTag{}; //!< The last execution tag this pipeline was used at
        bool skipLogged{}; //!< If the skipped dispatches of this pipeline were already reported

        void SyncCachedStorageBufferViews(ContextTag executionTag);

      public:
        CompiledPipeline compiledPipeline;

        PackedPipelineState sourcePackedState;

        /**
         * @return If the shader uses descriptors that Strato doesn't write (storage images, texel buffers), dispatching it makes the Adreno GPU hang so it is skipped
         */
        bool HasUnsupportedDescriptors() const {
            return descriptorInfo.hasUnsupportedDescriptors;
        }

        /**
         * @return True the first time it's called, so a skipped pipeline is only reported once
         */
        bool ShouldReportSkip() {
            return !std::exchange(skipLogged, true);
        }

        bool MatchesTexelTypes(InterconnectContext &ctx, Textures &textures, ConstantBufferSet &constantBuffers) const;

        Pipeline(InterconnectContext &ctx, Textures &textures, ConstantBufferSet &constantBuffers, const PackedPipelineState &packedState, const ShaderBinary &shaderBinary);

        /**
         * @brief Creates a descriptor set update from the current GPU state
         */
        DescriptorUpdateInfo *SyncDescriptors(InterconnectContext &ctx, ConstantBufferSet &constantBuffers, Samplers &samplers, Textures &textures, vk::PipelineStageFlags &srcStageMask, vk::PipelineStageFlags &dstStageMask);
    };

    class PipelineManager {
      private:
        tsl::robin_map<PackedPipelineState, std::vector<std::unique_ptr<Pipeline>>, util::ObjectHash<PackedPipelineState>> map;

      public:
        Pipeline *FindOrCreate(InterconnectContext &ctx, Textures &textures, ConstantBufferSet &constantBuffers, const PackedPipelineState &packedState, const ShaderBinary &shaderBinary) {
            auto &variants{map[packedState]};
            for (auto &pipeline : variants)
                if (pipeline->MatchesTexelTypes(ctx, textures, constantBuffers))
                    return pipeline.get();
            // Retain old variants: in-flight command buffers can still reference them.
            auto pipeline{std::make_unique<Pipeline>(ctx, textures, constantBuffers, packedState, shaderBinary)};
            auto *result{pipeline.get()};
            variants.push_back(std::move(pipeline));
            return result;
        }
    };
}
