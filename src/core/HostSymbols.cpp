// NOTE: compiled with RTTI enabled (/GR) - see CMakeLists.txt - only to read the type_info vtable address.
#include "HostSymbols.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <typeinfo>

namespace cr {
namespace {

void sizedDelete(void* p, size_t) { ::operator delete(p); }
void sizedDeleteArray(void* p, size_t) { ::operator delete[](p); }
void* alignedNew(size_t n, std::align_val_t a) { return ::operator new(n, a); }
void* alignedNewArray(size_t n, std::align_val_t a) { return ::operator new[](n, a); }
void alignedDelete(void* p, std::align_val_t a) { ::operator delete(p, a); }
void alignedDeleteArray(void* p, std::align_val_t a) { ::operator delete[](p, a); }
void sizedAlignedDelete(void* p, size_t, std::align_val_t a) { ::operator delete(p, a); }
void sizedAlignedDeleteArray(void* p, size_t, std::align_val_t a) { ::operator delete[](p, a); }

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
void* exported(const char* name) {
    for (const char* dll : {"vcruntime140.dll", "ucrtbase.dll", "msvcp140.dll"}) {
        HMODULE m = GetModuleHandleA(dll);
        if (!m) m = LoadLibraryA(dll);
        if (m)
            if (void* p = (void*)GetProcAddress(m, name)) return p;
    }
    return nullptr;
}
std::string moduleOf(void* addr) {
    HMODULE m = nullptr;
    char buf[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &m))
        GetModuleFileNameA(m, buf, MAX_PATH);
    return buf;
}
}  // namespace

std::vector<HostSym> hostSymbols() {
    std::vector<HostSym> v = {
        {"??_7type_info@@6B@", typeInfoVtable()},
        {"??2@YAPEAX_KW4align_val_t@std@@@Z", (void*)&alignedNew},
        {"??_U@YAPEAX_KW4align_val_t@std@@@Z", (void*)&alignedNewArray},
        {"??3@YAXPEAXW4align_val_t@std@@@Z", (void*)&alignedDelete},
        {"??_V@YAXPEAXW4align_val_t@std@@@Z", (void*)&alignedDeleteArray},
        {"??3@YAXPEAX_KW4align_val_t@std@@@Z", (void*)&sizedAlignedDelete},
        {"??_V@YAXPEAX_KW4align_val_t@std@@@Z", (void*)&sizedAlignedDeleteArray},
    };
    // Sized delete == unsized delete + an ignored 2nd argument (x64 ABI): alias to the DLL's exported operator delete
    // so allocation and deallocation always go through exactly the same implementation as `operator new`.
    if (void* d = exported("??3@YAXPEAX@Z")) v.push_back({"??3@YAXPEAX_K@Z", d});
    else v.push_back({"??3@YAXPEAX_K@Z", (void*)&sizedDelete});
    if (void* d = exported("??_V@YAXPEAX@Z")) v.push_back({"??_V@YAXPEAX_K@Z", d});
    else v.push_back({"??_V@YAXPEAX_K@Z", (void*)&sizedDeleteArray});

    if (traceOn()) {
        for (const auto& h : v) std::fprintf(stderr, "[sym] %-40s -> %p  %s\n", h.name, h.addr, moduleOf(h.addr).c_str());
        for (const char* n : {"??2@YAPEAX_K@Z", "??3@YAXPEAX@Z", "??_U@YAPEAX_K@Z", "??_V@YAXPEAX@Z"}) {
            void* p = exported(n);
            std::fprintf(stderr, "[sym] %-40s -> %p  %s\n", n, p, p ? moduleOf(p).c_str() : "(not exported)");
        }
    }
    return v;
}

}  // namespace cr
