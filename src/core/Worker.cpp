#include "Worker.h"
#include <algorithm>
#include <atomic>

#include <windows.h>

#include <cstdio>

#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include "Compiler.h"
#include "JitEngine.h"
#include "Scanner.h"
#include "Toolchain.h"

namespace cr {

const char* usageText() {
    return "CRuntime - run C/C++ sources and projects straight from memory (LLVM ORC JIT)\n"
           "\n"
           "  CRuntime                          open the GUI (drag & drop files/folders)\n"
           "  CRuntime run <input>... [options] [-- program args]\n"
           "\n"
           "  <input>   .c .cpp .cc .cxx .h .hpp | folder | CMakeLists.txt | CMake.txt\n"
           "Options:\n"
           "  -O0..-O3            optimisation level (default -O2)\n"
           "  --target <name>     CMake executable target to run\n"
           "  -I<dir>  -D<M[=V]>  extra include dir / macro\n"
           "  -L<dir>  -l<lib>    extra library dir / library (.lib/.dll name or path)\n"
           "  --std <std>         language standard for the language of the sources (c17, c++23, ...)\n"
           "  --no-exceptions     compile C++ without exceptions\n"
           "  --dump-ir <dir>     write the generated LLVM IR (.ll) of each source to <dir> (debugging)\n"
           "  -v                  verbose (also traces unwind-table registration)\n";
}

bool parseRunArgs(const std::vector<std::string>& args, RunOptions& o, std::string& err) {
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--") {
            for (size_t j = i + 1; j < args.size(); ++j) o.programArgs.push_back(args[j]);
            break;
        }
        auto value = [&](const char* flag, std::string& out) -> bool {
            if (a == flag) {
                if (i + 1 >= args.size()) { err = std::string("missing value for ") + flag; return false; }
                out = args[++i];
                return true;
            }
            size_t n = std::char_traits<char>::length(flag);
            if (a.size() > n && a.compare(0, n, flag) == 0 && flag[1] != '-') { out = a.substr(n); return true; }
            return false;
        };
        std::string v;
        if (a.size() == 3 && a[0] == '-' && a[1] == 'O' && a[2] >= '0' && a[2] <= '3') o.optLevel = a[2] - '0';
        else if (a == "--no-exceptions") o.exceptions = false;
        else if (a == "--dump-ir") {
            if (i + 1 >= args.size()) { err = "missing value for --dump-ir"; return false; }
            o.dumpIrDir = pathFromU8(args[++i]);
        }
        else if (a == "-v" || a == "--verbose") o.verbose = true;
        else if (a == "--target") {
            if (i + 1 >= args.size()) { err = "missing value for --target"; return false; }
            o.cmakeTarget = args[++i];
        } else if (a == "--std") {
            if (i + 1 >= args.size()) { err = "missing value for --std"; return false; }
            std::string s = args[++i];
            (s.rfind("c++", 0) == 0 || s.rfind("gnu++", 0) == 0 ? o.cxxStd : o.cStd) = s;
        } else if (value("-I", v)) o.extraIncludes.push_back(pathFromU8(v));
        else if (value("-D", v)) o.extraDefines.push_back(v);
        else if (value("-L", v)) o.extraLibDirs.push_back(pathFromU8(v));
        else if (value("-l", v)) o.extraLibs.push_back(v);
        else if (!a.empty() && a[0] == '-' && a.size() > 1) { err = "unknown option: " + a; return false; }
        else o.inputs.push_back(pathFromU8(a));
        if (!err.empty()) return false;
    }
    if (o.inputs.empty()) { err = "no input given"; return false; }
    return true;
}

namespace {

LONG CALLBACK fatalHandler(PEXCEPTION_POINTERS p) {
    DWORD c = p->ExceptionRecord->ExceptionCode;
    const char* what = nullptr;
    switch (c) {
        case EXCEPTION_ACCESS_VIOLATION: what = "access violation"; break;
        case EXCEPTION_STACK_OVERFLOW: what = "stack overflow"; break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO: what = "integer divide by zero"; break;
        case EXCEPTION_ILLEGAL_INSTRUCTION: what = "illegal instruction"; break;
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: what = "array bounds exceeded"; break;
        default: break;
    }
    if (what) {
        std::fprintf(stderr, "\n[CRuntime] fatal: %s (0x%08lX) at address %p\n", what, (unsigned long)c,
                     p->ExceptionRecord->ExceptionAddress);
        std::fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void err(const std::string& s) { std::fprintf(stderr, "[CRuntime] %s\n", s.c_str()); }

}  // namespace

int runPipeline(const RunOptions& o) {
    AddVectoredExceptionHandler(1, fatalHandler);

    if (o.verbose) _putenv_s("CRUNTIME_TRACE", "1");

    // 1) Understand the input.
    ProjectSpec spec;
    std::string e;
    if (!scanInputs(o.inputs, o.cmakeTarget, spec, e)) { err("error: " + e); return 3; }
    for (auto& d : o.extraIncludes) spec.includeDirs.push_back(normalize(d));
    for (auto& d : o.extraLibDirs) spec.libDirs.push_back(normalize(d));
    for (auto& d : o.extraDefines) spec.defines.push_back(d);
    for (auto& l : o.extraLibs) spec.libs.push_back(l);
    if (!o.cxxStd.empty()) spec.cxxStd = o.cxxStd;
    if (!o.cStd.empty()) spec.cStd = o.cStd;
    for (auto& w : spec.warnings) err("warning: " + w);
    err("project '" + spec.name + "': " + std::to_string(spec.sources.size()) + " source file(s)");

    // 2) Toolchain headers.
    Toolchain tc;
    if (!locateToolchain(tc, e)) { err("error: " + e); return 4; }
    if (o.verbose) err("MSVC " + tc.msvcVersion + ", Windows SDK " + tc.sdkVersion);

    // 3) JIT engine.
    auto jit = JitEngine::create(o.optLevel, e);
    if (!jit) { err("error: JIT init failed: " + e); return 5; }

    // 4) Clang frontend: sources -> LLVM IR (memory only).
    CompileOptions co;
    co.optLevel = o.optLevel;
    co.exceptions = o.exceptions;
    co.verbose = o.verbose;
    // Translation units compile in parallel (each has its own LLVMContext); results are reported in source order.
    struct Job { CompiledUnit u; std::string diag; bool ok = false; };
    std::vector<Job> jobs(spec.sources.size());
    {
        struct Ctx { std::atomic<size_t> next{0}; std::vector<Job>* jobs; const ProjectSpec* spec; const Toolchain* tc; const CompileOptions* co; };
        Ctx cx;
        cx.jobs = &jobs; cx.spec = &spec; cx.tc = &tc; cx.co = &co;
        auto work = [](LPVOID p) -> DWORD {
            auto* c = (Ctx*)p;
            for (size_t i; (i = c->next++) < c->jobs->size();) {
                auto& jb = (*c->jobs)[i];
                jb.ok = compileSource(c->spec->sources[i], *c->spec, *c->tc, *c->co, jb.u, jb.diag);
            }
            return 0;
        };
        SYSTEM_INFO si; GetSystemInfo(&si);
        size_t nt = std::min<size_t>(std::min<size_t>(si.dwNumberOfProcessors, 8), jobs.size());
        std::vector<HANDLE> ths;
        for (size_t t = 1; t < nt; ++t) {
            HANDLE h = CreateThread(nullptr, 128u << 20, work, &cx, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
            if (h) ths.push_back(h);
        }
        work(&cx);  // this thread (large stack) participates too
        for (HANDLE h : ths) { WaitForSingleObject(h, INFINITE); CloseHandle(h); }
    }
    std::vector<CompiledUnit> units;
    bool failed = false;
    for (size_t i = 0; i < jobs.size(); ++i) {
        const auto& src = spec.sources[i];
        auto& jb = jobs[i];
        if (o.verbose) err("compiled " + u8(src.path));
        if (!jb.diag.empty()) { std::fputs(jb.diag.c_str(), stderr); if (jb.diag.back() != '\n') std::fputc('\n', stderr); }
        if (jb.ok && !o.dumpIrDir.empty()) {
            std::error_code ec;
            fs::create_directories(o.dumpIrDir, ec);
            std::string path = u8(o.dumpIrDir / (u8(src.path.stem()) + ".ll"));
            llvm::raw_fd_ostream os(path, ec);
            if (!ec) jb.u.mod->print(os, nullptr);
        }
        if (jb.ok) units.push_back(std::move(jb.u));
        else failed = true;
    }
    if (failed) { err("compilation failed"); return 2; }

    // 5) Libraries and modules into the JIT.
    std::vector<fs::path> libDirs = spec.libDirs;
    for (auto& d : tc.libDirs) libDirs.push_back(d);
    for (const auto& l : spec.libs) {
        std::string w;
        if (!jit->addLibrary(l, libDirs, w)) err("warning: " + w);
    }
    for (auto& u : units) {
        if (!jit->addModule(std::move(u), e)) { err("error: " + e); return 5; }
    }

    // 6) Link (lazy, at lookup) and run.
    int rc = 0;
    std::fflush(stderr);
    if (!jit->runEntry(o.programArgs, rc, e)) { err("link/run error: " + e); return 5; }
    std::fflush(stdout);
    std::fflush(stderr);
    err("exited with code " + std::to_string(rc));
    return rc;
}

}  // namespace cr
