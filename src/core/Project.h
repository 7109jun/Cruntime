#pragma once
#include "Common.h"

namespace cr {

struct SourceFile {
    fs::path path;
    Lang lang = Lang::Cxx;
};

// Everything the compiler/JIT needs to know about the user's program.
struct ProjectSpec {
    std::string name;
    fs::path root;
    std::vector<SourceFile> sources;
    std::vector<fs::path> includeDirs;
    std::vector<std::string> defines;        // "NAME" or "NAME=VALUE"
    std::vector<std::string> compileOptions; // raw options (filtered at compile time)
    std::vector<fs::path> libDirs;
    std::vector<std::string> libs;           // names or paths (.lib/.dll)
    std::string cStd;                        // e.g. "c17" (empty = default)
    std::string cxxStd;                      // e.g. "c++20" (empty = default)
    std::vector<std::string> warnings;
    // Files generated in memory by the CMake interpreter (configure_file, file(WRITE)): path -> content.
    std::vector<std::pair<fs::path, std::string>> virtualFiles;
};

// Merge b into a (used when several files are dropped at once).
inline void mergeSpec(ProjectSpec& a, const ProjectSpec& b) {
    if (a.name.empty()) a.name = b.name;
    if (a.root.empty()) a.root = b.root;
    auto addUnique = [](auto& dst, const auto& src) {
        for (const auto& x : src)
            if (std::find(dst.begin(), dst.end(), x) == dst.end()) dst.push_back(x);
    };
    for (const auto& s : b.sources) {
        bool dup = false;
        for (const auto& t : a.sources) if (t.path == s.path) { dup = true; break; }
        if (!dup) a.sources.push_back(s);
    }
    addUnique(a.includeDirs, b.includeDirs);
    addUnique(a.defines, b.defines);
    addUnique(a.compileOptions, b.compileOptions);
    addUnique(a.libDirs, b.libDirs);
    addUnique(a.libs, b.libs);
    if (a.cStd.empty()) a.cStd = b.cStd;
    if (a.cxxStd.empty()) a.cxxStd = b.cxxStd;
    for (const auto& w : b.warnings) a.warnings.push_back(w);
    for (const auto& v : b.virtualFiles) a.virtualFiles.push_back(v);
}

}  // namespace cr
