// Portable test driver: prints what CRuntime would compile for an input.
//   cr_scan_test <file|folder|CMakeLists.txt> [cmake-target]
#include <cstdio>

#include "Scanner.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: cr_scan_test <input> [target]\n");
        return 2;
    }
    cr::ProjectSpec spec;
    std::string err;
    if (!cr::scanInput(cr::pathFromU8(argv[1]), argc > 2 ? argv[2] : "", spec, err)) {
        std::printf("ERROR: %s\n", err.c_str());
        return 1;
    }
    std::printf("name: %s\nroot: %s\ncStd: %s\ncxxStd: %s\n", spec.name.c_str(), cr::u8(spec.root).c_str(),
                spec.cStd.c_str(), spec.cxxStd.c_str());
    for (auto& s : spec.sources)
        std::printf("source[%s]: %s\n", s.lang == cr::Lang::C ? "C" : "C++", cr::u8(s.path).c_str());
    for (auto& i : spec.includeDirs) std::printf("include: %s\n", cr::u8(i).c_str());
    for (auto& d : spec.defines) std::printf("define: %s\n", d.c_str());
    for (auto& o : spec.compileOptions) std::printf("option: %s\n", o.c_str());
    for (auto& d : spec.libDirs) std::printf("libdir: %s\n", cr::u8(d).c_str());
    for (auto& l : spec.libs) std::printf("lib: %s\n", l.c_str());
    for (auto& w : spec.warnings) std::printf("warning: %s\n", w.c_str());
    return 0;
}
