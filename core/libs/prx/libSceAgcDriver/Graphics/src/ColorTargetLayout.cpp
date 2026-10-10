#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureSwizzleEquations.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleSwizzleEquations.hpp"
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace AgcDriver::Graphics {
namespace {

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

std::uint32_t parity(std::uint32_t value) {
    return static_cast<std::uint32_t>(std::popcount(value)) & 1u;
}

std::uint32_t standardOffset(std::uint32_t x, std::uint32_t y, std::uint32_t elementBytes) {
    switch (elementBytes) {
        case 1u: return ((y << 4) & 0x1f0u) ^ ((y << 5) & 0x400u) ^ (x & 0x00fu) ^ ((x << 5) & 0x200u) ^ ((x << 6) & 0x800u);
        case 2u: return ((y << 4) & 0x070u) ^ ((y << 5) & 0x100u) ^ ((y << 6) & 0x400u) ^ ((x << 1) & 0x00eu) ^ ((x << 4) & 0x080u) ^ ((x << 5) & 0x200u) ^ ((x << 6) & 0x800u);
        case 4u: return ((y << 4) & 0x070u) ^ ((y << 5) & 0x100u) ^ ((y << 6) & 0x400u) ^ ((x << 2) & 0x00cu) ^ ((x << 5) & 0x080u) ^ ((x << 6) & 0x200u) ^ ((x << 7) & 0x800u);
        case 8u: return ((y << 4) & 0x030u) ^ ((y << 6) & 0x100u) ^ ((y << 7) & 0x400u) ^ ((x << 3) & 0x008u) ^ ((x << 5) & 0x0c0u) ^ ((x << 6) & 0x200u) ^ ((x << 7) & 0x800u);
        default: return ((y << 4) & 0x030u) ^ ((y << 6) & 0x100u) ^ ((y << 7) & 0x400u) ^ ((x << 6) & 0x0c0u) ^ ((x << 7) & 0x200u) ^ ((x << 8) & 0x800u);
    }
}

std::uint32_t standard64Extra(std::uint32_t x, std::uint32_t y, std::uint32_t elementBytes) {
    switch (elementBytes) {
        case 1u: return ((x << 7) & 0x2000u) ^ ((x << 8) & 0x8000u) ^ ((y << 6) & 0x1000u) ^ ((y << 7) & 0x4000u);
        case 2u: return ((x << 7) & 0x2000u) ^ ((x << 8) & 0x8000u) ^ ((y << 7) & 0x1000u) ^ ((y << 8) & 0x4000u);
        case 4u: return ((x << 8) & 0x2000u) ^ ((x << 9) & 0x8000u) ^ ((y << 7) & 0x1000u) ^ ((y << 8) & 0x4000u);
        case 8u: return ((x << 8) & 0x2000u) ^ ((x << 9) & 0x8000u) ^ ((y << 8) & 0x1000u) ^ ((y << 9) & 0x4000u);
        default: return ((x << 9) & 0x2000u) ^ ((x << 10) & 0x8000u) ^ ((y << 8) & 0x1000u) ^ ((y << 9) & 0x4000u);
    }
}

}

ColorTileMode DecodeColorTileMode(std::uint32_t attrib3) {
    const auto resourceType = (attrib3 >> 24u) & 3u;
    require((attrib3 & 0x80002000u) == 0 && ((resourceType == 0 && (attrib3 & 0x1fffu) == 0) || resourceType == 1 || resourceType == 2) && ((attrib3 >> 27u) & 7u) == 1, "AGC graphics: unsupported color depth, dimension, resource level or metadata mode");
    const auto mode = (attrib3 >> 14u) & 0x1fu;
    const auto fmaskMode = (attrib3 >> 19u) & 0x1fu;
    require(mode == 0 || mode == 5 || mode == 9 || mode == 0x15 || mode == 0x16 || mode == 0x1b, "AGC graphics: unsupported color tile mode");
    require(fmaskMode == 0 || fmaskMode == 0x18 || fmaskMode == (mode & ~3u), "AGC graphics: unsupported color FMASK swizzle mode");
    require(resourceType != 0 || mode == 0 || mode == 0x1b, "AGC graphics: 1D color targets with a standard swizzle mode are invalid");
    return static_cast<ColorTileMode>(mode);
}

namespace {

struct SwizzleTables {
    std::vector<std::uint32_t> x;
    std::vector<std::uint32_t> y;
    std::vector<std::uint32_t> samples;
};

const SwizzleTables& multisampleTables(std::uint32_t bytesPerElement, std::uint32_t samples) {
    // XOR is separable by coordinate. Build the small periodic tables once rather than evaluating
    // sixteen parity equations for every sample in every CPU layout transfer.
    static std::once_flag once[3][5];
    static SwizzleTables tables[3][5];
    const auto sampleIndex = std::countr_zero(samples) - 1u;
    const auto elementIndex = std::countr_zero(bytesPerElement);
    std::call_once(once[sampleIndex][elementIndex], [&] {
        const MultisampleSwizzleEquation* equation = nullptr;
        for (const auto& candidate : MultisampleSwizzleEquations) {
            if (candidate.samples == samples && candidate.elementBytes == bytesPerElement) equation = &candidate;
        }
        require(equation != nullptr, "AGC graphics: missing multisample swizzle equation");
        std::uint64_t used = 0;
        for (const auto mask : equation->bits) used |= mask;
        auto& table = tables[sampleIndex][elementIndex];
        table.x.resize(1u << std::bit_width(static_cast<std::uint32_t>(used & 0xffffu)));
        table.y.resize(1u << std::bit_width(static_cast<std::uint32_t>((used >> 16u) & 0xffffu)));
        table.samples.resize(samples);
        const auto fill = [&](auto& offsets, std::uint32_t shift) {
            for (std::uint32_t coordinate = 0; coordinate < offsets.size(); ++coordinate) {
                const auto packed = static_cast<std::uint64_t>(coordinate) << shift;
                for (std::uint32_t bit = 0; bit < 16u; ++bit) offsets[coordinate] |= (static_cast<std::uint32_t>(std::popcount(packed & equation->bits[bit])) & 1u) << bit;
            }
        };
        fill(table.x, 0);
        fill(table.y, 16);
        fill(table.samples, 48);
    });
    return tables[sampleIndex][elementIndex];
}

// Single-sample XOR swizzles (S4KBX, D4KBX and SW_64KB_R_X), one table pair per mode and element size.
const SwizzleTables& equationTables(ColorTileMode mode, std::uint32_t bytesPerElement, std::uint32_t blockWidth, std::uint32_t blockHeight) {
    static std::once_flag once[3][5];
    static SwizzleTables tables[3][5];
    const auto modeIndex = mode == ColorTileMode::S4KBX ? 0u : mode == ColorTileMode::D4KBX ? 1u : 2u;
    const auto index = static_cast<std::size_t>(std::countr_zero(bytesPerElement));
    std::call_once(once[modeIndex][index], [&] {
        const auto* equation = FindTextureSwizzleEquation(static_cast<std::uint32_t>(mode), bytesPerElement);
        require(equation != nullptr, "AGC graphics: no XOR swizzle equation for the color tile mode and element size");
        auto& table = tables[modeIndex][index];
        table.x.resize(blockWidth);
        table.y.resize(blockHeight);
        for (std::uint32_t x = 0; x < blockWidth; ++x) {
            std::uint32_t offset = 0;
            for (std::uint32_t bit = 0; bit < 16u; ++bit) offset |= parity(x & equation->bits[bit] & 0xfffu) << bit;
            table.x[x] = offset;
        }
        for (std::uint32_t y = 0; y < blockHeight; ++y) {
            std::uint32_t offset = 0;
            for (std::uint32_t bit = 0; bit < 16u; ++bit) offset |= parity((y << 12u) & equation->bits[bit] & 0xfff000u) << bit;
            table.y[y] = offset;
        }
    });
    return tables[modeIndex][index];
}

const SwizzleTables& standardTables(std::uint32_t bytesPerElement, std::uint32_t blockWidth, std::uint32_t blockHeight, bool block64KB) {
    static std::once_flag once[2][5];
    static SwizzleTables tables[2][5];
    const auto index = static_cast<std::size_t>(std::countr_zero(bytesPerElement));
    std::call_once(once[block64KB][index], [&] {
        auto& table = tables[block64KB][index];
        table.x.resize(blockWidth);
        table.y.resize(blockHeight);
        for (std::uint32_t x = 0; x < blockWidth; ++x) table.x[x] = standardOffset(x, 0, bytesPerElement) ^ (block64KB ? standard64Extra(x, 0, bytesPerElement) : 0u);
        for (std::uint32_t y = 0; y < blockHeight; ++y) table.y[y] = standardOffset(0, y, bytesPerElement) ^ (block64KB ? standard64Extra(0, y, bytesPerElement) : 0u);
    });
    return tables[block64KB][index];
}

}

ColorTargetLayout::ColorTargetLayout(std::uint32_t width, std::uint32_t height, ColorTileMode mode, std::uint32_t bytesPerElement, std::uint32_t samples, std::uint32_t pipeBankXor) : width(width), height(height), pitch(width), mode(mode), bytes(0), elementBytes(bytesPerElement), samples(samples), pipeBankXor(pipeBankXor) {
    require(width != 0 && height != 0 && width <= 16384 && height <= 16384, "AGC graphics: invalid color surface extent");
    require(std::has_single_bit(bytesPerElement) && bytesPerElement <= 16u, "AGC graphics: unsupported color element size");
    require(std::has_single_bit(samples) && samples <= 8u, "AGC graphics: unsupported stored color sample count");
    require(samples == 1 || mode == ColorTileMode::RenderTarget, "AGC graphics: multisampled color requires SW_64KB_R_X");
    require(pipeBankXor == 0 || (ColorTileModeIsXor(mode) && pipeBankXor < ColorTileModeBlockBytes(mode) && pipeBankXor % 256u == 0), "AGC graphics: a color pipe/bank XOR applies only to whole 256-byte units of XOR swizzle blocks");
    std::uint32_t paddedHeight = height;
    switch (mode) {
        case ColorTileMode::Linear: {
            const auto pitchAlignment = 256u / bytesPerElement;
            pitch = (width + pitchAlignment - 1u) / pitchAlignment * pitchAlignment;
            break;
        }
        case ColorTileMode::S4KBX:
        case ColorTileMode::D4KBX:
        case ColorTileMode::RenderTarget: {
            // XOR blocks hold 2^(log2(block bytes) - log2(bpe) - log2(samples)) pixels: 64 KiB for
            // SW_64KB_R_X, 4 KiB for S4KBX/D4KBX. Every stored sample lives in the same block, and
            // AMD's odd sample exponent changes which side gets the extra coordinate bit (RGBA8 2x
            // is 64x128, not a wider 1x texel). Single-sample blocks are wider than tall for odd powers.
            const auto log2Samples = static_cast<std::uint32_t>(std::countr_zero(samples));
            const auto log2Elements = (mode == ColorTileMode::RenderTarget ? 16u : 12u) - static_cast<std::uint32_t>(std::countr_zero(bytesPerElement)) - log2Samples;
            const auto log2Width = (log2Elements + ((log2Samples & 1u) == 0 ? 1u : 0u)) / 2u;
            blockWidth = 1u << log2Width;
            blockHeight = 1u << (log2Elements - log2Width);
            pitch = (width + blockWidth - 1u) / blockWidth * blockWidth;
            paddedHeight = (height + blockHeight - 1u) / blockHeight * blockHeight;
            if (samples == 1) {
                const auto& tables = equationTables(mode, bytesPerElement, blockWidth, blockHeight);
                xOffsets = tables.x.data();
                yOffsets = tables.y.data();
            } else {
                const auto& tables = multisampleTables(bytesPerElement, samples);
                xOffsets = tables.x.data();
                yOffsets = tables.y.data();
                sampleOffsets = tables.samples.data();
                xPeriod = static_cast<std::uint32_t>(tables.x.size());
                yPeriod = static_cast<std::uint32_t>(tables.y.size());
            }
            break;
        }
        case ColorTileMode::Standard4KB:
        case ColorTileMode::Standard64KB: {
            const auto log2Bytes = static_cast<std::uint32_t>(std::countr_zero(bytesPerElement));
            const bool block64KB = mode == ColorTileMode::Standard64KB;
            const auto log2Side = block64KB ? 8u : 6u;
            blockWidth = 1u << (log2Side - log2Bytes / 2u);
            blockHeight = 1u << (log2Side - (log2Bytes + 1u) / 2u);
            pitch = (width + blockWidth - 1u) / blockWidth * blockWidth;
            paddedHeight = (height + blockHeight - 1u) / blockHeight * blockHeight;
            const auto& tables = standardTables(bytesPerElement, blockWidth, blockHeight, block64KB);
            xOffsets = tables.x.data();
            yOffsets = tables.y.data();
            break;
        }
        default: throw std::runtime_error("AGC graphics: unsupported color tile mode");
    }
    const auto size = static_cast<std::uint64_t>(pitch) * paddedHeight * bytesPerElement * samples;
    require(size <= std::numeric_limits<std::size_t>::max(), "AGC graphics: color surface size overflow");
    bytes = static_cast<std::size_t>(size);
}

std::size_t ColorTargetLayout::offset(std::uint32_t x, std::uint32_t y, std::uint32_t sample) const {
    if (mode == ColorTileMode::Linear) return (static_cast<std::size_t>(y) * pitch + x) * elementBytes;
    const auto block = static_cast<std::size_t>(y / blockHeight) * (pitch / blockWidth) + x / blockWidth;
    if (sampleOffsets) {
        // Coordinates above the block dimensions participate in the pipe XOR.
        return block * Alignment() + (xOffsets[x & (xPeriod - 1u)] ^ yOffsets[y & (yPeriod - 1u)] ^ sampleOffsets[sample] ^ pipeBankXor);
    }
    return block * Alignment() + (xOffsets[x % blockWidth] ^ yOffsets[y % blockHeight] ^ pipeBankXor);
}

std::size_t ColorTargetLayout::Offset(std::uint32_t x, std::uint32_t y, std::uint32_t sample) const {
    require(x < width && y < height && sample < samples, "AGC graphics: color surface coordinate out of range");
    return offset(x, y, sample);
}

void ColorTargetLayout::Detile(std::span<const std::byte> source, std::span<std::byte> destination) const {
    require(source.size() == Bytes() && destination.size() == LinearBytes(), "AGC graphics: color detile buffer size mismatch");
    for (std::uint32_t sample = 0; sample < samples; ++sample) {
      for (std::uint32_t y = 0; y < height; ++y) {
        auto* row = destination.data() + (static_cast<std::size_t>(sample) * height + y) * width * elementBytes;
        for (std::uint32_t x = 0; x < width; ++x) std::memcpy(row + static_cast<std::size_t>(x) * elementBytes, source.data() + offset(x, y, sample), elementBytes);
      }
    }
}

void ColorTargetLayout::Tile(std::span<const std::byte> source, std::span<std::byte> destination) const {
    require(source.size() == LinearBytes() && destination.size() == Bytes(), "AGC graphics: color tile buffer size mismatch");
    for (std::uint32_t sample = 0; sample < samples; ++sample) {
      for (std::uint32_t y = 0; y < height; ++y) {
        const auto* row = source.data() + (static_cast<std::size_t>(sample) * height + y) * width * elementBytes;
        for (std::uint32_t x = 0; x < width; ++x) std::memcpy(destination.data() + offset(x, y, sample), row + static_cast<std::size_t>(x) * elementBytes, elementBytes);
      }
    }
}

CmaskLayout::CmaskLayout(std::uint32_t width, std::uint32_t height) : width(width), height(height), blocksPerRow((width + 1023u) / 1024u), bytes(0) {
    require(width != 0 && height != 0 && width <= 16384 && height <= 16384, "AGC graphics: invalid CMASK surface extent");
    bytes = static_cast<std::size_t>(blocksPerRow) * ((height + 511u) / 512u) * Alignment;
}

std::size_t CmaskLayout::Nibble(std::uint32_t tileX, std::uint32_t tileY) const {
    require(tileX < TilesX() && tileY < TilesY(), "AGC graphics: CMASK tile out of range");
    static constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 13> equation{{
        {0x008u, 0x000u}, {0x000u, 0x010u}, {0x040u, 0x000u}, {0x000u, 0x040u}, {0x080u, 0x000u}, {0x000u, 0x080u}, {0x100u, 0x000u},
        {0x000u, 0x100u}, {0x200u, 0x000u}, {0x008u, 0x008u}, {0x010u, 0x010u}, {0x040u, 0x020u}, {0x020u, 0x040u},
    }};
    const auto x = tileX * 8u;
    const auto y = tileY * 8u;
    std::size_t offset = 0;
    for (std::size_t bit = 0; bit < equation.size(); ++bit) offset |= static_cast<std::size_t>(parity((x & equation[bit].first) ^ (y & equation[bit].second))) << bit;
    const auto block = static_cast<std::size_t>(y / 512u) * blocksPerRow + x / 1024u;
    return block * Alignment * 2u + offset;
}

}
