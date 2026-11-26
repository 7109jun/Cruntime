// NOTE: compiled with RTTI enabled (/GR) - see CMakeLists.txt - only to read the type_info vtable address.
#include "HostSymbols.h"

#include <new>
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

std::vector<HostSym> hostSymbols() {
    return {
        {"??_7type_info@@6B@", typeInfoVtable()},
        {"??3@YAXPEAX_K@Z", (void*)&sizedDelete},
        {"??_V@YAXPEAX_K@Z", (void*)&sizedDeleteArray},
        {"??2@YAPEAX_KW4align_val_t@std@@@Z", (void*)&alignedNew},
        {"??_U@YAPEAX_KW4align_val_t@std@@@Z", (void*)&alignedNewArray},
        {"??3@YAXPEAXW4align_val_t@std@@@Z", (void*)&alignedDelete},
        {"??_V@YAXPEAXW4align_val_t@std@@@Z", (void*)&alignedDeleteArray},
        {"??3@YAXPEAX_KW4align_val_t@std@@@Z", (void*)&sizedAlignedDelete},
        {"??_V@YAXPEAX_KW4align_val_t@std@@@Z", (void*)&sizedAlignedDeleteArray},
    };
}

}  // namespace cr
