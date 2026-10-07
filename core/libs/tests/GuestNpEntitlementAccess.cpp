#include "SceTypes.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
int APS5_VABI sceNpEntitlementAccessGetEntitlementKey(std::uint32_t service_label, const NpUnifiedEntitlementLabel* entitlement_label, NpEntitlementAccessEntitlementKey* key);
}

namespace {

constexpr int Parameter = static_cast<int>(0x80558003u);
constexpr int NotFound = static_cast<int>(0x80558007u);

void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "NpEntitlementAccess: %s\n", message);
        std::abort();
    }
}

NpUnifiedEntitlementLabel EntitlementLabel(const char* text) {
    NpUnifiedEntitlementLabel label{};
    std::strncpy(label.data, text, sizeof(label.data) - 1);
    return label;
}

}

int main() {
    const char* path = "anyps5-np-entitlement-access-test.ini";
    std::FILE* file = std::fopen(path, "w");
    Require(file != nullptr, "cannot write the entitlement list");
    std::fputs("# owned add-ons\nOWNEDDLC0000001\n", file);
    std::fclose(file);
#ifdef _WIN32
    _putenv_s("ANYPS5_ENTITLEMENTS", path);
#else
    setenv("ANYPS5_ENTITLEMENTS", path, 1);
#endif

    NpEntitlementAccessEntitlementKey key{};
    std::memset(key.data, 0x5a, sizeof(key.data));
    const auto owned = EntitlementLabel("OWNEDDLC0000001");
    Require(sceNpEntitlementAccessGetEntitlementKey(1, &owned, &key) == 0, "owned add-on has a key");
    const NpEntitlementAccessEntitlementKey zero{};
    Require(std::memcmp(key.data, zero.data, sizeof(key.data)) == 0, "key is all zeros");

    const auto missing = EntitlementLabel("NOTOWNED0000001");
    Require(sceNpEntitlementAccessGetEntitlementKey(1, &missing, &key) == NotFound, "unowned add-on is not found");
    Require(sceNpEntitlementAccessGetEntitlementKey(1, nullptr, &key) == Parameter, "null label is rejected");
    Require(sceNpEntitlementAccessGetEntitlementKey(1, &owned, nullptr) == Parameter, "null key is rejected");

    std::remove(path);
    return 0;
}
