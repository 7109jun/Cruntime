#pragma once
#include "Common.h"
#include "Project.h"

namespace cr {

// In-memory interpreter for a practical subset of CMake. Never writes to disk and
// never launches cmake. Fills `spec` for the selected executable target
// (wantedTarget empty = first add_executable).
bool interpretCMake(const fs::path& file, const std::string& wantedTarget, ProjectSpec& spec, std::string& err);

}  // namespace cr
