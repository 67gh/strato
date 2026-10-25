// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include "bc_decoder.h"

// CPU readback fallback, not an offline quality encoder. Preserve original blocks
// when their decoded texels match; only GPU-modified blocks undergo lossy encoding.
// Bit layouts: Khronos Data Format Specification, S3TC/RGTC/BPTC chapters.
namespace bcn::readback {
    enum class Format { Bc1, Bc2, Bc3, Bc4, Bc4Signed, Bc5, Bc5Signed, Bc6, Bc6Signed, Bc7 };

    inline size_t Product(size_t a, size_t b) {
        if (b && a > std::numeric_limits<size_t>::max() / b)
            throw std::overflow_error("BC readback size overflow");
        return a * b;
    }
    inline size_t PixelBytes(Format f) {
        switch (f) {
            case Format::Bc4: case Format::Bc4Signed: return 1;
            case Format::Bc5: case Format::Bc5Signed: return 2;
            case Format::Bc6: case Format::Bc6Signed: return 8;
            default: return 4;
        }
    }
    inline size_t BlockBytes(Format f) {
        return f == Format::Bc1 || f == Format::Bc4 || f == Format::Bc4Signed ? 8 : 16;
    }
    inline size_t EncodedSize(Format f, size_t w, size_t h, size_t depth) {
        return Product(Product(Product(w / 4 + (w % 4 != 0), h / 4 + (h % 4 != 0)), depth), BlockBytes(f));
    }
    inline size_t DecodedSize(Format f, size_t w, size_t h, size_t depth) {
        return Product(Product(Product(w, h), depth), PixelBytes(f));
    }
    inline void DecodeBlock(Format f, const uint8_t *src, uint8_t *dst) {
        switch (f) {
            case Format::Bc1: DecodeBc1(src, dst, 4, 4, true); break;
            case Format::Bc2: DecodeBc2(src, dst, 4, 4); break;
            case Format::Bc3: DecodeBc3(src, dst, 4, 4); break;
            case Format::Bc4: case Format::Bc4Signed: DecodeBc4(src, dst, 4, 4, f == Format::Bc4Signed); break;
            case Format::Bc5: case Format::Bc5Signed: DecodeBc5(src, dst, 4, 4, f == Format::Bc5Signed); break;
            case Format::Bc6: case Format::Bc6Signed: DecodeBc6(src, dst, 4, 4, f == Format::Bc6Signed); break;
            case Format::Bc7: DecodeBc7(src, dst, 4, 4); break;
        }
    }

    namespace detail {
        using Pixel = std::array<int, 4>;
        using Pixels = std::array<Pixel, 16>;
        inline void Store(uint8_t *out, uint64_t value, unsigned bytes) {
            for (unsigned i{}; i < bytes; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
        }
        inline int Distance(const Pixel &a, const Pixel &b, unsigned channels) {
            int distance{};
            for (unsigned c{}; c < channels; ++c) { int d{a[c] - b[c]}; distance += d * d; }
            return distance;
        }
        // Pick endpoints along the channel with greatest range. Avoid choosing
        // RGB-box corners that are not present (e.g. magenta for red/blue pixels).
        inline std::array<Pixel, 2> Endpoints(const Pixels &p, unsigned channels, bool skipTransparent = false) {
            std::array<Pixel, 2> endpoints{};
            int bestRange{-1};
            for (unsigned c{}; c < channels; ++c) {
                int low{std::numeric_limits<int>::max()}, high{std::numeric_limits<int>::min()};
                size_t lo{}, hi{};
                for (size_t i{}; i < p.size(); ++i) {
                    if (skipTransparent && p[i][3] < 128) continue;
                    if (p[i][c] < low) { low = p[i][c]; lo = i; }
                    if (p[i][c] > high) { high = p[i][c]; hi = i; }
                }
                if (low <= high && high - low > bestRange) { bestRange = high - low; endpoints = {p[lo], p[hi]}; }
            }
            return endpoints;
        }
        inline unsigned Pack565(const Pixel &p) {
            return static_cast<unsigned>(((p[0] * 31 + 127) / 255) << 11 |
                                         ((p[1] * 63 + 127) / 255) << 5 |
                                          (p[2] * 31 + 127) / 255);
        }
        inline Pixel Unpack565(unsigned v) {
            int r{static_cast<int>(v >> 11)}, g{static_cast<int>((v >> 5) & 63)}, b{static_cast<int>(v & 31)};
            return {(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255};
        }
        inline void Color(const Pixels &p, uint8_t *out, bool punchThrough) {
            bool transparent{punchThrough && std::any_of(p.begin(), p.end(), [](const Pixel &v) { return v[3] < 128; })};
            auto endpoints{Endpoints(p, 3, transparent)};
            unsigned a{Pack565(endpoints[0])}, b{Pack565(endpoints[1])};
            if (transparent ? a > b : a < b) std::swap(a, b);
            // BC1 opaque colors must select the four-color mode, even for solid blocks.
            if (!transparent && a == b) { if (a < 65535) ++a; else --b; }
            std::array<Pixel, 4> palette{Unpack565(a), Unpack565(b), Pixel{}, Pixel{}};
            for (unsigned c{}; c < 3; ++c) {
                palette[2][c] = transparent ? (palette[0][c] + palette[1][c]) / 2 : (2 * palette[0][c] + palette[1][c]) / 3;
                palette[3][c] = transparent ? 0 : (palette[0][c] + 2 * palette[1][c]) / 3;
            }
            uint32_t indices{};
            for (unsigned i{}; i < 16; ++i) {
                unsigned best{}; int error{std::numeric_limits<int>::max()};
                if (transparent && p[i][3] < 128) best = 3;
                else for (unsigned j{}; j < (transparent ? 3U : 4U); ++j) {
                    auto e{Distance(p[i], palette[j], 3)};
                    if (e < error) { error = e; best = j; }
                }
                indices |= best << (i * 2);
            }
            Store(out, a, 2); Store(out + 2, b, 2); Store(out + 4, indices, 4);
        }
        inline void Channel(const Pixels &p, uint8_t *out, unsigned c) {
            int low{p[0][c]}, high{low};
            for (const auto &v : p) { low = std::min(low, v[c]); high = std::max(high, v[c]); }
            std::array<int, 8> palette{high, low};
            if (high > low)
                for (int j{2}; j < 8; ++j) palette[j] = ((8 - j) * high + (j - 1) * low) / 7;
            else palette.fill(low);
            uint64_t bits{static_cast<uint8_t>(high) | (uint64_t{static_cast<uint8_t>(low)} << 8)};
            for (unsigned i{}; i < 16; ++i) {
                unsigned best{}; int error{std::numeric_limits<int>::max()};
                for (unsigned j{}; j < 8; ++j) {
                    int d{p[i][c] - palette[j]}, e{d * d};
                    if (e < error) { error = e; best = j; }
                }
                bits |= uint64_t{best} << (16 + 3 * i);
            }
            Store(out, bits, 8);
        }
        struct Bits {
            uint8_t *out; unsigned position{};
            void Put(unsigned value, unsigned count) {
                for (unsigned i{}; i < count; ++i, ++position)
                    out[position / 8] |= static_cast<uint8_t>(((value >> i) & 1) << (position % 8));
            }
        };
        inline constexpr std::array<int, 16> Weights{0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
        inline std::array<unsigned, 16> Indices(const Pixels &pixels, const std::array<Pixel, 16> &palette, unsigned channels) {
            std::array<unsigned, 16> indices{};
            for (unsigned i{}; i < 16; ++i) {
                // BC6 comparison uses signed half-float magnitudes (log-like error).
                int64_t error{std::numeric_limits<int64_t>::max()};
                for (unsigned j{}; j < 16; ++j) {
                    int64_t e{};
                    for (unsigned c{}; c < channels; ++c) { int64_t d{pixels[i][c] - palette[j][c]}; e += d * d; }
                    if (e < error) { error = e; indices[i] = j; }
                }
            }
            return indices;
        }
        inline void Bc7(const Pixels &p, uint8_t *out) {
            auto endpoints{Endpoints(p, 4)};
            // Mode 6: 7-bit RGBA endpoints + one shared low bit per endpoint.
            for (auto &v : endpoints) {
                Pixel choices[2]{}; int errors[2]{};
                for (int parity{}; parity < 2; ++parity)
                    for (unsigned c{}; c < 4; ++c) {
                        int q{std::clamp((v[c] - parity + 1) / 2, 0, 127)};
                        choices[parity][c] = 2 * q + parity;
                        int d{v[c] - choices[parity][c]}; errors[parity] += d * d;
                    }
                v = choices[errors[1] < errors[0] ? 1 : 0];
            }
            std::array<Pixel, 16> palette{};
            for (unsigned i{}; i < 16; ++i) for (unsigned c{}; c < 4; ++c)
                palette[i][c] = ((64 - Weights[i]) * endpoints[0][c] + Weights[i] * endpoints[1][c] + 32) >> 6;
            auto indices{Indices(p, palette, 4)};
            if (indices[0] >= 8) { std::swap(endpoints[0], endpoints[1]); for (auto &i : indices) i = 15 - i; }
            Bits bits{out}; bits.Put(64, 7);
            for (unsigned c{}; c < 4; ++c) for (auto &v : endpoints) bits.Put(static_cast<unsigned>(v[c] >> 1), 7);
            for (auto &v : endpoints) bits.Put(static_cast<unsigned>(v[0] & 1), 1);
            for (unsigned i{}; i < 16; ++i) bits.Put(indices[i], i == 0 ? 3 : 4);
        }
        inline int Unquantize6(int q, bool sign) {
            if (!q) return 0;
            const int magnitude{q < 0 ? -q : q};
            int value{sign ? (magnitude >= 511 ? 32767 : ((magnitude << 15) + 0x4000) >> 9)
                           : (magnitude == 1023 ? 65535 : ((magnitude << 16) + 0x8000) >> 10)};
            return q < 0 ? -value : value;
        }
        inline int Finish6(int value, bool sign) {
            int magnitude{value < 0 ? -value : value};
            int half{(magnitude * 31) >> (sign ? 5 : 6)};
            return value < 0 ? -half : half;
        }
        inline int Quantize6(int value, bool sign) {
            bool negative{sign && value < 0};
            int magnitude{negative ? -value : std::max(value, 0)};
            int low{}, high{sign ? 511 : 1023};
            while (low < high) {
                int mid{(low + high) / 2};
                if (Finish6(Unquantize6(mid, sign), sign) < magnitude) low = mid + 1; else high = mid;
            }
            if (low && magnitude - Finish6(Unquantize6(low - 1, sign), sign) < Finish6(Unquantize6(low, sign), sign) - magnitude) --low;
            return negative ? -low : low;
        }
        inline void Bc6(const Pixels &p, uint8_t *out, bool sign) {
            auto endpoints{Endpoints(p, 3)};
            for (auto &v : endpoints) for (unsigned c{}; c < 3; ++c) v[c] = Quantize6(v[c], sign);
            std::array<Pixel, 16> palette{};
            for (unsigned i{}; i < 16; ++i) for (unsigned c{}; c < 3; ++c) {
                int value{((64 - Weights[i]) * Unquantize6(endpoints[0][c], sign) + Weights[i] * Unquantize6(endpoints[1][c], sign) + 32) >> 6};
                palette[i][c] = Finish6(value, sign);
            }
            auto indices{Indices(p, palette, 3)};
            if (indices[0] >= 8) { std::swap(endpoints[0], endpoints[1]); for (auto &i : indices) i = 15 - i; }
            Bits bits{out}; bits.Put(3, 5); // BC6H single-subset, independent 10-bit endpoints.
            for (auto &v : endpoints) for (unsigned c{}; c < 3; ++c) bits.Put(static_cast<unsigned>(v[c]) & 1023, 10);
            for (unsigned i{}; i < 16; ++i) bits.Put(indices[i], i == 0 ? 3 : 4);
        }
        inline void Encode(Format f, const uint8_t *raw, uint8_t *out) {
            Pixels pixels{}; auto bpp{PixelBytes(f)};
            for (unsigned i{}; i < 16; ++i) {
                if (f == Format::Bc6 || f == Format::Bc6Signed) {
                    for (unsigned c{}; c < 3; ++c) {
                        unsigned half{unsigned{raw[i * 8 + c * 2]} | (unsigned{raw[i * 8 + c * 2 + 1]} << 8)};
                        int magnitude{static_cast<int>(std::min(half & 32767U, 0x7bffU))}; // Saturate non-finite HDR inputs.
                        pixels[i][c] = half & 32768 ? (f == Format::Bc6Signed ? -magnitude : 0) : magnitude;
                    }
                } else {
                    for (size_t c{}; c < bpp; ++c) {
                        int value{raw[i * bpp + c]};
                        if (f == Format::Bc4Signed || f == Format::Bc5Signed) value = std::max(value < 128 ? value : value - 256, -127);
                        pixels[i][c] = value;
                    }
                }
            }
            std::memset(out, 0, BlockBytes(f));
            switch (f) {
                case Format::Bc1: Color(pixels, out, true); break;
                case Format::Bc2: {
                    uint64_t alpha{};
                    for (unsigned i{}; i < 16; ++i) alpha |= uint64_t((pixels[i][3] + 8) / 17) << (4 * i);
                    Store(out, alpha, 8); Color(pixels, out + 8, false); break;
                }
                case Format::Bc3: Channel(pixels, out, 3); Color(pixels, out + 8, false); break;
                case Format::Bc4: case Format::Bc4Signed: Channel(pixels, out, 0); break;
                case Format::Bc5: case Format::Bc5Signed: Channel(pixels, out, 0); Channel(pixels, out + 8, 1); break;
                case Format::Bc6: case Format::Bc6Signed: Bc6(pixels, out, f == Format::Bc6Signed); break;
                case Format::Bc7: Bc7(pixels, out); break;
            }
        }
    }

    // `blocks` initially contains the guest's original compressed bytes. Host
    // pixels are tightly packed; depth slices are independent images, not rows.
    inline size_t Recompress(Format f, std::span<const uint8_t> host, std::span<uint8_t> blocks,
                             size_t width, size_t height, size_t depth = 1) {
        if (!width || !height || !depth || host.size() != DecodedSize(f, width, height, depth) || blocks.size() != EncodedSize(f, width, height, depth))
            throw std::invalid_argument("Invalid BC readback extent or buffer size");
        const auto bpp{PixelBytes(f)}, blockSize{BlockBytes(f)}, row{Product(width, bpp)};
        size_t offset{}, changed{};
        for (size_t z{}; z < depth; ++z) for (size_t y{}; y < height; y += 4) for (size_t x{}; x < width; x += 4) {
            alignas(8) std::array<uint8_t, 16> encoded{};
            alignas(8) std::array<uint8_t, 128> before{}, after{};
            std::memcpy(encoded.data(), blocks.data() + offset, blockSize);
            DecodeBlock(f, encoded.data(), before.data());
            bool equal{true};
            for (size_t dy{}; dy < 4; ++dy) for (size_t dx{}; dx < 4; ++dx) {
                const auto *source{host.data() + ((z * height + std::min(y + dy, height - 1)) * row) + std::min(x + dx, width - 1) * bpp};
                auto pos{(dy * 4 + dx) * bpp};
                std::memcpy(after.data() + pos, source, bpp);
                if (x + dx < width && y + dy < height && std::memcmp(before.data() + pos, source, bpp)) equal = false;
            }
            if (!equal) { detail::Encode(f, after.data(), encoded.data()); std::memcpy(blocks.data() + offset, encoded.data(), blockSize); ++changed; }
            offset += blockSize;
        }
        return changed;
    }
}
