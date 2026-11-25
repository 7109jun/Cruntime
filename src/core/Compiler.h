#pragma once
#include <memory>

#include "Common.h"
#include "Project.h"
#include "Toolchain.h"

namespace llvm {
class LLVMContext;
class Module;
}  // namespace llvm

namespace cr {

struct CompiledUnit {
    std::unique_ptr<llvm::LLVMContext> ctx;
    std::unique_ptr<llvm::Module> mod;
    CompiledUnit();
    CompiledUnit(CompiledUnit&&) noexcept;
    CompiledUnit& operator=(CompiledUnit&&) noexcept;
    ~CompiledUnit();
};

struct CompileOptions {
    int optLevel = 2;
    bool exceptions = true;
    bool verbose = false;
};

// Clang frontend (in-process, no driver, no files written) -> LLVM IR module in memory.
// `diag` receives all compiler diagnostics. Returns false on error.
bool compileSource(const SourceFile& src, const ProjectSpec& spec, const Toolchain& tc, const CompileOptions& opt,
                   CompiledUnit& out, std::string& diag);

}  // namespace cr
