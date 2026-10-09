#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DEPTHSURFACE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DEPTHSURFACE_HPP

#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include <cstdint>
#include <memory>
#include <span>

namespace AgcDriver::Graphics {

class Texture;
class StorageTexture;

VkImageView DepthSurfaceView(const Context& context, const DepthTarget& target);
std::uint64_t DepthSliceBytes(VkExtent2D extent, std::uint32_t bytesPerTexel);
void ClearDepthSurfaces(VkDevice device);
// An address reused with a color layout no longer names the cached native depth image.
bool DepthSurfaceAt(const Context& context, const GuestTextureResource& resource);
std::shared_ptr<Texture> DepthSurfaceTexture(const Context& context, std::span<const std::uint32_t> words, const GuestTextureResource& resource, VkComponentMapping components);
// Vulkan depth/stencil formats cannot be storage images. R16/R32 depth and R8 stencil proxies
// are loaded before shader use and written back after writable use, entirely on the GPU.
std::shared_ptr<StorageTexture> DepthSurfaceStorage(const Context& context, const GuestTextureResource& resource, std::uint32_t mip, std::span<const std::uint32_t> words = {});
bool IsDepthSurfaceStorage(const Context& context, const StorageTexture* storage);
void StoreDepthSurfaceStorage(const Context& context, const StorageTexture* storage);

}

#endif
