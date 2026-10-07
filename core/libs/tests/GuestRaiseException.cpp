#include "SceTypes.hpp"
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <windows.h>

extern "C" {
int APS5_VABI sceKernelInstallExceptionHandler(int signum, void* handler);
int APS5_VABI sceKernelRemoveExceptionHandler(int signum);
int APS5_VABI sceKernelRaiseException(Pthread thread, int signum);
int APS5_VABI scePthreadCreate(Pthread* thread, const PthreadAttr* attr, PthreadEntry entry, void* arg, const char* name);
int APS5_VABI scePthreadJoin(Pthread thread, void** value);
Pthread APS5_VABI scePthreadSelf();
}

static constexpr int SCE_KERNEL_ERROR_EINVAL = static_cast<int>(0x80020016);
static constexpr int SCE_KERNEL_ERROR_ESRCH = static_cast<int>(0x80020003);
static constexpr int Signal = 30;

#include <cstdio>
static void RequireAt(bool value, int line) { if (!value) { std::fprintf(stderr, "failed at line %d\n", line); std::abort(); } }
#define Require(value) RequireAt((value), __LINE__)

static HANDLE release;
static std::atomic<DWORD> waiterId{0};
static std::atomic<std::uintptr_t> waiterStack{0};
static std::atomic<DWORD> handledOn{0};
static std::atomic<Pthread> handledAs{nullptr};
static std::atomic<std::uintptr_t> handledRsp{0};
static std::atomic<int> handledSignal{0};

static void APS5_VABI Handler(int signum, void* context) {
    std::uint64_t rsp = 0;
    std::memcpy(&rsp, static_cast<const char*>(context) + 0xf8, sizeof(rsp));
    handledRsp.store(rsp);
    handledSignal.store(signum);
    handledAs.store(scePthreadSelf());
    handledOn.store(GetCurrentThreadId());
}

static void* APS5_VABI Waiter(void*) {
    int local = 0;
    waiterStack.store(reinterpret_cast<std::uintptr_t>(&local));
    waiterId.store(GetCurrentThreadId());
    WaitForSingleObject(release, INFINITE);  // a non-alertable wait, as a blocked game thread would be
    return nullptr;
}

static bool WaitFor(const std::atomic<DWORD>& value) {
    for (int i = 0; i < 500 && value.load() == 0; ++i) Sleep(10);
    return value.load() != 0;
}

int main() {
    Require(sceKernelRaiseException(scePthreadSelf(), 9) == SCE_KERNEL_ERROR_EINVAL);
    Require(sceKernelRaiseException(nullptr, Signal) == SCE_KERNEL_ERROR_ESRCH);
    Require(sceKernelInstallExceptionHandler(Signal, reinterpret_cast<void*>(&Handler)) == 0);

    Require(sceKernelRaiseException(scePthreadSelf(), Signal) == 0);
    Require(handledOn.load() == GetCurrentThreadId() && handledSignal.load() == Signal);
    Require(handledAs.load() == scePthreadSelf());

    handledOn.store(0);
    release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Pthread thread = nullptr;
    Require(scePthreadCreate(&thread, nullptr, &Waiter, nullptr, "waiter") == 0);
    Require(WaitFor(waiterId));
    Sleep(50);  // let it block
    Require(sceKernelRaiseException(thread, Signal) == 0);
    Require(WaitFor(handledOn));
    Require(handledAs.load() == thread);  // the handler sees the target as itself
    // mc_rsp is where the target was blocked: just below its own frame.
    Require(handledRsp.load() != 0 && handledRsp.load() < waiterStack.load());
    Require(waiterStack.load() - handledRsp.load() < 64 * 1024);
    Sleep(50);
    Require(scePthreadSelf() != thread);

    SetEvent(release);
    Require(scePthreadJoin(thread, nullptr) == 0);
    Require(sceKernelRemoveExceptionHandler(Signal) == 0);
    CloseHandle(release);
}
