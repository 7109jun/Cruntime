#include "Toolchain.h"

#include <windows.h>

#include <cstdlib>

namespace cr {

fs::path executableDir() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    return fs::path(std::wstring(buf, n)).parent_path();
}

namespace {

std::string getEnv(const char* name) {
    char* v = nullptr;
    size_t len = 0;
    if (_dupenv_s(&v, &len, name) != 0 || !v) return {};
    std::string s(v);
    free(v);
    return s;
}

std::vector<int> verParts(const std::string& s) {
    std::vector<int> r;
    std::stringstream ss(s);
    std::string p;
    while (std::getline(ss, p, '.')) r.push_back(std::atoi(p.c_str()));
    return r;
}
bool verLess(const std::string& a, const std::string& b) { return verParts(a) < verParts(b); }

std::vector<fs::path> subdirs(const fs::path& dir) {
    std::vector<fs::path> r;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_directory(e2)) r.push_back(it->path());
    }
    return r;
}
bool exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

std::vector<std::string> splitPaths(const std::string& s) {
    std::vector<std::string> r;
    std::string cur;
    for (char c : s) {
        if (c == ';') { if (!cur.empty()) r.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) r.push_back(cur);
    return r;
}

bool findMsvc(fs::path& inc, fs::path& lib, std::string& ver) {
    std::string vct = getEnv("VCToolsInstallDir");
    if (!vct.empty() && exists(fs::path(vct) / "include" / "vcruntime.h")) {
        fs::path r(vct);
        inc = r / "include";
        lib = r / "lib" / "x64";
        ver = u8(r.parent_path().empty() ? r : (r.has_filename() ? r.filename() : r.parent_path().filename()));
        return true;
    }
    bool found = false;
    for (const char* pf : {"ProgramFiles", "ProgramFiles(x86)"}) {
        std::string base = getEnv(pf);
        if (base.empty()) continue;
        fs::path vs = fs::path(base) / "Microsoft Visual Studio";
        for (const auto& year : subdirs(vs))
            for (const auto& edition : subdirs(year))
                for (const auto& v : subdirs(edition / "VC" / "Tools" / "MSVC")) {
                    if (!exists(v / "include" / "vcruntime.h")) continue;
                    std::string vn = u8(v.filename());
                    if (!found || verLess(ver, vn)) {
                        inc = v / "include";
                        lib = v / "lib" / "x64";
                        ver = vn;
                        found = true;
                    }
                }
    }
    return found;
}

bool findSdk(fs::path& root, std::string& ver) {
    std::vector<fs::path> roots;
    std::string sdkDir = getEnv("WindowsSdkDir");
    if (!sdkDir.empty()) roots.push_back(fs::path(sdkDir));
    for (const char* pf : {"ProgramFiles(x86)", "ProgramFiles"}) {
        std::string b = getEnv(pf);
        if (!b.empty()) roots.push_back(fs::path(b) / "Windows Kits" / "10");
    }
    bool found = false;
    for (const auto& r : roots)
        for (const auto& v : subdirs(r / "Include")) {
            if (!exists(v / "ucrt" / "stdio.h")) continue;
            std::string vn = u8(v.filename());
            if (!found || verLess(ver, vn)) { root = r; ver = vn; found = true; }
        }
    return found;
}

}  // namespace

bool locateToolchain(Toolchain& tc, std::string& err) {
    // Resource headers shipped with CRuntime (read-only).
    fs::path res = executableDir() / "clang_resource";
    std::string envRes = getEnv("CRUNTIME_RESOURCE_DIR");
    if (!envRes.empty()) res = fs::path(envRes);
    if (!exists(res / "include" / "stddef.h")) {
        err = "Clang resource headers not found: " + u8(res / "include") +
              " (copy LLVM's lib/clang/<ver>/include here, or set CRUNTIME_RESOURCE_DIR)";
        return false;
    }
    tc.resourceDir = res;

    // 1) Developer prompt: INCLUDE / LIB already set.
    std::string envInc = getEnv("INCLUDE");
    if (!envInc.empty()) {
        bool hasVc = false;
        for (auto& d : splitPaths(envInc)) {
            tc.includeDirs.push_back(fs::path(d));
            if (exists(fs::path(d) / "vcruntime.h")) hasVc = true;
        }
        if (hasVc) {
            for (auto& d : splitPaths(getEnv("LIB"))) tc.libDirs.push_back(fs::path(d));
            tc.msvcVersion = "env";
            return true;
        }
        tc.includeDirs.clear();
    }

    // 2) Auto-detect Visual Studio / Build Tools + Windows SDK.
    fs::path mi, ml, sdk;
    std::string mv, sv;
    if (!findMsvc(mi, ml, mv)) {
        err = "MSVC headers not found. Install \"Visual Studio Build Tools\" with the C++ workload "
              "(or run from a Developer Command Prompt).";
        return false;
    }
    if (!findSdk(sdk, sv)) {
        err = "Windows 10/11 SDK not found (needs Include\\<ver>\\ucrt). Install it via Visual Studio Installer.";
        return false;
    }
    tc.msvcVersion = mv;
    tc.sdkVersion = sv;
    tc.includeDirs = {mi, sdk / "Include" / sv / "ucrt", sdk / "Include" / sv / "shared", sdk / "Include" / sv / "um"};
    tc.libDirs = {ml, sdk / "Lib" / sv / "ucrt" / "x64", sdk / "Lib" / sv / "um" / "x64"};
    return true;
}

}  // namespace cr
