#pragma once
// Registers the unwind tables (.pdata) of JIT-compiled COFF objects with the OS
// (RtlAddFunctionTable) so SEH and C++ exceptions can unwind through JIT frames.
//
// RuntimeDyld resolves IMAGE_REL_AMD64_ADDR32NB against an "image base" = lowest load address of
// the object's sections; we register the table with that same base. Requires all sections of one
// object to lie within 2 GB of each other (the default allocator places them adjacent).
#include <windows.h>

#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>

namespace cr {

class SehMemoryManager : public llvm::SectionMemoryManager {
public:
    uint8_t* allocateCodeSection(uintptr_t size, unsigned align, unsigned id, llvm::StringRef name) override {
        uint8_t* p = SectionMemoryManager::allocateCodeSection(size, align, id, name);
        track(p, size, name);
        return p;
    }
    uint8_t* allocateDataSection(uintptr_t size, unsigned align, unsigned id, llvm::StringRef name,
                                 bool readOnly) override {
        uint8_t* p = SectionMemoryManager::allocateDataSection(size, align, id, name, readOnly);
        track(p, size, name);
        return p;
    }

    bool finalizeMemory(std::string* err = nullptr) override {
        bool failed = SectionMemoryManager::finalizeMemory(err);
        // Each COMDAT function brings its own ".pdata" section, so an object can have many of them. All entries
        // are relative to the same ImageBase (lowest section address); register every section as its own table.
        if (!failed && lo_ && hi_ - lo_ < (1ull << 31)) {
            for (auto& sec : pdata_) {
                DWORD count = (DWORD)(sec.second / sizeof(RUNTIME_FUNCTION));
                if (!count) continue;
                BOOL ok = RtlAddFunctionTable((PRUNTIME_FUNCTION)sec.first, count, (DWORD64)lo_);
                if (ok) tables_.push_back((PRUNTIME_FUNCTION)sec.first);
                if (trace()) {
                    auto* rf = (PRUNTIME_FUNCTION)sec.first;
                    DWORD64 base = 0;
                    PRUNTIME_FUNCTION found = RtlLookupFunctionEntry(lo_ + rf[0].BeginAddress, &base, nullptr);
                    std::fprintf(stderr, "[seh] table ok=%d count=%lu base=%p first=[%08lx..%08lx] lookup=%s\n", (int)ok,
                                 (unsigned long)count, (void*)lo_, rf[0].BeginAddress, rf[0].EndAddress,
                                 found ? (base == lo_ ? "ok" : "BASE-MISMATCH") : "NOT-FOUND");
                }
            }
            if (trace() && !pdata_.empty())
                std::fprintf(stderr, "[seh] object: %zu .pdata section(s), base=%p span=0x%llx\n", pdata_.size(),
                             (void*)lo_, (unsigned long long)(hi_ - lo_));
        }
        pdata_.clear();
        lo_ = hi_ = 0;
        return failed;
    }

    ~SehMemoryManager() override {
        for (auto* t : tables_) RtlDeleteFunctionTable(t);
    }

private:
    static bool trace() {
        char* v = nullptr;
        size_t n = 0;
        bool on = _dupenv_s(&v, &n, "CRUNTIME_TRACE") == 0 && v;
        free(v);
        return on;
    }
    void track(uint8_t* p, uintptr_t size, llvm::StringRef name) {
        if (!p) return;
        uintptr_t a = (uintptr_t)p;
        lo_ = lo_ ? std::min(lo_, a) : a;
        hi_ = std::max(hi_, a + size);
        if (name == ".pdata") pdata_.push_back({p, size});
    }

    std::vector<std::pair<uint8_t*, uintptr_t>> pdata_;
    uintptr_t lo_ = 0, hi_ = 0;
    std::vector<PRUNTIME_FUNCTION> tables_;
};

}  // namespace cr
