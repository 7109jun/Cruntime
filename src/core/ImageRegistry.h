#pragma once
// Address ranges of JIT-loaded "images" (one per object file) and their RuntimeDyld ImageBase.
// MSVC x64 C++ exceptions store 32-bit RVAs in ThrowInfo/FuncInfo; they must be resolved against the right base.
#include <cstdint>
#include <mutex>
#include <vector>

namespace cr {

struct ImageRange {
    uintptr_t lo, hi;
};
inline std::mutex& imageMutex() {
    static std::mutex m;
    return m;
}
inline std::vector<ImageRange>& imageRanges() {
    static std::vector<ImageRange> v;
    return v;
}
inline void registerImage(uintptr_t lo, uintptr_t hi) {
    std::lock_guard<std::mutex> g(imageMutex());
    imageRanges().push_back({lo, hi});
}
// ImageBase of the JIT image containing `p`, or 0.
inline uintptr_t imageBaseFor(const void* p) {
    std::lock_guard<std::mutex> g(imageMutex());
    uintptr_t a = (uintptr_t)p;
    for (const auto& r : imageRanges())
        if (a >= r.lo && a < r.hi) return r.lo;
    return 0;
}

}  // namespace cr
