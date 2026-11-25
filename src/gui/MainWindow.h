#pragma once
#include <windows.h>

#include <string>
#include <vector>

namespace cr {

// Runs the message loop. `initialInputs` (e.g. files dropped on the exe icon) are loaded and run immediately.
int runGui(HINSTANCE hInst, int showCmd, const std::vector<std::wstring>& initialInputs);

}  // namespace cr
