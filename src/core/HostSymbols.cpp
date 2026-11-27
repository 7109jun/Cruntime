// NOTE: compiled with RTTI enabled (/GR) - see CMakeLists.txt - only to read the type_info vtable address.
#include "HostSymbols.h"
#include <intrin.h>
#include <cstdio>
#include <cstdlib>
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
    if (getenv("CRUNTIME_TRACE")) {
        CONTEXT ctx;
        RtlCaptureContext(&ctx);
        for (int fr = 0; fr < 6; ++fr) {
            DWORD64 ib = 0;
            RUNTIME_FUNCTION* rf = RtlLookupFunctionEntry(ctx.Rip, &ib, nullptr);
            if (!rf) { fprintf(stderr, "[eh] frame%d rip=%p no function entry\n", fr, (void*)ctx.Rip); break; }
            PVOID hdata = nullptr;
            DWORD64 est = 0;
            PEXCEPTION_ROUTINE h = RtlVirtualUnwind(UNW_FLAG_EHANDLER, ib, ctx.Rip, rf, &ctx, &hdata, &est, nullptr);
            bool jit = imageBaseFor((void*)ib) != 0;
            fprintf(stderr, "[eh] frame%d ib=%p func=[%x..%x] jit=%d handler=%p hdata=%p\n", fr, (void*)ib, rf->BeginAddress, rf->EndAddress, (int)jit, (void*)h, hdata);
            if (jit && hdata) {
                unsigned fiRva = *(const unsigned*)hdata;
                const unsigned* fi = (const unsigned*)(ib + fiRva);
                fprintf(stderr, "[eh]   FuncInfo@%x: magic=%x maxState=%d unwindMap=%x nTry=%u tryMap=%x nIp=%u ip2state=%x flags=%x\n", fiRva,
                        fi[0], (int)fi[1], fi[2], fi[3], fi[4], fi[5], fi[6], fi[8]);
                unsigned n = fi[5];
                const unsigned* im = (const unsigned*)(ib + fi[6]);
                for (unsigned k = 0; k < n && k < 8; ++k) fprintf(stderr, "[eh]   ip2state[%u] ip=%x state=%d\n", k, im[2*k], (int)im[2*k+1]);
            }
            if (jit && hdata && fr == 1) {
                unsigned fiRva = *(const unsigned*)hdata;
                const unsigned* fi = (const unsigned*)(ib + fiRva);
                const unsigned* tm = (const unsigned*)(ib + fi[4]);
                fprintf(stderr, "[eh]   try: low=%d high=%d catchHigh=%d nCatch=%d handlers=%x\n", (int)tm[0], (int)tm[1], (int)tm[2], (int)tm[3], tm[4]);
                const unsigned* ha = (const unsigned*)(ib + tm[4]);
                fprintf(stderr, "[eh]   catch: adj=%x pType=%x disp=%d addr=%x frameOff=%x\n", ha[0], ha[1], (int)ha[2], ha[3], ha[4]);
                if (ha[1]) fprintf(stderr, "[eh]   catch type name='%s' vtbl=%p\n", (const char*)(ib + ha[1] + 16), *(void**)(ib + ha[1]));
                const unsigned* ti = (const unsigned*)throwInfo;
                fprintf(stderr, "[eh]   throwInfo: attr=%x unwind=%x fwd=%x cta=%x\n", ti[0], ti[1], ti[2], ti[3]);
                const unsigned* cta = (const unsigned*)(base + ti[3]);
                fprintf(stderr, "[eh]   cta n=%d\n", (int)cta[0]);
                for (int k = 0; k < (int)cta[0] && k < 4; ++k) {
                    const unsigned* ct = (const unsigned*)(base + cta[1 + k]);
                    fprintf(stderr, "[eh]   ct[%d] props=%x pType=%x size=%d copy=%x name='%s' vtbl=%p\n", k, ct[0], ct[1], (int)ct[5], ct[6], (const char*)(base + ct[1] + 16), *(void**)(base + ct[1]));
                }
            }
            if (!jit && fr > 0) break;
        }
    }
    ULONG_PTR params[4] = {0x19930520, (ULONG_PTR)obj, (ULONG_PTR)throwInfo, (ULONG_PTR)base};
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
