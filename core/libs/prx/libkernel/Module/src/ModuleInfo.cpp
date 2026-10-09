#include <cstdint>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/KernelErrors.hpp"
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <link.h>
#include <unistd.h>
#endif

extern "C" std::int32_t ModuleIdForImage_nid_no_patch(const void* native);
#ifdef _WIN32
extern "C" int APS5_VABI sceKernelGetModuleInfoForUnwind(std::uint64_t, int, ModuleInfoForUnwind*);
#endif

namespace {

constexpr char GuestModuleSuffix[] = ".guest.prx";
constexpr std::int32_t ProtRead = 1;
constexpr std::int32_t ProtWrite = 2;
constexpr std::int32_t ProtExecute = 4;

std::uint32_t ToU32(std::uint64_t value, const char* field) {
    if (value > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error(std::string("sceKernelGetModuleInfoFromAddr: ") + field + " exceeds 32 bits");
    return static_cast<std::uint32_t>(value);
}

#ifdef _WIN32
void FillWindows(HMODULE module, std::uint64_t address, ModuleInfoEx& info) {
    const auto* base = reinterpret_cast<const std::uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    const auto contains = [base, nt](std::uint64_t start, std::uint64_t bytes) {
        const auto offset = start - reinterpret_cast<std::uintptr_t>(base);
        return offset <= nt->OptionalHeader.SizeOfImage && bytes <= nt->OptionalHeader.SizeOfImage - offset;
    };
    char path[32768];
    const auto length = GetModuleFileNameA(module, path, sizeof(path));
    if (length == 0 || length >= sizeof(path)) throw std::runtime_error("sceKernelGetModuleInfoFromAddr: cannot resolve the module path");
    std::string name(path, length);
    name = name.substr(name.find_last_of("/\\") + 1);
    if (name.ends_with(GuestModuleSuffix)) name.resize(name.size() - (sizeof(GuestModuleSuffix) - 1));
    if (name.size() >= sizeof(info.name)) throw std::runtime_error("sceKernelGetModuleInfoFromAddr: module name too long");
    std::memcpy(info.name, name.c_str(), name.size() + 1);
    // Converted PT_LOAD segments retain their own .elf sections. Exclude the relinker's
    // trampolines and PE loader data from the guest segment list; native modules use PE sections.
    bool guest = false;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) guest |= std::memcmp(sections[i].Name, ".elf", 4) == 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& section = sections[i];
        if (guest && std::memcmp(section.Name, ".elf", 4) != 0) continue;
        if (section.Misc.VirtualSize == 0 || (section.Characteristics & IMAGE_SCN_MEM_DISCARDABLE) != 0) continue;
        if (info.segment_count >= std::size(info.segments)) break;
        auto& segment = info.segments[info.segment_count++];
        segment.address = reinterpret_cast<std::uintptr_t>(base + section.VirtualAddress);
        segment.size = section.Misc.VirtualSize;
        segment.prot = ((section.Characteristics & IMAGE_SCN_MEM_READ) ? ProtRead : 0) |
                       ((section.Characteristics & IMAGE_SCN_MEM_WRITE) ? ProtWrite : 0) |
                       ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) ? ProtExecute : 0);
    }
    const auto& tlsDirectory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (tlsDirectory.VirtualAddress != 0) {
        // PE TLS fields are relocated addresses, while the guest ABI stores sizes in 32 bits.
        // Validate the template and index before reading them and reject oversized zero fill.
        if (!contains(reinterpret_cast<std::uintptr_t>(base) + tlsDirectory.VirtualAddress, sizeof(IMAGE_TLS_DIRECTORY64)))
            throw std::runtime_error("sceKernelGetModuleInfoFromAddr: TLS directory outside the image");
        const auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(base + tlsDirectory.VirtualAddress);
        if (tls->EndAddressOfRawData < tls->StartAddressOfRawData ||
            (tls->EndAddressOfRawData != tls->StartAddressOfRawData &&
             !contains(tls->StartAddressOfRawData, tls->EndAddressOfRawData - tls->StartAddressOfRawData)) ||
            !contains(tls->AddressOfIndex, sizeof(DWORD)))
            throw std::runtime_error("sceKernelGetModuleInfoFromAddr: TLS data outside the image");
        info.tls_index = *reinterpret_cast<const DWORD*>(tls->AddressOfIndex);
        info.tls_init_addr = tls->StartAddressOfRawData;
        info.tls_init_size = ToU32(tls->EndAddressOfRawData - tls->StartAddressOfRawData, "TLS image size");
        info.tls_size = ToU32(static_cast<std::uint64_t>(info.tls_init_size) + tls->SizeOfZeroFill, "TLS size");
        const auto alignment = (tls->Characteristics >> 20u) & 15u;
        info.tls_align = alignment == 0 ? 1u : 1u << (alignment - 1u);
    }
    ModuleInfoForUnwind unwind{};
    if (sceKernelGetModuleInfoForUnwind(address, 0, &unwind) == 0 && unwind.eh_frame_hdr_addr != 0) {
        info.eh_frame_hdr_addr = unwind.eh_frame_hdr_addr;
        info.eh_frame_addr = unwind.eh_frame_addr;
        // The unwind query omits the terminating zero record; ModuleInfoEx includes it.
        info.eh_frame_size = ToU32(unwind.eh_frame_size + 4, "eh_frame size");
        const auto offset = unwind.eh_frame_hdr_addr - reinterpret_cast<std::uintptr_t>(base);
        if (offset > nt->OptionalHeader.SizeOfImage || 12 > nt->OptionalHeader.SizeOfImage - offset)
            throw std::runtime_error("sceKernelGetModuleInfoFromAddr: eh_frame_hdr outside the image");
        const auto* header = reinterpret_cast<const std::uint8_t*>(unwind.eh_frame_hdr_addr);
        // Relinked DWARF tables use a fixed-width count and data-relative 32-bit search entries.
        // Reject other formats instead of reporting an invented header extent.
        if (header[0] != 1 || header[1] != 0x1b || header[2] != 0x03 || header[3] != 0x3b)
            throw std::runtime_error("sceKernelGetModuleInfoFromAddr: unsupported eh_frame_hdr table encoding");
        std::uint32_t count;
        std::memcpy(&count, header + 8, sizeof(count));
        const auto bytes = 12ull + 8ull * count;
        if (offset > nt->OptionalHeader.SizeOfImage || bytes > nt->OptionalHeader.SizeOfImage - offset)
            throw std::runtime_error("sceKernelGetModuleInfoFromAddr: eh_frame_hdr table outside the image");
        info.eh_frame_hdr_size = static_cast<std::uint32_t>(bytes);
    }
    info.id = ModuleIdForImage_nid_no_patch(module);
    // PE entry points are loader callbacks, not the original ELF DT_INIT/DT_FINI routines.
    // Leave those guest addresses unset until the converter preserves their metadata.
    info.ref_count = 1;
}
#else

struct ImageSearch {
    std::uintptr_t address;
    ModuleInfoEx* info;
    bool found;
};

bool Contains(const dl_phdr_info& image, std::uintptr_t begin, std::uint64_t size) {
    for (std::uint16_t i = 0; i < image.dlpi_phnum; ++i) {
        const auto& header = image.dlpi_phdr[i];
        if (header.p_type != PT_LOAD) continue;
        const std::uintptr_t start = image.dlpi_addr + header.p_vaddr;
        if (begin >= start && begin - start <= header.p_memsz && size <= header.p_memsz - (begin - start)) return true;
    }
    return false;
}

std::uintptr_t EhFrameAddress(const dl_phdr_info& image, std::uintptr_t header) {
    if (!Contains(image, header, 8))
        throw std::runtime_error("sceKernelGetModuleInfoFromAddr: eh_frame_hdr outside the image");
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(header);
    if (bytes[0] != 1) throw std::runtime_error("sceKernelGetModuleInfoFromAddr: unsupported eh_frame_hdr version");
    const std::uintptr_t field = header + 4;
    switch (bytes[1]) {
    case 0x1b: {
        std::int32_t offset;
        std::memcpy(&offset, bytes + 4, sizeof(offset));
        return field + static_cast<std::intptr_t>(offset);
    }
    case 0x03: {
        std::uint32_t value;
        std::memcpy(&value, bytes + 4, sizeof(value));
        return value;
    }
    case 0x00: {
        std::uint64_t value;
        if (!Contains(image, header, 12))
            throw std::runtime_error("sceKernelGetModuleInfoFromAddr: eh_frame_hdr outside the image");
        std::memcpy(&value, bytes + 4, sizeof(value));
        return static_cast<std::uintptr_t>(value);
    }
    default:
        throw std::runtime_error("sceKernelGetModuleInfoFromAddr: unsupported eh_frame_ptr encoding");
    }
}

std::uint64_t EhFrameSize(const dl_phdr_info& image, std::uintptr_t frame) {
    std::uintptr_t position = frame;
    for (;;) {
        if (!Contains(image, position, 4))
            throw std::runtime_error("sceKernelGetModuleInfoFromAddr: eh_frame outside the image");
        std::uint32_t length;
        std::memcpy(&length, reinterpret_cast<const void*>(position), sizeof(length));
        position += 4;
        if (length == 0) return position - frame;
        std::uint64_t recordLength = length;
        if (length == 0xffffffffu) {
            if (!Contains(image, position, 8))
                throw std::runtime_error("sceKernelGetModuleInfoFromAddr: eh_frame outside the image");
            std::memcpy(&recordLength, reinterpret_cast<const void*>(position), sizeof(recordLength));
            position += 8;
        }
        if (!Contains(image, position, recordLength))
            throw std::runtime_error("sceKernelGetModuleInfoFromAddr: eh_frame record outside the image");
        position += recordLength;
    }
}

std::string ImageName(const dl_phdr_info& image) {
    std::string path = image.dlpi_name ? image.dlpi_name : "";
    if (path.empty()) {
        char executable[4096];
        const auto length = ::readlink("/proc/self/exe", executable, sizeof(executable) - 1);
        if (length < 0) throw std::runtime_error("sceKernelGetModuleInfoFromAddr: cannot resolve the executable path");
        path.assign(executable, static_cast<std::size_t>(length));
    }
    std::string name = path.substr(path.find_last_of('/') + 1);
    if (name.size() > sizeof(GuestModuleSuffix) - 1 && name.ends_with(GuestModuleSuffix))
        name.resize(name.size() - (sizeof(GuestModuleSuffix) - 1));
    return name;
}

void Fill(const dl_phdr_info& image, ModuleInfoEx& info) {
    const auto name = ImageName(image);
    if (name.size() >= sizeof(info.name))
        throw std::runtime_error("sceKernelGetModuleInfoFromAddr: module name too long: " + name);
    std::memcpy(info.name, name.c_str(), name.size() + 1);
    info.tls_index = ToU32(image.dlpi_tls_modid, "TLS module index");
    for (std::uint16_t i = 0; i < image.dlpi_phnum; ++i) {
        const auto& header = image.dlpi_phdr[i];
        const std::uintptr_t address = image.dlpi_addr + header.p_vaddr;
        if (header.p_type == PT_LOAD && info.segment_count < std::size(info.segments)) {
            auto& segment = info.segments[info.segment_count++];
            segment.address = address;
            segment.size = ToU32(header.p_memsz, "segment size");
            segment.prot = ((header.p_flags & PF_R) ? ProtRead : 0) | ((header.p_flags & PF_W) ? ProtWrite : 0) | ((header.p_flags & PF_X) ? ProtExecute : 0);
        } else if (header.p_type == PT_TLS) {
            info.tls_init_addr = address;
            info.tls_init_size = ToU32(header.p_filesz, "TLS image size");
            info.tls_size = ToU32(header.p_memsz, "TLS size");
            info.tls_align = ToU32(header.p_align, "TLS alignment");
        } else if (header.p_type == PT_GNU_EH_FRAME) {
            info.eh_frame_hdr_addr = address;
            info.eh_frame_hdr_size = ToU32(header.p_memsz, "eh_frame_hdr size");
            info.eh_frame_addr = EhFrameAddress(image, address);
            info.eh_frame_size = ToU32(EhFrameSize(image, info.eh_frame_addr), "eh_frame size");
        } else if (header.p_type == PT_DYNAMIC) {
            for (const auto* entry = reinterpret_cast<const ElfW(Dyn)*>(address); entry->d_tag != DT_NULL; ++entry) {
                if (entry->d_tag == DT_INIT) info.init_proc_addr = image.dlpi_addr + entry->d_un.d_ptr;
                else if (entry->d_tag == DT_FINI) info.fini_proc_addr = image.dlpi_addr + entry->d_un.d_ptr;
            }
        }
    }
    info.ref_count = 1;
}

int FindImage(dl_phdr_info* image, std::size_t, void* data) {
    auto& search = *static_cast<ImageSearch*>(data);
    if (!Contains(*image, search.address, 1)) return 0;
    Fill(*image, *search.info);
    search.found = true;
    return 1;
}
#endif

}

extern "C" {

int APS5_VABI sceKernelGetModuleInfoFromAddr(std::uint64_t address, int flags, ModuleInfoEx* info) {
    if (!info) return SCE_KERNEL_ERROR_EFAULT;
    if (flags != 2) throw std::invalid_argument("sceKernelGetModuleInfoFromAddr: unsupported flags " + std::to_string(flags));
    if (info->st_size != sizeof(ModuleInfoEx))
        throw std::invalid_argument("sceKernelGetModuleInfoFromAddr: unsupported st_size " + std::to_string(info->st_size));
#ifdef _WIN32
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module)) return SCE_KERNEL_ERROR_ESRCH;
    ModuleInfoEx result{};
    result.st_size = sizeof(ModuleInfoEx);
    FillWindows(module, address, result);
    *info = result;
    return 0;
#else
    Dl_info symbol{};
    link_map* native = nullptr;
    if (!dladdr1(reinterpret_cast<const void*>(address), &symbol, reinterpret_cast<void**>(&native), RTLD_DL_LINKMAP) || !native)
        return SCE_KERNEL_ERROR_ESRCH;
    ModuleInfoEx result{};
    result.st_size = sizeof(ModuleInfoEx);
    ImageSearch search{static_cast<std::uintptr_t>(address), &result, false};
    dl_iterate_phdr(FindImage, &search);
    if (!search.found) return SCE_KERNEL_ERROR_ESRCH;
    result.id = ModuleIdForImage_nid_no_patch(native);
    *info = result;
    return 0;
#endif
}

}
