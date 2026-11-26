#pragma once
#include <string>
#include <vector>

#include "Common.h"

namespace cr {

struct RunOptions {
    std::vector<fs::path> inputs;
    std::string cmakeTarget;
    int optLevel = 2;
    bool exceptions = true;
    bool verbose = false;
    fs::path dumpIrDir;  // --dump-ir <dir>: write each unit's LLVM IR (.ll) there (debugging aid, opt-in)
    std::vector<fs::path> extraIncludes, extraLibDirs;
    std::vector<std::string> extraDefines, extraLibs;
    std::string cStd, cxxStd;
    std::vector<std::string> programArgs;
};

// Parse "run" sub-command arguments (UTF-8). Returns false with `err` on bad usage.
bool parseRunArgs(const std::vector<std::string>& args, RunOptions& opt, std::string& err);

// Whole pipeline: scan -> Clang IR -> ORC JIT -> run entry. Returns the program's exit code, or a
// CRuntime error code (2 compile, 3 input, 4 toolchain, 5 JIT/link).
int runPipeline(const RunOptions& opt);

const char* usageText();

}  // namespace cr
