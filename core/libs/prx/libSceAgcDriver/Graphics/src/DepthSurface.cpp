#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "RdnaDecoder/include/RdnaDecoder/RdnaDescriptorFormat.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace AgcDriver::Graphics {
namespace {

class DepthSurface {
public:
    DepthSurface(const Context& context, const DepthTarget& target) : context(context), target(target) {
        this->context.bufferPool.reset();
        VkFormatProperties properties{};
        context.formatProperties(context.physical, target.format, &properties);
        Require((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0, "depth/stencil format " + std::to_string(target.format) + " cannot be an attachment on this device");
        Require((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0, "depth/stencil format " + std::to_string(target.format) + " cannot be sampled on this device");
        Require(target.extent.width <= context.limits.maxFramebufferWidth && target.extent.height <= context.limits.maxFramebufferHeight, "depth target exceeds framebuffer limits");
        const VkImageAspectFlags aspects = VK_IMAGE_ASPECT_DEPTH_BIT | (target.stencilAddress != 0 ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
        try {
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = target.format;
            info.extent = {target.extent.width, target.extent.height, 1};
            info.mipLevels = 1;
            info.arrayLayers = 1;
            info.samples = static_cast<VkSampleCountFlagBits>(target.samples);
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            if (target.samples > 1) {
                Require(context.imageFormatProperties != nullptr, "missing multisample depth format query");
                VkImageFormatProperties supported{};
                Check(context.imageFormatProperties(context.physical, info.format, info.imageType, info.tiling, info.usage, 0, &supported), "vkGetPhysicalDeviceImageFormatProperties multisample depth");
                Require((supported.sampleCounts & info.samples) != 0, "depth sample count is unsupported by this device");
            }
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            Check(context.Function<PFN_vkCreateImage>("vkCreateImage")(context.device, &info, nullptr, &image), "vkCreateImage depth");
            VkMemoryRequirements requirements{};
            context.Function<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(context.device, image, &requirements);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            Check(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocation, nullptr, &memory), "vkAllocateMemory depth target");
            Check(context.Function<PFN_vkBindImageMemory>("vkBindImageMemory")(context.device, image, memory, 0), "vkBindImageMemory depth");
            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = target.format;
            viewInfo.subresourceRange = {aspects, 0, 1, 0, 1};
            Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &viewInfo, nullptr, &view), "vkCreateImageView depth");
            auto* recorder = Recorder::Active();
            std::unique_ptr<CommandBatch> batch;
            if (recorder == nullptr) batch = std::make_unique<CommandBatch>(context);
            const auto commands = recorder != nullptr ? recorder->Commands() : batch->Handle();
            VkImageMemoryBarrier toGeneral{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toGeneral.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toGeneral.image = image;
            toGeneral.subresourceRange = viewInfo.subresourceRange;
            const auto barrier = context.Resolved(&DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier");
            barrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toGeneral);
            const VkClearDepthStencilValue clear{target.clearDepth, target.clearStencil};
            context.Function<PFN_vkCmdClearDepthStencilImage>("vkCmdClearDepthStencilImage")(commands, image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &toGeneral.subresourceRange);
            RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
            if (batch) batch->SubmitAndWait();
            else Recorder::CountBarriers(Recorder::CommandClass::Draw, 2);
        } catch (...) {
            release();
            throw;
        }
    }
    ~DepthSurface() { release(); }
    DepthSurface(const DepthSurface&) = delete;
    DepthSurface& operator=(const DepthSurface&) = delete;

    std::shared_ptr<Texture> Sampled(std::span<const std::uint32_t> words, const GuestTextureResource& resource, VkComponentMapping components) {
        Require(target.samples == 1, "multisampled depth cannot be bound as a single-sample texture");
        std::array<std::uint32_t, 12> key{};
        std::copy_n(words.begin(), std::min<std::size_t>(words.size(), 8), key.begin());
        key[8] = components.r;
        key[9] = components.g;
        key[10] = components.b;
        key[11] = components.a;
        if (const auto found = textures.find(key); found != textures.end()) return found->second;
        const bool stencil = target.stencilAddress != 0 && resource.baseAddress == target.stencilAddress;
        const bool d16 = target.format == VK_FORMAT_D16_UNORM || target.format == VK_FORMAT_D16_UNORM_S8_UINT;
        const auto expected = stencil ? VK_FORMAT_R8_UINT : d16 ? VK_FORMAT_R16_UNORM : VK_FORMAT_R32_SFLOAT;
        const auto format = ResolveTextureFormat(resource.format);
        const bool depthBits = !stencil && words.size() >= 4 && ShaderRecompiler::DepthBitsTextureWidth(words[1], words[3]) == (d16 ? 16u : 32u);
        if ((format != expected && !depthBits) || resource.dimension != TextureDimension::k2D || resource.width != target.extent.width || resource.height != target.extent.height || resource.baseLevel != 0 || resource.lastLevel != 0 || resource.baseArray != 0) {
            char text[448];
            std::snprintf(text, sizeof(text), "AGC graphics: sampling the %s plane of depth surface 0x%llx (%ux%u, vk format %d) as a %ux%u texture of guest format %u (vk %d), tile mode %u, dimension %d, levels %u-%u, slice %u is not implemented (T# %08x %08x %08x %08x %08x %08x %08x %08x)",
                          stencil ? "stencil" : "depth", static_cast<unsigned long long>(target.address), target.extent.width, target.extent.height, static_cast<int>(target.format), resource.width, resource.height, resource.format, static_cast<int>(format),
                          static_cast<unsigned>(resource.tileMode), static_cast<int>(resource.dimension), resource.baseLevel, resource.lastLevel, resource.baseArray, key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7]);
            throw std::runtime_error(text);
        }
        auto texture = std::make_shared<Texture>(context, image, target.format, stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, components);
        textures.emplace(key, texture);
        return texture;
    }

    std::shared_ptr<StorageTexture> Storage(const GuestTextureResource& resource, std::uint32_t mip, std::span<const std::uint32_t> words) {
        // Depth/color transfers preserve the native 16-bit UNORM or 32-bit float payload through
        // a linear buffer; cross-aspect image copies are illegal even for equal texel sizes.
        Require(target.samples == 1 && resource.samples == 1, "multisampled depth storage access is not implemented");
        const bool d16 = target.format == VK_FORMAT_D16_UNORM || target.format == VK_FORMAT_D16_UNORM_S8_UINT;
        const bool d32 = target.format == VK_FORMAT_D32_SFLOAT || target.format == VK_FORMAT_D32_SFLOAT_S8_UINT;
        const bool stencil = target.stencilAddress != 0 && resource.baseAddress == target.stencilAddress;
        const auto format = ResolveTextureFormat(resource.format);
        const bool supported = stencil ? (d16 || d32) && format == VK_FORMAT_R8_UINT : resource.baseAddress == target.address && ((d16 && format == VK_FORMAT_R16_UNORM) || (d32 && (format == VK_FORMAT_R32_SFLOAT || format == VK_FORMAT_R32_UINT)));
        if (!supported) {
            char diagnostic[512];
            std::snprintf(diagnostic, sizeof(diagnostic), "AGC graphics: storage %s plane requires %s; depth address 0x%llx (native format %d), descriptor 0x%llx %ux%u guest format %u (vk %d) mip %u T# %08x %08x %08x %08x %08x %08x %08x %08x",
                          stencil ? "stencil" : "depth", stencil ? "R8_UINT" : d16 ? "D16 with R16_UNORM" : "D32 with R32 float or uint", static_cast<unsigned long long>(target.address), static_cast<int>(target.format), static_cast<unsigned long long>(resource.baseAddress), resource.width, resource.height, resource.format, static_cast<int>(format), mip,
                          words.size() > 0 ? words[0] : 0, words.size() > 1 ? words[1] : 0, words.size() > 2 ? words[2] : 0, words.size() > 3 ? words[3] : 0, words.size() > 4 ? words[4] : 0, words.size() > 5 ? words[5] : 0, words.size() > 6 ? words[6] : 0, words.size() > 7 ? words[7] : 0);
            throw std::runtime_error(diagnostic);
        }
        // The proxy mirrors exactly one native depth level, so any other view shape would read or
        // write the wrong texels. Report every field so the next unsupported shape is identifiable
        // from the game log alone.
        // A one-layer 2D array (baseArray = lastArray = 0) addresses the same texels as a plain 2D
        // view, and titles use both shapes on the same depth plane.
        const bool singleLevel2D = resource.dimension == TextureDimension::k2D || resource.dimension == TextureDimension::k2DArray;
        if (!(singleLevel2D && resource.width == target.extent.width && resource.height == target.extent.height && resource.baseArray == 0 && resource.depthOrLastArray == 0 && mip == 0 && resource.baseLevel == 0 && resource.lastLevel == 0 && resource.mipCount == 1 && resource.dccAddress == 0)) {
            char diagnostic[384];
            std::snprintf(diagnostic, sizeof(diagnostic), "AGC graphics: storage depth view must cover one complete 2D level (target %ux%u; view dim %d %ux%u baseArray %u lastArray %u mip %u baseLevel %u lastLevel %u mipCount %u dcc 0x%llx)",
                          target.extent.width, target.extent.height, static_cast<int>(resource.dimension), resource.width, resource.height, resource.baseArray, resource.depthOrLastArray, mip, resource.baseLevel, resource.lastLevel, resource.mipCount, static_cast<unsigned long long>(resource.dccAddress));
            throw std::runtime_error(diagnostic);
        }
        Require(context.detiler != nullptr, "storage depth access requires a texture detiler");
        auto& proxy = stencil ? stencilStorage : storage;
        if (proxy == nullptr) {
            // Float and integer depth-bit descriptors share one mutable image. Separate proxies
            // would carry competing pending writes when a shader alternates the two views.
            auto canonical = resource;
            canonical.format = stencil ? 5u : d16 ? 7u : 22u;
            // Always a one-layer 2D array: its full view serves image2DArray bindings and its
            // first-layer view serves image2D ones, so 2D and 2D-array descriptors share one proxy.
            // ShaderResources selects the first-layer view for this proxy whenever the shader is 2D.
            canonical.dimension = TextureDimension::k2DArray;
            proxy = std::make_shared<StorageTexture>(context, *context.detiler, canonical, 0, false);
            // One scratch allocation fits the larger depth plane. Barriers serialize its reuse
            // between independent stencil and depth copies without changing the other aspect.
            if (transfer == nullptr) transfer = std::make_shared<DeviceBuffer>(context, static_cast<std::size_t>(target.extent.width) * target.extent.height * sizeof(float), VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        }
        TransferStorage(proxy, false);
        // The proxy now owns actual GPU depth values rather than guest bytes. Its ordinary
        // pending-image path can retile those values when a CPU or a buffer reader needs them.
        proxy->MarkDirty();
        return proxy;
    }

    void TransferStorage(const std::shared_ptr<StorageTexture>& proxy, bool store) {
        auto* recorder = Recorder::Active();
        std::unique_ptr<CommandBatch> batch;
        if (recorder == nullptr) batch = std::make_unique<CommandBatch>(context);
        const auto commands = recorder != nullptr ? recorder->Commands() : batch->Handle();
        // Closing an open render pass through Commands() makes its depth writes available here.
        // The scratch buffer is shared by successive transfers, so include reads as well as writes
        // in the leading barrier to cover reuse after a previous buffer-to-image copy.
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy source{};
        const auto nativeAspect = proxy == stencilStorage ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        source.imageSubresource = {store ? VK_IMAGE_ASPECT_COLOR_BIT : nativeAspect, 0, 0, 1};
        source.imageExtent = {target.extent.width, target.extent.height, 1};
        context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands, store ? proxy->Image() : image, VK_IMAGE_LAYOUT_GENERAL, transfer->Handle(), 1, &source);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        auto destination = source;
        destination.imageSubresource.aspectMask = store ? nativeAspect : VK_IMAGE_ASPECT_COLOR_BIT;
        context.Function<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(commands, transfer->Handle(), store ? image : proxy->Image(), VK_IMAGE_LAYOUT_GENERAL, 1, &destination);
        RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        if (batch) batch->SubmitAndWait();
        else {
            recorder->Keep(proxy);
            recorder->Keep(transfer);
            Recorder::CountBarriers(Recorder::CommandClass::StorageUpload, 3);
        }
    }

    const Context context;
    const DepthTarget target;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::shared_ptr<StorageTexture> storage;
    std::shared_ptr<StorageTexture> stencilStorage;

private:
    std::map<std::array<std::uint32_t, 12>, std::shared_ptr<Texture>> textures;
    std::shared_ptr<DeviceBuffer> transfer;

    void release() noexcept {
        storage.reset();
        stencilStorage.reset();
        transfer.reset();
        textures.clear();
        if (view) context.Function<PFN_vkDestroyImageView>("vkDestroyImageView")(context.device, view, nullptr);
        if (image) context.Function<PFN_vkDestroyImage>("vkDestroyImage")(context.device, image, nullptr);
        if (memory) context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
        view = VK_NULL_HANDLE;
        image = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
    }
};

bool sameSurface(const DepthTarget& a, const DepthTarget& b) {
    // Reused guest addresses can change AA mode; attaching the old sample count invalidates the pass.
    return a.address == b.address && a.stencilAddress == b.stencilAddress && a.extent.width == b.extent.width && a.extent.height == b.extent.height && a.format == b.format && a.samples == b.samples;
}

std::mutex& surfacesMutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<std::unique_ptr<DepthSurface>>& surfaces() {
    static auto* list = new std::vector<std::unique_ptr<DepthSurface>>();
    return *list;
}

}

std::uint64_t DepthSliceBytes(VkExtent2D extent, std::uint32_t bytesPerTexel) {
    const std::uint32_t blockWidth = bytesPerTexel == 4 ? 128u : 256u;
    const std::uint32_t blockHeight = bytesPerTexel == 1 ? 256u : 128u;
    const auto width = static_cast<std::uint64_t>((extent.width + blockWidth - 1) / blockWidth * blockWidth);
    const auto height = static_cast<std::uint64_t>((extent.height + blockHeight - 1) / blockHeight * blockHeight);
    return width * height * bytesPerTexel;
}

VkImageView DepthSurfaceView(const Context& context, const DepthTarget& target) {
    std::lock_guard lock(surfacesMutex());
    auto& list = surfaces();
    for (auto it = list.begin(); it != list.end(); ++it) {
        if ((*it)->context.device == context.device && sameSurface((*it)->target, target)) {
            const auto view = (*it)->view;
            // Keep older allocations alive for queued commands, but reverse descriptor lookup
            // must find the most recently attached geometry when an address alternates sizes.
            std::rotate(it, std::next(it), list.end());
            return view;
        }
    }
    surfaces().push_back(std::make_unique<DepthSurface>(context, target));
    return surfaces().back()->view;
}

void ClearDepthSurfaces(VkDevice device) {
    std::lock_guard lock(surfacesMutex());
    std::erase_if(surfaces(), [&](const auto& surface) { return surface->context.device == device; });
}

std::shared_ptr<Texture> DepthSurfaceTexture(const Context& context, std::span<const std::uint32_t> words, const GuestTextureResource& resource, VkComponentMapping components) {
    // Queued work can retain an old depth image after its allocation becomes color storage.
    // Only Z-layout descriptors may sample that native depth image; other layouts use the color cache.
    if (resource.tileMode != TextureTileMode::kZ64KBX) return nullptr;
    std::lock_guard lock(surfacesMutex());
    const auto& list = surfaces();
    const auto found = std::find_if(list.rbegin(), list.rend(), [&](const auto& surface) {
        return surface->context.device == context.device && (surface->target.address == resource.baseAddress || (surface->target.stencilAddress != 0 && surface->target.stencilAddress == resource.baseAddress));
    });
    return found == list.rend() ? nullptr : (*found)->Sampled(words, resource, components);
}

bool DepthSurfaceAt(const Context& context, const GuestTextureResource& resource) {
    // Native depth images represent Z-swizzled storage. A transient allocation reused with
    // an R/S/D color layout must reach the normal storage cache even if an old depth entry lives.
    if (resource.tileMode != TextureTileMode::kZ64KBX) return false;
    std::lock_guard lock(surfacesMutex());
    return std::any_of(surfaces().begin(), surfaces().end(), [&](const auto& surface) {
        return surface->context.device == context.device && (surface->target.address == resource.baseAddress || (surface->target.stencilAddress != 0 && surface->target.stencilAddress == resource.baseAddress));
    });
}

std::shared_ptr<StorageTexture> DepthSurfaceStorage(const Context& context, const GuestTextureResource& resource, std::uint32_t mip, std::span<const std::uint32_t> words) {
    if (resource.tileMode != TextureTileMode::kZ64KBX) return nullptr;
    std::lock_guard lock(surfacesMutex());
    // Titles alias transient allocations: a compute pass can run at another resolution over memory
    // that last held a differently sized depth target. Only a surface of the view's own extent
    // owns those texels on the GPU; otherwise the guest bytes are authoritative and the ordinary
    // storage cache (which detiles Z layouts) serves the view.
    bool aliased = false;
    for (auto it = surfaces().rbegin(); it != surfaces().rend(); ++it) {
        const auto& surface = *it;
        if (surface->context.device != context.device || !(surface->target.address == resource.baseAddress || (surface->target.stencilAddress != 0 && surface->target.stencilAddress == resource.baseAddress))) continue;
        if (surface->target.extent.width == resource.width && surface->target.extent.height == resource.height) return surface->Storage(resource, mip, words);
        aliased = true;
    }
    if (aliased) std::fprintf(stderr, "[depth] storage view 0x%llx %ux%u differs from every depth target there; using guest memory\n", static_cast<unsigned long long>(resource.baseAddress), resource.width, resource.height);
    return nullptr;
}

bool IsDepthSurfaceStorage(const Context& context, const StorageTexture* storage) {
    if (storage == nullptr || storage->Descriptor().tileMode != TextureTileMode::kZ64KBX) return false;
    std::lock_guard lock(surfacesMutex());
    return storage != nullptr && std::any_of(surfaces().begin(), surfaces().end(), [&](const auto& surface) { return surface->context.device == context.device && (surface->storage.get() == storage || surface->stencilStorage.get() == storage); });
}

void StoreDepthSurfaceStorage(const Context& context, const StorageTexture* storage) {
    if (storage == nullptr || storage->Descriptor().tileMode != TextureTileMode::kZ64KBX) return;
    std::lock_guard lock(surfacesMutex());
    for (const auto& surface : surfaces()) {
        if (storage != nullptr && surface->context.device == context.device && (surface->storage.get() == storage || surface->stencilStorage.get() == storage)) {
            surface->TransferStorage(surface->storage.get() == storage ? surface->storage : surface->stencilStorage, true);
            return;
        }
    }
}

}
