#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/KernelErrors.hpp"
#include "prx/libkernel/Pthread/include/Pthread.hpp"
#include <array>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {

constexpr std::array<int, 6> AllowedSignals{1, 4, 8, 10, 11, 30};

std::mutex handlersLock;
std::array<void*, 32> handlers{};

bool Allowed(int signum) {
    for (const int allowed : AllowedSignals)
        if (allowed == signum) return true;
    return false;
}

#ifdef _WIN32
using ExceptionHandler = void (APS5_VABI*)(int, void*);

// The handler's second argument is a ucontext: the signal mask (16 bytes) and 48
// reserved bytes, then a FreeBSD amd64 mcontext (Unity reads mc_rsp at 0xf8).
constexpr std::size_t McontextOffset = 0x40;
constexpr std::size_t McontextSize = 0x320;
constexpr std::size_t UcontextSize = 0x500;

struct Ucontext {
    alignas(16) std::uint8_t bytes[UcontextSize] = {};

    explicit Ucontext(const CONTEXT& context) {
        const std::pair<std::size_t, std::uint64_t> registers[] = {
            {0x08, context.Rdi}, {0x10, context.Rsi}, {0x18, context.Rdx}, {0x20, context.Rcx},
            {0x28, context.R8}, {0x30, context.R9}, {0x38, context.Rax}, {0x40, context.Rbx},
            {0x48, context.Rbp}, {0x50, context.R10}, {0x58, context.R11}, {0x60, context.R12},
            {0x68, context.R13}, {0x70, context.R14}, {0x78, context.R15}, {0xa0, context.Rip},
            {0xb0, context.EFlags}, {0xb8, context.Rsp}, {0xc8, McontextSize},
        };
        for (const auto& [field, value] : registers) std::memcpy(bytes + McontextOffset + field, &value, sizeof(value));
    }
};

// Windows cannot interrupt a thread blocked in a wait, so a raised exception is
// emulated: the target is suspended where it is, and its handler runs on a helper
// thread that answers to scePthreadSelf as the target and sees the target's
// registers. The target resumes when the handler returns, as after a signal.
void DeliverToSuspended(Pthread thread, ExceptionHandler handler, int signum) {
    const auto native = static_cast<HANDLE>(thread->nativeHandle);
    if (SuspendThread(native) == static_cast<DWORD>(-1))
        throw std::runtime_error("sceKernelRaiseException: SuspendThread failed: " + std::to_string(GetLastError()));
    CONTEXT context{};
    context.ContextFlags = CONTEXT_INTEGER | CONTEXT_CONTROL;
    if (!GetThreadContext(native, &context)) {
        const auto error = GetLastError();
        ResumeThread(native);
        throw std::runtime_error("sceKernelRaiseException: GetThreadContext failed: " + std::to_string(error));
    }
    std::thread([thread, native, handler, signum, ucontext = Ucontext(context)]() mutable {
        ExchangeCurrentGuestThread(thread);
        handler(signum, ucontext.bytes);
        ExchangeCurrentGuestThread(nullptr);
        ResumeThread(native);
    }).detach();
}
#endif

}

extern "C" {

int APS5_VABI sceKernelInstallExceptionHandler(int signum, void* handler) {
 if (!Allowed(signum) || handler == nullptr) return SCE_KERNEL_ERROR_EINVAL;
 std::lock_guard lock(handlersLock);
 if (handlers[signum] != nullptr) return SCE_KERNEL_ERROR_EAGAIN;
 handlers[signum] = handler;
 return 0;
}

int APS5_VABI sceKernelRemoveExceptionHandler(int signum) {
 if (!Allowed(signum)) return SCE_KERNEL_ERROR_EINVAL;
 std::lock_guard lock(handlersLock);
 handlers[signum] = nullptr;
 return 0;
}

int APS5_VABI sceKernelRaiseException(Pthread thread, int signum) {
 if (!Allowed(signum)) return SCE_KERNEL_ERROR_EINVAL;
 if (thread == nullptr) return SCE_KERNEL_ERROR_ESRCH;
 void* handler = nullptr;
 {
  std::lock_guard lock(handlersLock);
  handler = handlers[signum];
 }
 if (handler == nullptr) throw std::runtime_error("sceKernelRaiseException: no handler for signal " + std::to_string(signum));
#ifdef _WIN32
 const auto run = reinterpret_cast<ExceptionHandler>(handler);
 if (thread->threadId == std::this_thread::get_id()) {
  CONTEXT context{};
  RtlCaptureContext(&context);
  Ucontext ucontext(context);
  run(signum, ucontext.bytes);
  return 0;
 }
 DeliverToSuspended(thread, run, signum);
 return 0;
#else
 NotImplemented_nid_no_patch(__func__);
 return 0;
#endif
}

void APS5_VABI sceKernelDebugRaiseException(int c1, int c2) {
  APS5_LOG_OUT("sceKernelDebugRaiseException c1=%d c2=%d", c1, c2);
}

void APS5_VABI sceKernelDebugRaiseExceptionOnReleaseMode(int c1, int c2) {
  APS5_LOG_OUT("sceKernelDebugRaiseExceptionOnReleaseMode c1=%d c2=%d", c1, c2);
}

}
