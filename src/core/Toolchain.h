#pragma once
#include "Common.h"

namespace cr {

// MSVC STL / UCRT / Windows SDK locations used as -isystem paths for Clang.
struct Toolchain {
    std::vector<fs::path> includeDirs;
    std::vector<fs::path> libDirs;
    fs::path resourceDir;  // contains include/stddef.h etc. (shipped next to the exe)
    std::string msvcVersion;
    std::string sdkVersion;
};

fs::path executableDir();
bool locateToolchain(Toolchain& out, std::string& err);

}  // namespace cr
