// SPDX-License-Identifier: MPL-2.0
// Copyright © 2022 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include <common/base.h>
#include <limits>
#include <stdexcept>

#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_enums.hpp>

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
