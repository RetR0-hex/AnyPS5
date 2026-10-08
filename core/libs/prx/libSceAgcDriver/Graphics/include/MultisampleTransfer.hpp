#pragma once
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"

namespace AgcDriver::Graphics {
class Buffer;
// Transfers RGBA8 stored samples without resolving them. Call only at synchronization points;
// resident draws keep their native image and do not invoke this transfer per draw.
void TransferMultisampleColor(const Context& context, VkImage image, VkImageView view,
                             Buffer& guest, std::uint32_t width, std::uint32_t height,
                             std::uint32_t samples, bool toImage, bool initialized);
}
