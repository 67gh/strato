// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <vulkan/vulkan_raii.hpp>
#include <gpu/texture/texture.h>

namespace skyline::vfs {
    class FileSystem;
}

namespace skyline::gpu {
    class GPU;
    class FenceCycle;

    /**
     * @brief Implements host-side motion-compensated frame generation, interpolating N-1 extra
     * frames between every pair of frames the game presents in order to smooth out perceived motion
     * @note This is a purely host-side visual effect; it does not affect emulation timing, input
     * latency or the rate at which the guest actually renders frames
     */
    class FrameGenerator {
      public:
        /**
         * @brief The frame generation mode, controls how many total frames are shown for every
         * real frame the game renders
         */
        enum class Mode {
            Stable = 0, //!< Frame generation disabled, frames are presented as rendered by the game
            X2 = 1, //!< 1 generated frame is inserted between every pair of real frames (2x the presented framerate)
            X3 = 2, //!< 2 generated frames are inserted between every pair of real frames (3x the presented framerate)
            X4 = 3, //!< 3 generated frames are inserted between every pair of real frames (4x the presented framerate)
        };

        /**
         * @return The amount of frames shown in total (real + generated) for one real frame under the given mode
         */
        static constexpr u32 FrameMultiplier(Mode mode) {
            switch (mode) {
                case Mode::Stable:
                    return 1;
                case Mode::X2:
                    return 2;
                case Mode::X3:
                    return 3;
                case Mode::X4:
                    return 4;
            }
            return 1;
        }

      private:
        GPU &gpu;

        vk::raii::ShaderModule motionEstimateShaderModule;
        vk::raii::ShaderModule frameInterpolateShaderModule;

        vk::raii::DescriptorSetLayout motionEstimateSetLayout;
        vk::raii::DescriptorSetLayout frameInterpolateSetLayout;
        vk::raii::PipelineLayout motionEstimatePipelineLayout;
        vk::raii::PipelineLayout frameInterpolatePipelineLayout;
        vk::raii::Pipeline motionEstimatePipeline;
        vk::raii::Pipeline frameInterpolatePipeline;

        vk::raii::Sampler linearSampler;

        static constexpr int BaseBlockSize{16}; //!< Block size (px) for the finest motion estimation level
        static constexpr int SearchRadius{12}; //!< Search radius (px) at each pyramid level
        static constexpr size_t MaxGeneratedFrames{3}; //!< Maximum simultaneous generated frames, i.e. for 4x mode

        texture::Dimensions cachedExtent{}; //!< The frame extent the below resources were sized for

        std::shared_ptr<Texture> motionFieldCoarse; //!< Coarse-level (half resolution blocks) motion vector field
        std::shared_ptr<Texture> motionFieldFine; //!< Finest-level motion vector field, used for interpolation
        std::array<std::shared_ptr<Texture>, MaxGeneratedFrames> generatedFrames; //!< Ring of output textures for generated frames

        /**
         * @brief (Re)allocates the motion field and generated frame textures if the presentation extent changed
         */
        void EnsureResources(texture::Dimensions extent);

        /**
         * @brief Dispatches the motion estimation pass for a given pyramid level
         */
        void DispatchMotionEstimate(vk::raii::CommandBuffer &cmd, TextureView *prevFrame, TextureView *currFrame,
                                     Texture *motionOut, Texture *prevLevelMotion, texture::Dimensions levelExtent, int blockSize);

        /**
         * @brief Dispatches the interpolation pass, writing one generated frame at interpolation factor 't'
         */
        void DispatchInterpolate(vk::raii::CommandBuffer &cmd, TextureView *prevFrame, TextureView *currFrame,
                                  Texture *motionField, Texture *output, texture::Dimensions extent, float t);

      public:
        FrameGenerator(GPU &gpu, std::shared_ptr<vfs::FileSystem> shaderFileSystem);

        /**
         * @brief Generates the intermediate frames between 'prevFrame' and 'currFrame' according to 'mode'
         * @param cmd A command buffer in the recording state that the generation work will be appended to
         * @param prevFrame The previously presented frame (N-1)
         * @param currFrame The most recently presented frame (N), i.e. the one that triggered this generation
         * @param mode How many frames to generate, see FrameMultiplier()
         * @return A list of (FrameMultiplier(mode) - 1) generated frames, in presentation order, that should be
         * shown between 'prevFrame' and 'currFrame'
         * @note The returned texture views are only valid for reading until the next call to this function,
         * as the underlying textures are reused in a ring buffer; the caller is responsible for attaching
         * them to whatever FenceCycle guards the submission that 'cmd' is part of
         */
        std::vector<std::shared_ptr<TextureView>> GenerateFrames(vk::raii::CommandBuffer &cmd, TextureView *prevFrame, TextureView *currFrame, Mode mode);
    };
}
