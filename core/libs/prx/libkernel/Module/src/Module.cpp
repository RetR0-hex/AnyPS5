#include <cstdint>
#include <cstddef>
#include <cstring>
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
int APS5_VABI dlclose_nid_postfix(void* handle);
}

#ifdef _WIN32
namespace {

// Reads a DWARF exception-header pointer (the encodings eh_frame_hdr uses).
bool ReadEncoded(const std::uint8_t*& p, std::uint8_t encoding, std::uintptr_t dataBase, std::uintptr_t& value) {
  const auto at = reinterpret_cast<std::uintptr_t>(p);
  std::int64_t raw = 0;
  switch (encoding & 0x0f) {
  case 0x00: case 0x04: case 0x0c: { std::int64_t v; std::memcpy(&v, p, 8); raw = v; p += 8; break; }
  case 0x03: { std::uint32_t v; std::memcpy(&v, p, 4); raw = v; p += 4; break; }
  case 0x0b: { std::int32_t v; std::memcpy(&v, p, 4); raw = v; p += 4; break; }
  default: return false;
  }
  switch (encoding & 0x70) {
  case 0x00: value = static_cast<std::uintptr_t>(raw); return true;
  case 0x10: value = at + static_cast<std::uintptr_t>(raw); return true;
  case 0x30: value = dataBase + static_cast<std::uintptr_t>(raw); return true;
  default: return false;
  }
}

// A relinked image records its eh_frame_hdr RVA in a ".ehmeta" section; the
// game's own unwinder (its libc.prx) needs it, and the eh_frame it points to.
bool FillUnwindInfo(std::uint64_t addr, ModuleInfoForUnwind& info) {
  MEMORY_BASIC_INFORMATION memory{};
  if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &memory, sizeof(memory)) || memory.Type != MEM_IMAGE) return false;
  const auto* base = static_cast<const std::uint8_t*>(memory.AllocationBase);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
  info = {};
  info.st_size = sizeof(ModuleInfoForUnwind);
  char path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameA(reinterpret_cast<HMODULE>(const_cast<std::uint8_t*>(base)), path, sizeof(path));
  std::string name(path, length);
  name = name.substr(name.find_last_of("\/") + 1);
  if (name.ends_with(".guest.prx")) name.resize(name.size() - 10);
  std::strncpy(info.name, name.c_str(), sizeof(info.name) - 1);
  const auto* sections = IMAGE_FIRST_SECTION(nt);
  const auto rva = static_cast<std::uint32_t>(addr - reinterpret_cast<std::uintptr_t>(base));
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    const auto& section = sections[i];
    if (rva >= section.VirtualAddress && rva - section.VirtualAddress < section.Misc.VirtualSize) {
      info.seg0_addr = reinterpret_cast<std::uintptr_t>(base) + section.VirtualAddress;
      info.seg0_size = section.Misc.VirtualSize;
    }
    if (std::memcmp(section.Name, ".ehmeta", 8) != 0) continue;
    std::uint32_t headerRva = 0;
    std::memcpy(&headerRva, base + section.VirtualAddress, sizeof(headerRva));
    const auto* header = base + headerRva;
    if (header[0] != 1) continue;
    const auto* p = header + 4;
    std::uintptr_t frames = 0;
    if (!ReadEncoded(p, header[1], reinterpret_cast<std::uintptr_t>(header), frames)) continue;
    info.eh_frame_hdr_addr = reinterpret_cast<std::uintptr_t>(header);
    info.eh_frame_addr = frames;
    // eh_frame ends at a zero-length record or at the end of its section.
    const auto imageEnd = reinterpret_cast<std::uintptr_t>(base) + nt->OptionalHeader.SizeOfImage;
    std::uintptr_t cursor = frames;
    while (cursor + 4 <= imageEnd) {
      std::uint32_t recordLength = 0;
      std::memcpy(&recordLength, reinterpret_cast<const void*>(cursor), 4);
      if (recordLength == 0 || recordLength == 0xffffffffu || recordLength > imageEnd - cursor - 4) break;
      cursor += 4 + recordLength;
    }
    info.eh_frame_size = cursor - frames;
  }
  if (info.seg0_addr == 0) {
    info.seg0_addr = reinterpret_cast<std::uint64_t>(memory.BaseAddress);
    info.seg0_size = memory.RegionSize;
  }
  return true;
}

}
#endif

namespace {
constexpr int kRtldNow = 2;
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
  return FillUnwindInfo(addr, *info) ? 0 : SCE_KERNEL_ERROR_ESRCH;
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
 (void)args;
 (void)argp;
 (void)flags;
 (void)opt;
 if (res) *res = 0;
 if (!module_file_name) return static_cast<KernelModule>(SCE_KERNEL_ERROR_EFAULT);
 void* handle = dlopen_nid_postfix(module_file_name, kRtldNow);
 if (!handle) return static_cast<KernelModule>(SCE_KERNEL_ERROR_ENOENT);
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
