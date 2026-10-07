#include "SceTypes.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

extern "C" {
int APS5_VABI sceVideoOutOpen(int userId, int busType, int index, const void* param);
int APS5_VABI sceVideoOutClose(int handle);
int APS5_VABI sceVideoOutVrrPegToFixedRate(int handle, uint64_t unknown1, uint64_t unknown2);
int APS5_VABI sceVideoOutVrrUnpegFromFixedRate(int handle);
}

static constexpr int SYSTEM_USER = 255;
static constexpr int MAIN_BUS = 0;
static constexpr int NEVER_OPENED_HANDLE = 2;

static void Require(bool value) { if (!value) std::abort(); }

template <typename Call>
static bool Rejects(Call call) {
    try {
        call();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

static bool RejectsHandle(int handle) {
    return Rejects([&] { sceVideoOutVrrPegToFixedRate(handle, 0, 0); }) &&
        Rejects([&] { sceVideoOutVrrUnpegFromFixedRate(handle); });
}

int main() {
    const int handle = sceVideoOutOpen(SYSTEM_USER, MAIN_BUS, 0, nullptr);
    Require(handle > 0);

    Require(sceVideoOutVrrPegToFixedRate(handle, 0, 0) == 0);
    Require(sceVideoOutVrrUnpegFromFixedRate(handle) == 0);
    Require(sceVideoOutVrrUnpegFromFixedRate(handle) == 0);
    Require(sceVideoOutVrrPegToFixedRate(handle, 0, 0) == 0);
    Require(Rejects([&] { sceVideoOutVrrPegToFixedRate(handle, 1, 0); }));
    Require(Rejects([&] { sceVideoOutVrrPegToFixedRate(handle, 0, 1); }));

    Require(RejectsHandle(0));
    Require(RejectsHandle(-1));
    Require(RejectsHandle(NEVER_OPENED_HANDLE));

    Require(sceVideoOutClose(handle) == 0);
    Require(RejectsHandle(handle));
    LibcRunShutdown_nid_postfix();
}
