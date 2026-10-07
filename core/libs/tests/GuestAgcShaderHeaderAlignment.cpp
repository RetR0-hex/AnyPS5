#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

static void Require(bool value) { if (!value) std::abort(); }

static bool Registers(const Shader* header) {
    try {
        AgcDriverRegisterShader_nid_postfix(header);
        return true;
    } catch (const std::runtime_error&) {
        return false;
    }
}

int main() {
    alignas(256) static const std::array<std::uint32_t, 4> code{0xbf810000, 0, 0, 0};
    Shader shader{};
    shader.file_header = 0x34333231;
    shader.version = 0x18;
    shader.header_size = sizeof(Shader);
    shader.shader_size = sizeof(code);
    shader.code = code.data();

    // Shader binaries keep their headers on 4-byte boundaries; those register.
    alignas(16) static std::uint8_t storage[sizeof(Shader) + 16];
    auto* packed = reinterpret_cast<Shader*>(storage + 4);
    std::memcpy(static_cast<void*>(packed), &shader, sizeof(Shader));
    Require(Registers(packed));

    // A header that is not even 4-byte aligned is still refused.
    auto* broken = reinterpret_cast<Shader*>(storage + 2);
    std::memmove(static_cast<void*>(broken), &shader, sizeof(Shader));
    Require(!Registers(broken));
}
