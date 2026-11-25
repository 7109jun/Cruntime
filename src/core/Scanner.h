#pragma once
#include "Common.h"
#include "Project.h"

namespace cr {

bool isCMakeFileName(const fs::path& p);

// Analyse one input (file / folder / CMakeLists.txt / CMake.txt) into a ProjectSpec.
bool scanInput(const fs::path& input, const std::string& cmakeTarget, ProjectSpec& spec, std::string& err);

// Several inputs (e.g. multiple dropped files) merged into one project.
bool scanInputs(const std::vector<fs::path>& inputs, const std::string& cmakeTarget, ProjectSpec& spec,
                std::string& err);

}  // namespace cr
