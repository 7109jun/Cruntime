#pragma once
#include <memory>
#include <string>
#include <vector>

#include "Common.h"
#include "Compiler.h"

namespace llvm {
namespace orc {
class LLJIT;
}
}  // namespace llvm

namespace cr {

// Thin wrapper around LLVM ORC LLJIT: IR modules in memory -> native code in memory -> run.
class JitEngine {
public:
    static std::unique_ptr<JitEngine> create(int optLevel, std::string& err);
    ~JitEngine();

    // Optimise (if enabled) and hand an IR module to the JIT.
    bool addModule(CompiledUnit&& unit, std::string& err);

    // Resolve an external library: path or bare name, .dll (loaded) or .lib (static archive, or import
    // library -> matching DLL). Directories in `searchDirs` are consulted for bare names.
    // Returns false (with `warning` set) if it could not be resolved; callers treat that as a warning.
    bool addLibrary(const std::string& lib, const std::vector<fs::path>& searchDirs, std::string& warning);

    // Initialise static constructors, find main/wmain/WinMain/wWinMain and run it.
    // Returns false with `err` on link/lookup failure; otherwise `exitCode` is the program's return value.
    bool runEntry(const std::vector<std::string>& args, int& exitCode, std::string& err);

private:
    JitEngine() = default;
    std::unique_ptr<llvm::orc::LLJIT> jit_;
    int optLevel_ = 2;
    bool initialized_ = false;
};

}  // namespace cr
