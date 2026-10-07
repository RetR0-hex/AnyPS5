#include "prx/libc/include/general/VabiMacros.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <io.h>

extern "C" {
int APS5_VABI sceKernelOpen(const char*, int, std::uint16_t);
int APS5_VABI sceKernelClose(int);
}

static constexpr int SCE_KERNEL_ERROR_EBADF = static_cast<int>(0x80020009);

static void Require(bool value) { if (!value) std::abort(); }

int main() {
    // A program started without standard handles has 0-2 free; the game must not get them.
    for (int fd = 0; fd <= 2; ++fd) _close(fd);
    const auto path = std::filesystem::temp_directory_path() / "anyps5-standard-descriptors.txt";
    std::ofstream(path) << "data";
    const int fd = sceKernelOpen(path.string().c_str(), 0, 0);
    Require(fd > 2);
    Require(sceKernelClose(fd) == 0);
    Require(sceKernelClose(fd) == SCE_KERNEL_ERROR_EBADF);  // closing twice is an error, not a crash
    std::filesystem::remove(path);
}
