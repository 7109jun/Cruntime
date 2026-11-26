#pragma once
#include <vector>

namespace cr {

// Symbols that MSVC-ABI code expects from the *static* part of the CRT (msvcrt.lib), which is not exported by
// any DLL. The host exe owns them, so the JIT gets their addresses from here.
struct HostSym {
    const char* name;  // COFF x64 mangled name (no prefix)
    void* addr;
};
std::vector<HostSym> hostSymbols();

}  // namespace cr
