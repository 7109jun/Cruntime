// NOTE: compiled with RTTI enabled (/GR) - see CMakeLists.txt - only to read the type_info vtable address.
#include "HostSymbols.h"
#include "ImageRegistry.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <typeinfo>

namespace cr {
namespace {

void* hostNew(size_t n) { return ::operator new(n); }
void* hostNewArray(size_t n) { return ::operator new[](n); }
void hostDelete(void* p) { ::operator delete(p); }
void hostDeleteArray(void* p) { ::operator delete[](p); }
void* hostNewNothrow(size_t n, const std::nothrow_t& t) { return ::operator new(n, t); }
void* hostNewArrayNothrow(size_t n, const std::nothrow_t& t) { return ::operator new[](n, t); }
void hostDeleteNothrow(void* p, const std::nothrow_t&) { ::operator delete(p); }
void hostDeleteArrayNothrow(void* p, const std::nothrow_t&) { ::operator delete[](p); }
void sizedDelete(void* p, size_t) { ::operator delete(p); }
void sizedDeleteArray(void* p, size_t) { ::operator delete[](p); }
void* alignedNew(size_t n, std::align_val_t a) { return ::operator new(n, a); }
void* alignedNewArray(size_t n, std::align_val_t a) { return ::operator new[](n, a); }
void alignedDelete(void* p, std::align_val_t a) { ::operator delete(p, a); }
void alignedDeleteArray(void* p, std::align_val_t a) { ::operator delete[](p, a); }
void sizedAlignedDelete(void* p, size_t, std::align_val_t a) { ::operator delete(p, a); }
void sizedAlignedDeleteArray(void* p, size_t, std::align_val_t a) { ::operator delete[](p, a); }

// MSVC's _CxxThrowException derives the exception's ImageBase with RtlPcToFileHeader(ThrowInfo), which is NULL for
// JIT memory (not a PE image) -> every RVA in ThrowInfo is wrong. For ThrowInfo inside a JIT image we raise the C++
// exception ourselves with that image's base; everything else (host throws, `throw;` rethrow) goes to the real one.
using ThrowFn = void(__stdcall*)(void*, void*);
void __stdcall jitCxxThrow(void* obj, void* throwInfo) {
    static ThrowFn real = (ThrowFn)GetProcAddress(GetModuleHandleA("vcruntime140.dll"), "_CxxThrowException");
    uintptr_t base = throwInfo ? imageBaseFor(throwInfo) : 0;
    if (!base) {
        real(obj, throwInfo);
        return;
    }
    ULONG_PTR params[4] = {0x01994000, (ULONG_PTR)obj, (ULONG_PTR)throwInfo, (ULONG_PTR)base};
    RaiseException(0xE06D7363, EXCEPTION_NONCONTINUABLE, 4, params);
}

void* typeInfoVtable() {
    const std::type_info& ti = typeid(int);
    return *reinterpret_cast<void* const*>(&ti);  // vptr of a type_info object == address of its vftable
}

}  // namespace

namespace {
bool traceOn() {
    char* v = nullptr;
    size_t n = 0;
    bool on = _dupenv_s(&v, &n, "CRUNTIME_TRACE") == 0 && v;
    free(v);
    return on;
}
}  // namespace

std::vector<HostSym> hostSymbols() {
    // operator new/delete are NOT exported by ucrtbase/vcruntime140/msvcp140 (they live in the static part of the
    // CRT). The legacy system msvcrt.dll does export them, but it has its own heap - binding to it makes
    // new (msvcrt) / free (UCRT) mismatch. So the host provides every variant, all on the UCRT heap.
    std::vector<HostSym> v = {
        {"??_7type_info@@6B@", typeInfoVtable()},
        {"_CxxThrowException", (void*)&jitCxxThrow},
        {"??2@YAPEAX_K@Z", (void*)&hostNew},
        {"??_U@YAPEAX_K@Z", (void*)&hostNewArray},
        {"??3@YAXPEAX@Z", (void*)&hostDelete},
        {"??_V@YAXPEAX@Z", (void*)&hostDeleteArray},
        {"??3@YAXPEAX_K@Z", (void*)&sizedDelete},
        {"??_V@YAXPEAX_K@Z", (void*)&sizedDeleteArray},
        {"??2@YAPEAX_KAEBUnothrow_t@std@@@Z", (void*)&hostNewNothrow},
        {"??_U@YAPEAX_KAEBUnothrow_t@std@@@Z", (void*)&hostNewArrayNothrow},
        {"??3@YAXPEAXAEBUnothrow_t@std@@@Z", (void*)&hostDeleteNothrow},
        {"??_V@YAXPEAXAEBUnothrow_t@std@@@Z", (void*)&hostDeleteArrayNothrow},
        {"??2@YAPEAX_KW4align_val_t@std@@@Z", (void*)&alignedNew},
        {"??_U@YAPEAX_KW4align_val_t@std@@@Z", (void*)&alignedNewArray},
        {"??3@YAXPEAXW4align_val_t@std@@@Z", (void*)&alignedDelete},
        {"??_V@YAXPEAXW4align_val_t@std@@@Z", (void*)&alignedDeleteArray},
        {"??3@YAXPEAX_KW4align_val_t@std@@@Z", (void*)&sizedAlignedDelete},
        {"??_V@YAXPEAX_KW4align_val_t@std@@@Z", (void*)&sizedAlignedDeleteArray},
    };
    if (traceOn())
        for (const auto& h : v) std::fprintf(stderr, "[sym] host %-40s -> %p\n", h.name, h.addr);
    return v;
}

}  // namespace cr
