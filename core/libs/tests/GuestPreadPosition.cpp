#include "prx/libc/include/general/VabiMacros.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

extern "C" {
int APS5_VABI sceKernelOpen(const char*, int, std::uint16_t);
int APS5_VABI sceKernelClose(int);
std::int64_t APS5_VABI sceKernelRead(int, void*, std::size_t);
std::int64_t APS5_VABI sceKernelPread(int, void*, std::size_t, std::int64_t);
std::int64_t APS5_VABI sceKernelLseek(int, std::int64_t, int);
}

static void Require(bool value) { if (!value) std::abort(); }

int main() {
    const auto path = std::filesystem::temp_directory_path() / "anyps5-pread-position.bin";
    std::ofstream(path, std::ios::binary) << "0123456789abcdef";
    const int fd = sceKernelOpen(path.string().c_str(), 0, 0);
    Require(fd > 2);

    char buffer[4] = {};
    Require(sceKernelRead(fd, buffer, 4) == 4 && std::memcmp(buffer, "0123", 4) == 0);
    // A positioned read in between must not move where the next plain read starts.
    Require(sceKernelPread(fd, buffer, 4, 12) == 4 && std::memcmp(buffer, "cdef", 4) == 0);
    Require(sceKernelLseek(fd, 0, 1) == 4);
    Require(sceKernelRead(fd, buffer, 4) == 4 && std::memcmp(buffer, "4567", 4) == 0);
    Require(sceKernelPread(fd, buffer, 4, 16) == 0);
    Require(sceKernelRead(fd, buffer, 4) == 4 && std::memcmp(buffer, "89ab", 4) == 0);

    Require(sceKernelClose(fd) == 0);
    std::filesystem::remove(path);
}
