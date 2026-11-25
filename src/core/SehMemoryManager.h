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
        if (!failed && pdata_ && pdataSize_ >= sizeof(RUNTIME_FUNCTION) && lo_ && hi_ - lo_ < (1ull << 31)) {
            DWORD count = (DWORD)(pdataSize_ / sizeof(RUNTIME_FUNCTION));
            if (RtlAddFunctionTable((PRUNTIME_FUNCTION)pdata_, count, (DWORD64)lo_))
                tables_.push_back((PRUNTIME_FUNCTION)pdata_);
        }
        pdata_ = nullptr;
        pdataSize_ = 0;
        lo_ = hi_ = 0;
        return failed;
    }

    ~SehMemoryManager() override {
        for (auto* t : tables_) RtlDeleteFunctionTable(t);
    }

private:
    void track(uint8_t* p, uintptr_t size, llvm::StringRef name) {
        if (!p) return;
        uintptr_t a = (uintptr_t)p;
        lo_ = lo_ ? std::min(lo_, a) : a;
        hi_ = std::max(hi_, a + size);
        if (name == ".pdata") { pdata_ = p; pdataSize_ = size; }
    }

    uint8_t* pdata_ = nullptr;
    uintptr_t pdataSize_ = 0, lo_ = 0, hi_ = 0;
    std::vector<PRUNTIME_FUNCTION> tables_;
};

}  // namespace cr
