#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <mutex>
#include <vector>

using namespace AgcDriver::Graphics;

int main() {
    try {
        // Buffers outlive the device cache, whose destruction can flush pending images.
        struct Allocations {
            std::vector<std::vector<std::byte>> bytes;
            std::vector<void*> registered;
            ~Allocations() { for (const auto pointer : registered) GuestAllocations::Mutation().Remove(pointer); }
        } allocations;
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        for (const auto samples : {2u, 4u, 8u}) {
            constexpr std::uint32_t width = 129, height = 131;
            const ColorTargetLayout sourceLayout(width, height, ColorTileMode::RenderTarget, 4, samples);
            const ColorTargetLayout destinationLayout(width, height, ColorTileMode::RenderTarget);
            const auto allocate = [&](std::size_t bytes) {
                allocations.bytes.emplace_back(bytes + 65536, std::byte{0x5a});
                const auto base = (reinterpret_cast<std::uintptr_t>(allocations.bytes.back().data()) + 65535u) & ~std::uintptr_t{65535u};
                GuestAllocations::Mutation().Add(reinterpret_cast<void*>(base), bytes, true, true, true);
                allocations.registered.push_back(reinterpret_cast<void*>(base));
                return std::span(reinterpret_cast<std::byte*>(base), bytes);
            };
            auto sourceBytes = allocate(sourceLayout.Bytes());
            auto destinationBytes = allocate(destinationLayout.Bytes());
            std::vector<std::byte> linear(sourceLayout.LinearBytes());
            for (std::uint32_t sample = 0; sample < samples; ++sample) {
                for (std::uint32_t y = 0; y < height; ++y) {
                    for (std::uint32_t x = 0; x < width; ++x) {
                        const auto index = ((sample * height + y) * width + x) * 4;
                        linear[index] = std::byte{static_cast<unsigned char>(sample * 16)};
                        linear[index + 1] = std::byte{static_cast<unsigned char>(x)};
                        linear[index + 2] = std::byte{static_cast<unsigned char>(y)};
                        linear[index + 3] = std::byte{255};
                    }
                }
            }
            sourceLayout.Tile(linear, sourceBytes);
            const std::vector<std::byte> original(sourceBytes.begin(), sourceBytes.end());
            ColorTarget source{};
            source.address = source.surfaceAddress = reinterpret_cast<std::uintptr_t>(sourceBytes.data());
            source.bytes = sourceBytes.size();
            source.extent = source.surfaceExtent = {width, height};
            source.format = VK_FORMAT_R8G8B8A8_UNORM;
            source.elementBytes = 4;
            source.samples = source.fragments = samples;
            source.tileMode = ColorTileMode::RenderTarget;
            auto destination = source;
            destination.address = destination.surfaceAddress = reinterpret_cast<std::uintptr_t>(destinationBytes.data());
            destination.bytes = destinationBytes.size();
            destination.samples = destination.fragments = 1;
            device->ColorMetadataPass({ColorMetadataPass::Mode::Resolve, {source, destination}});
            auto image = StorageTexture::ClassifyFill(source.address, source.bytes).image;
            Require(image != nullptr && image->Descriptor().samples == samples, "resolve lost its cached multisample source");
            image->MarkDirty();
            StorageTexture::FlushAllPending("multisample resolve test");
            device->WaitIdle();
            Require(std::equal(sourceBytes.begin(), sourceBytes.end(), original.begin()), "GPU sample roundtrip corrupted data or padding");
            std::vector<std::byte> resolved(destinationLayout.LinearBytes());
            destinationLayout.Detile(destinationBytes, resolved);
            for (std::uint32_t y = 0; y < height; ++y) {
                for (std::uint32_t x = 0; x < width; ++x) {
                    const auto index = (y * width + x) * 4;
                    Require(resolved[index] == std::byte{static_cast<unsigned char>((samples - 1) * 8)} && resolved[index + 1] == std::byte{static_cast<unsigned char>(x)} && resolved[index + 2] == std::byte{static_cast<unsigned char>(y)} && resolved[index + 3] == std::byte{255}, "native resolve did not average samples correctly");
                }
            }
        }
        std::cout << "Multisample transfer and resolve tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
