// CRuntime entry point.
//   CRuntime.exe                       -> GUI
//   CRuntime.exe <files...>            -> GUI, runs the dropped/passed inputs immediately
//   CRuntime.exe run <input>... [opts] -> CLI / worker (the GUI launches itself in this mode so the
//                                          JIT'd program is isolated and can be stopped safely)
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/Worker.h"
#include "gui/MainWindow.h"

namespace {

std::string toUtf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)std::max(n, 1) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

void __cdecl ignoreInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {}

// GUI-subsystem exe. When stdio handles are inherited (pipes/files from the GUI or a shell redirect) the CRT has
// ALREADY opened fd 0..2 on them - re-wrapping the same handle and _dup2()-ing over it would close the handle and
// make the CRT fail-fast (0xC0000409). So only bind the fds that are not usable, falling back to the parent console.
void bindStdio() {
    _set_invalid_parameter_handler(ignoreInvalidParameter);  // CRT misuse returns an error instead of fast-failing
    AttachConsole(ATTACH_PARENT_PROCESS);
    SetConsoleOutputCP(CP_UTF8);
    const DWORD ids[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    for (int fd = 0; fd < 3; ++fd) {
        intptr_t cur = _get_osfhandle(fd);
        if (cur != -1 && cur != -2 && GetFileType((HANDLE)cur) != FILE_TYPE_UNKNOWN) continue;  // already usable
        HANDLE h = GetStdHandle(ids[fd]);
        if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) {
            int nfd = _open_osfhandle((intptr_t)h, fd == 0 ? _O_RDONLY : _O_WRONLY);
            if (nfd >= 0 && nfd != fd) { _dup2(nfd, fd); _close(nfd); }
        } else {
            FILE* f = nullptr;
            if (fd == 0) freopen_s(&f, "CONIN$", "r", stdin);
            else freopen_s(&f, "CONOUT$", "w", fd == 1 ? stdout : stderr);
        }
    }
    // Live output: no buffering so the GUI / console sees text immediately.
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
}

struct ThreadArg {
    cr::RunOptions opt;
    int rc = 0;
};
DWORD WINAPI pipelineThread(LPVOID p) {
    auto* a = (ThreadArg*)p;
    a->rc = cr::runPipeline(a->opt);
    return 0;
}

int cliRun(const std::vector<std::string>& args) {
    bindStdio();
    ThreadArg ta;
    std::string err;
    if (!cr::parseRunArgs(args, ta.opt, err)) {
        std::fprintf(stderr, "[CRuntime] %s\n\n%s", err.c_str(), cr::usageText());
        return 64;
    }
    // Large stack: deep template instantiation in Clang and deep recursion in user programs.
    HANDLE th = CreateThread(nullptr, 256u << 20, pipelineThread, &ta, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!th) return 70;
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    std::fflush(nullptr);
    return ta.rc;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int show) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 2 && wcscmp(argv[1], L"run") == 0) {
        std::vector<std::string> a;
        for (int i = 2; i < argc; ++i) a.push_back(toUtf8(argv[i]));
        int rc = cliRun(a);
        LocalFree(argv);
        return rc;
    }
    if (argc >= 2 && (wcscmp(argv[1], L"--help") == 0 || wcscmp(argv[1], L"-h") == 0 || wcscmp(argv[1], L"/?") == 0)) {
        bindStdio();
        std::fputs(cr::usageText(), stdout);
        LocalFree(argv);
        return 0;
    }
    std::vector<std::wstring> inputs;
    for (int i = 1; i < argc; ++i) inputs.push_back(argv[i]);
    LocalFree(argv);
    return cr::runGui(hInst, show, inputs);
}
