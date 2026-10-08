#include "prx/libc/include/General.hpp"
#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <stdexcept>

static void Require(bool value) { if (!value) std::abort(); }

int main(int argc, char** argv) {
    const auto root = std::filesystem::canonical(std::filesystem::current_path());
    const auto original = root / "app0/Media/globalgamemanagers";
    const auto overridePath = root / ".anyps5/msaa-off/globalgamemanagers";
    const bool enabled = argc > 1 && argv[1][0] == '1';
    // These tests run in an isolated directory supplied by CTest.
    std::filesystem::create_directories(original.parent_path());
    std::filesystem::create_directories(overridePath.parent_path());
    if (enabled) {
        bool missing = false;
        try { (void)ResolvePath_nid_no_patch("/app0/Media/globalgamemanagers"); }
        catch (const std::runtime_error&) { missing = true; }
        Require(missing);
    }
    if (auto* file = std::fopen(overridePath.string().c_str(), "wb")) {
        std::fputs("override", file);
        std::fclose(file);
    } else std::abort();
    const auto expected = enabled ? overridePath : original;
    Require(ResolvePath_nid_no_patch("/app0/Media/globalgamemanagers") == expected);
    Require(ResolvePath_nid_no_patch("app0/Media/./globalgamemanagers") == expected);
    Require(ResolvePath_nid_no_patch("/app0/Media/other.assets") == root / "app0/Media/other.assets");
    Require(ResolvePath_nid_no_patch("/other/globalgamemanagers") == root / "other/globalgamemanagers");
    std::filesystem::remove(overridePath);
}
