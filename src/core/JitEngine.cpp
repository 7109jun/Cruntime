#include "JitEngine.h"

#include <windows.h>

#include "HostSymbols.h"
#include "SehMemoryManager.h"

#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/Module.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>

using namespace llvm;
using namespace llvm::orc;

namespace cr {
namespace {

std::string errStr(Error e) { return toString(std::move(e)); }

// ---- symbol resolution --------------------------------------------------------------------------------------
// Never use "search every module in the process": the legacy system msvcrt.dll is loaded by the OS and also exports
// malloc/free/operator new/..., with a different heap than the UCRT the user program is compiled against.
// Instead look in a fixed, ordered list: user libraries first, then the UCRT/VC runtime, then Windows system DLLs.
std::vector<HMODULE> g_modules;

void initModules() {
    g_modules.clear();
    for (const char* dll : {"ucrtbase.dll", "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll", "msvcp140_1.dll",
                            "msvcp140_2.dll", "concrt140.dll", "ntdll.dll", "kernelbase.dll", "kernel32.dll",
                            "user32.dll", "gdi32.dll", "gdi32full.dll", "advapi32.dll", "shell32.dll", "ws2_32.dll",
                            "ole32.dll", "oleaut32.dll", "comdlg32.dll", "comctl32.dll", "winmm.dll", "shlwapi.dll",
                            "bcrypt.dll", "crypt32.dll", "imm32.dll", "version.dll", "psapi.dll", "dbghelp.dll"}) {
        HMODULE m = GetModuleHandleA(dll);
        if (!m) m = LoadLibraryA(dll);
        if (m) g_modules.push_back(m);
    }
}

bool isLegacyCrt(void* addr) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr, &m))
        return false;
    char buf[MAX_PATH] = {};
    GetModuleFileNameA(m, buf, MAX_PATH);
    std::string f = lower(buf);
    size_t sl = f.find_last_of("\\/");
    return (sl == std::string::npos ? f : f.substr(sl + 1)) == "msvcrt.dll";
}

void* findSymbol(const std::string& name) {
    for (HMODULE m : g_modules)
        if (void* p = (void*)GetProcAddress(m, name.c_str())) return isLegacyCrt(p) ? nullptr : p;
    return nullptr;
}

class ProcessSymbols : public DefinitionGenerator {
public:
    Error tryToGenerate(LookupState&, LookupKind, JITDylib& JD, JITDylibLookupFlags,
                        const SymbolLookupSet& Names) override {
        SymbolMap syms;
        for (const auto& kv : Names) {
            StringRef n = *kv.first;
            if (n.starts_with("__imp_")) continue;  // ImportGenerator
            if (void* a = findSymbol(n.str())) syms[kv.first] = {ExecutorAddr::fromPtr(a), JITSymbolFlags::Exported};
        }
        if (syms.empty()) return Error::success();
        return JD.define(absoluteSymbols(std::move(syms)));
    }
};

// Windows DLL imports: code compiled against dllimport declarations references
// `__imp_<name>` (a pointer cell holding the function address). Synthesise those cells.
class ImportGenerator : public DefinitionGenerator {
public:
    Error tryToGenerate(LookupState&, LookupKind, JITDylib& JD, JITDylibLookupFlags,
                        const SymbolLookupSet& Names) override {
        SymbolMap syms;
        for (const auto& kv : Names) {
            StringRef n = *kv.first;
            if (!n.starts_with("__imp_")) continue;
            std::string base = n.drop_front(6).str();
            void* addr = findSymbol(base);
            if (!addr) continue;
            void** cell = new void*(addr);  // lives for the whole JIT session
            cells_.push_back(cell);
            syms[kv.first] = {ExecutorAddr::fromPtr(cell), JITSymbolFlags::Exported};
        }
        if (syms.empty()) return Error::success();
        return JD.define(absoluteSymbols(std::move(syms)));
    }
    ~ImportGenerator() override {
        for (void** c : cells_) delete c;
    }

private:
    std::vector<void**> cells_;
};

void optimizeModule(Module& M, int level) {
    LoopAnalysisManager LAM;
    FunctionAnalysisManager FAM;
    CGSCCAnalysisManager CGAM;
    ModuleAnalysisManager MAM;
    PassBuilder PB;
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
    OptimizationLevel ol = level <= 1 ? OptimizationLevel::O1 : (level == 2 ? OptimizationLevel::O2 : OptimizationLevel::O3);
    ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(ol);
    MPM.run(M, MAM);
}

int g_fltused = 1;

}  // namespace

std::unique_ptr<JitEngine> JitEngine::create(int optLevel, std::string& err) {
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();

    auto jtmb = JITTargetMachineBuilder::detectHost();
    if (!jtmb) { err = errStr(jtmb.takeError()); return nullptr; }
    jtmb->setCodeModel(CodeModel::Large);  // JIT'd code may land >2GB away from host DLLs
    jtmb->setCodeGenOptLevel(optLevel <= 0 ? CodeGenOptLevel::None
                             : optLevel == 1 ? CodeGenOptLevel::Less
                             : optLevel == 2 ? CodeGenOptLevel::Default
                                             : CodeGenOptLevel::Aggressive);

    // RTDyld (COFF) linking layer with a memory manager that registers .pdata so exceptions unwind.
    auto jit = LLJITBuilder()
                   .setJITTargetMachineBuilder(std::move(*jtmb))
                   .setObjectLinkingLayerCreator(
                       [](ExecutionSession& es, const Triple&) -> Expected<std::unique_ptr<ObjectLayer>> {
                           auto layer = std::make_unique<RTDyldObjectLinkingLayer>(
                               es, []() -> std::unique_ptr<RuntimeDyld::MemoryManager> {
                                   return std::make_unique<SehMemoryManager>();
                               });
                           layer->setOverrideObjectFlagsWithResponsibilityFlags(true);
                           layer->setAutoClaimResponsibilityForObjectSymbols(true);
                           return std::unique_ptr<ObjectLayer>(std::move(layer));
                       })
                   .create();
    if (!jit) { err = errStr(jit.takeError()); return nullptr; }

    std::unique_ptr<JitEngine> e(new JitEngine());
    e->jit_ = std::move(*jit);
    e->optLevel_ = optLevel;

    JITDylib& jd = e->jit_->getMainJITDylib();
    ExecutionSession& es = e->jit_->getExecutionSession();

    initModules();
    jd.addGenerator(std::make_unique<ImportGenerator>());
    jd.addGenerator(std::make_unique<ProcessSymbols>());

    // Compiler-support symbols MSVC-ABI object code expects.
    SymbolMap abs;
    abs[es.intern("_fltused")] = {ExecutorAddr::fromPtr(&g_fltused), JITSymbolFlags::Exported};
    for (const auto& hs : hostSymbols())
        abs[es.intern(hs.name)] = {ExecutorAddr::fromPtr(hs.addr), JITSymbolFlags::Exported};
    if (auto er = jd.define(absoluteSymbols(std::move(abs)))) { err = errStr(std::move(er)); return nullptr; }
    return e;
}

JitEngine::~JitEngine() {
    if (jit_ && initialized_) consumeError(jit_->deinitialize(jit_->getMainJITDylib()));
}

bool JitEngine::addModule(CompiledUnit&& unit, std::string& err) {
    if (optLevel_ > 0) optimizeModule(*unit.mod, optLevel_);
    ThreadSafeModule tsm(std::move(unit.mod), std::move(unit.ctx));
    if (auto e = jit_->addIRModule(std::move(tsm))) { err = errStr(std::move(e)); return false; }
    return true;
}

bool JitEngine::addLibrary(const std::string& lib, const std::vector<fs::path>& dirs, std::string& warning) {
    std::error_code ec;
    fs::path p = pathFromU8(lib);
    std::string ext = lower(u8(p.extension()));
    bool isPath = p.has_parent_path();

    auto loadDll = [&](const fs::path& dll) -> bool {
        HMODULE m = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!m) return false;
        g_modules.insert(g_modules.begin(), m);  // user libraries take precedence
        return true;
    };
    auto find = [&](const std::string& fname) -> fs::path {
        for (const auto& d : dirs) {
            fs::path c = d / fname;
            if (fs::exists(c, ec)) return c;
        }
        return {};
    };

    fs::path file;
    if (isPath && fs::exists(p, ec)) file = p;
    else if (!isPath && (ext == ".lib" || ext == ".dll")) file = find(u8(p));
    else if (!isPath && ext.empty()) {
        file = find(lib + ".lib");
        if (file.empty()) file = find(lib + ".dll");
    }

    std::string stem = u8(file.empty() ? p.stem() : file.stem());
    if (!file.empty() && lower(u8(file.extension())) == ".dll") {
        if (loadDll(file)) return true;
        warning = "cannot load DLL: " + u8(file);
        return false;
    }
    if (!file.empty()) {
        auto gen = StaticLibraryDefinitionGenerator::Load(jit_->getObjLinkingLayer(), u8(file).c_str());
        if (gen) {
            jit_->getMainJITDylib().addGenerator(std::move(*gen));
            return true;
        }
        consumeError(gen.takeError());  // probably an import library -> use the DLL of the same name
    }
    // Import library / system library: load <stem>.dll from the system or next to the .lib.
    if (!file.empty() && loadDll(file.parent_path() / (stem + ".dll"))) return true;
    if (loadDll(stem + ".dll")) return true;
    warning = "library not resolved: " + lib;
    return false;
}

bool JitEngine::runEntry(const std::vector<std::string>& args, int& exitCode, std::string& err) {
    JITDylib& jd = jit_->getMainJITDylib();
    if (auto e = jit_->initialize(jd)) { err = errStr(std::move(e)); return false; }
    initialized_ = true;

    struct Entry { const char* name; int kind; };
    static const Entry entries[] = {{"main", 0}, {"wmain", 1}, {"WinMain", 2}, {"wWinMain", 3}};
    std::string lookupErrors;
    for (const auto& en : entries) {
        auto sym = jit_->lookup(en.name);
        if (!sym) {
            std::string m = errStr(sym.takeError());
            // "Symbols not found: [ main ]" -> try next entry; anything else is a real link error.
            if (m.find(std::string("Symbols not found: [ ") + en.name + " ]") != std::string::npos) continue;
            err = m;
            return false;
        }
        std::vector<std::string> full;
        full.push_back("program");
        full.insert(full.end(), args.begin(), args.end());

        if (en.kind == 0) {
            std::vector<char*> av;
            for (auto& s : full) av.push_back(s.data());
            av.push_back(nullptr);
            auto fn = sym->toPtr<int (*)(int, char**)>();
            exitCode = fn((int)full.size(), av.data());
        } else if (en.kind == 1) {
            std::vector<std::wstring> w;
            for (auto& s : full) {
                int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
                std::wstring ws((size_t)std::max(n, 1) - 1, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, ws.data(), n);
                w.push_back(std::move(ws));
            }
            std::vector<wchar_t*> av;
            for (auto& s : w) av.push_back(s.data());
            av.push_back(nullptr);
            auto fn = sym->toPtr<int (*)(int, wchar_t**)>();
            exitCode = fn((int)w.size(), av.data());
        } else if (en.kind == 2) {
            std::string cmd;
            for (size_t i = 0; i < args.size(); ++i) { if (i) cmd += ' '; cmd += args[i]; }
            auto fn = sym->toPtr<int (*)(HINSTANCE, HINSTANCE, LPSTR, int)>();
            exitCode = fn(GetModuleHandleW(nullptr), nullptr, cmd.data(), SW_SHOWDEFAULT);
        } else {
            std::string cmd;
            for (size_t i = 0; i < args.size(); ++i) { if (i) cmd += ' '; cmd += args[i]; }
            int n = MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, nullptr, 0);
            std::wstring ws((size_t)std::max(n, 1) - 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, ws.data(), n);
            auto fn = sym->toPtr<int (*)(HINSTANCE, HINSTANCE, LPWSTR, int)>();
            exitCode = fn(GetModuleHandleW(nullptr), nullptr, ws.data(), SW_SHOWDEFAULT);
        }
        return true;
    }
    err = "no entry point found (define main, wmain, WinMain or wWinMain)";
    return false;
}

}  // namespace cr
