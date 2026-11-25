#include "Scanner.h"

#include <set>

#include "CMakeInterp.h"

namespace cr {

bool isCMakeFileName(const fs::path& p) {
    std::string n = lower(u8(p.filename()));
    return n == "cmakelists.txt" || n == "cmake.txt";
}

namespace {

bool skipDir(const std::string& name) {
    std::string l = lower(name);
    return l.empty() || l[0] == '.' || l == "build" || l.rfind("build-", 0) == 0 || l.rfind("cmake-build", 0) == 0 ||
           l == "out" || l == "node_modules" || l == "bin" || l == "obj" || l == "debug" || l == "release";
}

void walk(const fs::path& dir, ProjectSpec& spec, std::set<fs::path>& headerDirs, std::set<fs::path>& includeNamed) {
    std::error_code ec;
    std::vector<fs::path> entries;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) entries.push_back(it->path());
    std::sort(entries.begin(), entries.end());
    for (const auto& p : entries) {
        std::error_code e2;
        if (fs::is_directory(p, e2)) {
            std::string name = u8(p.filename());
            if (skipDir(name)) continue;
            if (lower(name) == "include" || lower(name) == "inc") includeNamed.insert(p);
            walk(p, spec, headerDirs, includeNamed);
        } else if (fs::is_regular_file(p, e2)) {
            Lang l;
            if (sourceLang(p, l)) spec.sources.push_back({normalize(p), l});
            else if (isHeaderExt(p)) headerDirs.insert(normalize(p.parent_path()));
        }
    }
}

bool scanDirectory(const fs::path& dir, ProjectSpec& spec, std::string& err) {
    std::set<fs::path> headerDirs, includeNamed;
    spec.name = u8(dir.filename());
    spec.root = dir;
    walk(dir, spec, headerDirs, includeNamed);
    if (spec.sources.empty()) {
        err = "no C/C++ source files (.c/.cpp/.cc/.cxx) found in folder: " + u8(dir);
        return false;
    }
    spec.includeDirs.push_back(dir);
    for (const auto& d : includeNamed) spec.includeDirs.push_back(normalize(d));
    for (const auto& d : headerDirs)
        if (std::find(spec.includeDirs.begin(), spec.includeDirs.end(), d) == spec.includeDirs.end())
            spec.includeDirs.push_back(d);
    return true;
}

}  // namespace

bool scanInput(const fs::path& input, const std::string& cmakeTarget, ProjectSpec& spec, std::string& err) {
    std::error_code ec;
    fs::path in = normalize(input);
    if (!fs::exists(in, ec)) {
        err = "input not found: " + u8(in);
        return false;
    }
    if (fs::is_directory(in, ec)) {
        for (const char* n : {"CMakeLists.txt", "CMake.txt"}) {
            fs::path c = in / n;
            if (fs::exists(c, ec)) return interpretCMake(c, cmakeTarget, spec, err);
        }
        return scanDirectory(in, spec, err);
    }
    if (isCMakeFileName(in)) return interpretCMake(in, cmakeTarget, spec, err);

    // Single file: .c/.cpp/... compile as-is; .h -> C, .hpp/.hh/.hxx -> C++.
    Lang l;
    std::string ext = lower(u8(in.extension()));
    if (sourceLang(in, l)) {
    } else if (ext == ".h") {
        l = Lang::C;
    } else if (isHeaderExt(in)) {
        l = Lang::Cxx;
    } else {
        err = "unsupported input type '" + ext + "' (expected .c .cpp .h .hpp, a folder, or CMakeLists.txt/CMake.txt)";
        return false;
    }
    spec.name = u8(in.stem());
    spec.root = in.parent_path();
    spec.sources.push_back({in, l});
    spec.includeDirs.push_back(in.parent_path());
    return true;
}

bool scanInputs(const std::vector<fs::path>& inputs, const std::string& cmakeTarget, ProjectSpec& spec,
                std::string& err) {
    if (inputs.empty()) {
        err = "no input given";
        return false;
    }
    if (inputs.size() == 1) return scanInput(inputs[0], cmakeTarget, spec, err);
    for (const auto& p : inputs) {
        ProjectSpec s;
        if (!scanInput(p, cmakeTarget, s, err)) return false;
        mergeSpec(spec, s);
    }
    return true;
}

}  // namespace cr
