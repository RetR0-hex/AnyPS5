#include "SceTypes.hpp"
#include "tests/GuestUnwindModuleInfoFixture.hpp"
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <windows.h>

extern "C" int APS5_VABI sceKernelGetModuleInfoForUnwind(uint64_t addr, int flags, ModuleInfoForUnwind* info);
extern "C" int APS5_VABI sceKernelGetModuleInfoFromAddr(uint64_t addr, int flags, ModuleInfoEx* info);

static void Require(bool value) { if (!value) std::abort(); }

static bool Throws(std::uint64_t address) {
    ModuleInfoForUnwind info{};
    try {
        sceKernelGetModuleInfoForUnwind(address, 0, &info);
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

static bool ExtendedThrows(std::uint64_t address) {
    ModuleInfoEx info{};
    info.st_size = sizeof(info);
    info.id = -123;
    try {
        sceKernelGetModuleInfoFromAddr(address, 2, &info);
    } catch (const std::runtime_error&) {
        // Invalid metadata must not publish a partially populated guest result.
        Require(info.id == -123 && info.segment_count == 0);
        return true;
    }
    return false;
}

int main() {
    auto* fixture = GetUnwindFixture();
    Require(fixture != nullptr);
    const auto address = reinterpret_cast<std::uint64_t>(fixture);
    MEMORY_BASIC_INFORMATION memory{};
    Require(VirtualQuery(reinterpret_cast<LPCVOID>(address), &memory, sizeof(memory)) != 0);
    const auto base = reinterpret_cast<std::uint64_t>(memory.AllocationBase);
    Require(base != reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr)));

    ModuleInfoForUnwind info{};
    Require(sceKernelGetModuleInfoForUnwind(address, 0, &info) == 0);
    Require(info.st_size == sizeof(ModuleInfoForUnwind));
    Require(info.eh_frame_hdr_addr == reinterpret_cast<std::uint64_t>(&fixture->header));
    Require(info.eh_frame_addr == reinterpret_cast<std::uint64_t>(&fixture->frames));
    Require(info.eh_frame_size == 4 + 12);
    Require(info.seg0_addr == base);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    Require(info.seg0_size == nt->OptionalHeader.SizeOfImage);

    // Both module queries must identify the same image and its relocated guest DWARF data.
    ModuleInfoEx extended{};
    extended.st_size = sizeof(extended);
    Require(sceKernelGetModuleInfoFromAddr(address, 2, &extended) == 0);
    Require(extended.segment_count > 0 && extended.segment_count <= 4 && extended.ref_count == 1);
    Require(extended.eh_frame_hdr_addr == info.eh_frame_hdr_addr && extended.eh_frame_hdr_size == 12);
    Require(extended.eh_frame_addr == info.eh_frame_addr && extended.eh_frame_size == info.eh_frame_size + 4);
    ModuleInfoEx again{};
    again.st_size = sizeof(again);
    Require(sceKernelGetModuleInfoFromAddr(address + 1, 2, &again) == 0 && again.id == extended.id);
    int local = 0;
    again.st_size = sizeof(again);
    Require(sceKernelGetModuleInfoFromAddr(reinterpret_cast<std::uint64_t>(&local), 2, &again) != 0);
    Require(sceKernelGetModuleInfoFromAddr(address, 2, nullptr) != 0);

    // A valid frame pointer does not make an unsupported or oversized search table valid. The
    // shared EhFrame reader decodes the count from the encoding's format nibble alone and takes
    // every fixed width (sdata4 0x0b included), so the variable-length uleb128 (0x01) stands for the
    // unsupported case.
    fixture->header.countEncoding = 0x01;
    Require(ExtendedThrows(address));
    fixture->header.countEncoding = 0x03;
    fixture->header.tableEncoding = 0x01; // uleb128 table entries have no fixed width
    Require(ExtendedThrows(address));
    fixture->header.tableEncoding = 0x3b;
    fixture->header.count = 0xffffffffu;
    Require(ExtendedThrows(address));
    fixture->header.count = 0;
    Require(!ExtendedThrows(address));

    fixture->header.version = 2;
    Require(Throws(address));
    fixture->header.version = 1;
    for (const std::uint8_t encoding : {0x3b, 0x9b, 0x0f, 0xff}) {
        fixture->header.framePointerEncoding = encoding;
        Require(Throws(address));
    }
    fixture->header.framePointerEncoding = 0x1b;
    fixture->frames.cieLength = 0xffffffffu;
    Require(Throws(address));
    fixture->frames.cieLength = 12;
    Require(!Throws(address));

    ModuleInfoForUnwind host{};
    Require(sceKernelGetModuleInfoForUnwind(reinterpret_cast<std::uint64_t>(&GetModuleHandleW), 0, &host) == 0);
    Require(host.eh_frame_hdr_addr == 0 && host.eh_frame_addr == 0 && host.eh_frame_size == 0);
}
