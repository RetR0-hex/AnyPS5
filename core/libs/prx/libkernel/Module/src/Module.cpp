#include <cstdint>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/specifics/linux/ElfTypes.hpp"
#include "prx/libkernel/DirectMemory/DirectMemory.hpp"
#include "prx/libkernel/KernelErrors.hpp"
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <fstream>
#endif

extern "C" {
void* APS5_VABI dlopen_nid_postfix(const char* path, int flags);
void* APS5_VABI dlsym_nid_postfix(void* handle, const char* name);
char* APS5_VABI dlerror_nid_postfix();
int APS5_VABI dlclose_nid_postfix(void* handle);
}

namespace {
constexpr int kRtldNow = 2;

// A relinked module the program does not start itself (a Unity plugin) exports
// this table: its init RVA, the number of init array slots, then the slot RVAs.
constexpr char kGuestStartExport[] = "__aps5_guest_start";
using GuestInit = int (APS5_VABI*)(size_t, const void*, void*);
using GuestInitArrayEntry = void (APS5_VABI*)(int, char**, char**);

// Runs a start-on-load module's init once, passing the loader's arguments to its
// module_start. Returns that result, or 0 when the module was started already or
// at program start.
int StartDeferredModule(void* handle, size_t args, const void* argp) {
    const auto* table = static_cast<const std::uint32_t*>(dlsym_nid_postfix(handle, kGuestStartExport));
    if (!table) {
        dlerror_nid_postfix();
        return 0;
    }
    static std::mutex mutex;
    static std::set<const void*> started;
    {
        std::lock_guard lock(mutex);
        if (!started.insert(table).second) return 0;
    }
#ifdef _WIN32
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(table), &module))
        throw std::runtime_error("sceKernelLoadStartModule: cannot find the module of a start table");
    const auto base = reinterpret_cast<std::uintptr_t>(module);
#else
    throw std::runtime_error("sceKernelLoadStartModule: start-on-load modules are only produced for Windows");
    const std::uintptr_t base = 0;
#endif
    int result = 0;
    if (table[0] != 0) result = reinterpret_cast<GuestInit>(base + table[0])(args, argp, nullptr);
    for (std::uint32_t index = 0; index < table[1]; ++index) {
        const auto entry = *reinterpret_cast<GuestInitArrayEntry*>(base + table[2 + index]);
        if (entry) entry(0, nullptr, nullptr);
    }
    return result;
}
}

extern "C" {

int APS5_VABI sceKernelDlsym(KernelModule handle, const char* symbol, void** addr) {
 if (!symbol || !addr) return SCE_KERNEL_ERROR_EFAULT;
 void* found = dlsym_nid_postfix(reinterpret_cast<void*>(static_cast<intptr_t>(handle)), symbol);
 if (!found) return SCE_KERNEL_ERROR_ESRCH;
 *addr = found;
 return 0;
}

int APS5_VABI sceKernelGetModuleInfoForUnwind(uint64_t addr, int flags, ModuleInfoForUnwind* info) {
  (void)flags;
  if (!info) return SCE_KERNEL_ERROR_EFAULT;
#ifdef _WIN32
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) return SCE_KERNEL_ERROR_ESRCH;
  info->st_size = sizeof(ModuleInfoForUnwind);
  info->eh_frame_hdr_addr = 0;
  info->eh_frame_addr = 0;
  info->eh_frame_size = 0;
  info->seg0_addr = reinterpret_cast<std::uint64_t>(mbi.BaseAddress);
  info->seg0_size = mbi.RegionSize;
  char path[4096] = {};
  DWORD len = GetMappedFileNameA(GetCurrentProcess(), mbi.BaseAddress, path, sizeof(path) - 1);
  path[len] = '\0';
  std::strncpy(info->name, path, sizeof(info->name) - 1);
  info->name[sizeof(info->name) - 1] = '\0';
  return 0;
#else
  std::ifstream maps("/proc/self/maps");
  if (!maps) throw std::runtime_error("sceKernelGetModuleInfoForUnwind: failed to open /proc/self/maps");
  std::string line;
  while (std::getline(maps, line)) {
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    char perms[8] = {};
    std::uint64_t offset = 0;
    unsigned int devMajor = 0;
    unsigned int devMinor = 0;
    std::uint64_t inode = 0;
    char path[4096] = {};
    int parsed = std::sscanf(line.c_str(), "%llx-%llx %7s %llx %x:%x %llu %4095s",
      (unsigned long long*)&start, (unsigned long long*)&end, perms,
      (unsigned long long*)&offset, &devMajor, &devMinor, (unsigned long long*)&inode, path);
    if (parsed < 7 || addr < start || addr >= end) continue;
    info->st_size = sizeof(ModuleInfoForUnwind);
    std::strncpy(info->name, parsed >= 8 ? path : "", sizeof(info->name) - 1);
    info->name[sizeof(info->name) - 1] = '\0';
    info->eh_frame_hdr_addr = 0;
    info->eh_frame_addr = 0;
    info->eh_frame_size = 0;
    info->seg0_addr = start;
    info->seg0_size = end - start;
    return 0;
  }
  return SCE_KERNEL_ERROR_ESRCH;
#endif
}

KernelModule APS5_VABI sceKernelLoadStartModule(const char* module_file_name, size_t args, const void* argp, uint32_t flags, const KernelLoadModuleOpt* opt, int* res) {
 (void)flags;
 (void)opt;
 if (res) *res = 0;
 if (!module_file_name) return static_cast<KernelModule>(SCE_KERNEL_ERROR_EFAULT);
 void* handle = dlopen_nid_postfix(module_file_name, kRtldNow);
 if (!handle) return static_cast<KernelModule>(SCE_KERNEL_ERROR_ENOENT);
 const int started = StartDeferredModule(handle, args, argp);
 if (res) *res = started;
 return static_cast<KernelModule>(reinterpret_cast<intptr_t>(handle));
}

int APS5_VABI sceKernelStopUnloadModule(KernelModule handle, size_t args, const void* argp, uint32_t flags, const KernelUnloadModuleOpt* opt, int* res) {
 (void)args;
 (void)argp;
 (void)flags;
 (void)opt;
 if (res) *res = 0;
 return dlclose_nid_postfix(reinterpret_cast<void*>(static_cast<intptr_t>(handle))) == 0 ? 0 : SCE_KERNEL_ERROR_ESRCH;
}

}

extern "C" {

int APS5_VABI __elf_phdr_match_addr_nid_postfix(dl_phdr_info* phdrInfo, void* addr) {
    if (phdrInfo == nullptr) throw std::invalid_argument("__elf_phdr_match_addr: phdr_info is null");
    const auto address = reinterpret_cast<std::uintptr_t>(addr);
    for (std::uint16_t i = 0; i < phdrInfo->dlpi_phnum; ++i) {
        const Elf64_Phdr& header = phdrInfo->dlpi_phdr[i];
        if (header.p_type != PT_LOAD || (header.p_flags & PF_X) == 0) continue;
        const std::uintptr_t begin = phdrInfo->dlpi_addr + header.p_vaddr;
        if (begin <= address && address + sizeof(addr) < begin + header.p_memsz) return 1;
    }
    return 0;
}

// unknown signature
std::int32_t APS5_VABI sceKernelInternalMemoryGetModuleSegmentInfo_nid_postfix(void* result) {
    (void)result;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
