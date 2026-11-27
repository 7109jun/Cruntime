#include "Compiler.h"

#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/DiagnosticOptions.h>
#include <clang/CodeGen/CodeGenAction.h>
#include <clang/Frontend/CompilerInstance.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <clang/Frontend/CompilerInvocation.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

namespace cr {

CompiledUnit::CompiledUnit() = default;
CompiledUnit::CompiledUnit(CompiledUnit&&) noexcept = default;
CompiledUnit& CompiledUnit::operator=(CompiledUnit&&) noexcept = default;
// Module must die before its context.
CompiledUnit::~CompiledUnit() {
    mod.reset();
    ctx.reset();
}

namespace {

// Options we let through from CMake / the user (cc1 understands these).
bool acceptOption(const std::string& o, Lang lang) {
    if (o.rfind("-D", 0) == 0 || o.rfind("-U", 0) == 0 || o.rfind("-I", 0) == 0) return true;
    if (o.rfind("-std=", 0) == 0) return false;  // handled via spec.cStd / spec.cxxStd
    if (o.rfind("-W", 0) == 0 || o.rfind("-O", 0) == 0) return true;
    if (o == "-fno-exceptions" || o == "-fno-rtti" || o == "-fms-extensions" || o == "-ffast-math") return lang == Lang::Cxx || o != "-fno-rtti";
    return false;
}

}  // namespace

bool compileSource(const SourceFile& src, const ProjectSpec& spec, const Toolchain& tc, const CompileOptions& opt,
                   CompiledUnit& out, std::string& diag) {
    const bool cxx = src.lang == Lang::Cxx;
    std::vector<std::string> a;

    a.insert(a.end(), {"-triple", "x86_64-pc-windows-msvc", "-emit-llvm-only", "-disable-llvm-passes"});
    a.push_back(opt.optLevel > 0 ? "-O" + std::to_string(std::min(opt.optLevel, 3)) : "-O0");
    a.insert(a.end(), {"-fms-extensions", "-fms-compatibility", "-fms-compatibility-version=19.40"});
    if (cxx) {
        a.push_back("-fdelayed-template-parsing");
        if (opt.exceptions) a.insert(a.end(), {"-fcxx-exceptions", "-fexceptions"});
    }
    a.insert(a.end(), {"-x", cxx ? "c++" : "c"});
    const std::string& stdv = cxx ? spec.cxxStd : spec.cStd;
    a.push_back("-std=" + (stdv.empty() ? std::string(cxx ? "c++20" : "c17") : stdv));
    a.insert(a.end(), {"-resource-dir", u8(tc.resourceDir)});

    // Predefined macros expected by MSVC-flavoured headers (/MD runtime).
    for (const char* d : {"-D_MT", "-D_DLL", "-DWIN32", "-D_WINDOWS", "-D_CRT_SECURE_NO_WARNINGS",
                          "-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH"})
        a.push_back(d);

    // System include paths: Clang builtins first, then MSVC STL / UCRT / SDK.
    a.insert(a.end(), {"-internal-isystem", u8(tc.resourceDir / "include")});
    for (const auto& d : tc.includeDirs) a.insert(a.end(), {"-internal-isystem", u8(d)});

    for (const auto& d : spec.includeDirs) a.push_back("-I" + u8(d));
    for (const auto& d : spec.defines) a.push_back("-D" + d);
    for (const auto& o : spec.compileOptions)
        if (acceptOption(o, src.lang)) a.push_back(o);
    a.push_back(u8(src.path));

    std::vector<const char*> argv;
    for (const auto& s : a) argv.push_back(s.c_str());

    llvm::raw_string_ostream os(diag);
    llvm::IntrusiveRefCntPtr<clang::DiagnosticOptions> diagOpts = new clang::DiagnosticOptions();
    diagOpts->ShowColors = false;
    clang::TextDiagnosticPrinter printer(os, diagOpts.get());
    llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs> ids(new clang::DiagnosticIDs());
    clang::DiagnosticsEngine argDiags(ids, diagOpts, &printer, /*ShouldOwnClient=*/false);

    auto inv = std::make_shared<clang::CompilerInvocation>();
    if (!clang::CompilerInvocation::CreateFromArgs(*inv, argv, argDiags)) {
        os.flush();
        diag += "error: invalid compiler arguments for " + u8(src.path) + "\n";
        return false;
    }

    clang::CompilerInstance ci;
    ci.setInvocation(inv);
    ci.createDiagnostics(&printer, /*ShouldOwnClient=*/false);
    if (!spec.virtualFiles.empty()) {
        // Generated files (configure_file ...) exist only in memory: overlay them on the real file system.
        llvm::IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> ov(new llvm::vfs::OverlayFileSystem(llvm::vfs::getRealFileSystem()));
        llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> mem(new llvm::vfs::InMemoryFileSystem());
        for (const auto& v : spec.virtualFiles) mem->addFile(u8(v.first), 0, llvm::MemoryBuffer::getMemBufferCopy(v.second));
        ov->pushOverlay(mem);
        ci.createFileManager(ov);
        ci.createSourceManager(ci.getFileManager());
    }

    auto ctx = std::make_unique<llvm::LLVMContext>();
    clang::EmitLLVMOnlyAction action(ctx.get());
    bool ok = ci.ExecuteAction(action);
    bool errored = ci.getDiagnostics().hasErrorOccurred();
    os.flush();
    if (!ok || errored) return false;

    out.mod = action.takeModule();
    if (!out.mod) {
        diag += "error: no module produced for " + u8(src.path) + "\n";
        return false;
    }
    out.ctx = std::move(ctx);
    return true;
}

}  // namespace cr
