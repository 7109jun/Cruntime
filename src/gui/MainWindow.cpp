#include "MainWindow.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <memory>

namespace cr {
namespace {

enum {
    IDC_FILE = 101, IDC_DIR, IDC_RUN, IDC_STOP, IDC_CLEAR, IDC_OPT, IDC_ARGS, IDC_OUT, IDC_IN, IDC_PATH, IDC_STATUS,
    IDC_LBL_OPT, IDC_LBL_ARGS, IDC_LBL_IN,
    IDM_ASSOC_ON = 201, IDM_ASSOC_OFF, IDM_INSTALL_MSVC
};
constexpr UINT WM_APP_OUT = WM_APP + 1;   // wParam = generation, lParam = std::wstring*
constexpr UINT WM_APP_EXIT = WM_APP + 2;  // wParam = generation, lParam = exit code

struct ReaderCtx {
    HWND hwnd;
    HANDLE pipe;
    HANDLE proc;
    unsigned gen;
};

struct App {
    HWND hwnd = nullptr, out = nullptr, in = nullptr, args = nullptr, path = nullptr, status = nullptr;
    HWND combo = nullptr, bRun = nullptr, bStop = nullptr;
    HFONT ui = nullptr, mono = nullptr;
    HANDLE hProc = nullptr, hJob = nullptr, hStdin = nullptr;
    std::atomic<unsigned> gen{0};
    bool running = false;
    ULONGLONG t0 = 0;
    std::vector<std::wstring> inputs;
} g;


std::wstring exePath() {
    wchar_t b[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, b, (DWORD)std::size(b));
    return std::wstring(b, n);
}
bool regSet(const std::wstring& key, const wchar_t* name, const std::wstring& val) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, (L"Software\\Classes\\" + key).c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k,
                        nullptr) != ERROR_SUCCESS)
        return false;
    LONG r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val.c_str(), (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}
const wchar_t* kExts[] = {L".c", L".cpp", L".cc", L".cxx"};

// Per-user (HKCU, no admin): double-click opens CRuntime and runs the file; also an always-available
// "CRuntime으로 실행" context-menu entry (Windows may keep another program as the default opener).
bool registerAssoc() {
    std::wstring exe = exePath();
    std::wstring cmd = L"\"" + exe + L"\" \"%1\"";
    bool ok = regSet(L"CRuntime.Source", nullptr, L"C/C++ 소스 (CRuntime)");
    ok &= regSet(L"CRuntime.Source\\DefaultIcon", nullptr, exe + L",0");
    ok &= regSet(L"CRuntime.Source\\shell\\open", nullptr, L"CRuntime으로 실행");
    ok &= regSet(L"CRuntime.Source\\shell\\open\\command", nullptr, cmd);
    for (auto e : kExts) {
        ok &= regSet(e, nullptr, L"CRuntime.Source");
        ok &= regSet(std::wstring(e) + L"\\OpenWithProgids", L"CRuntime.Source", L"");
        std::wstring v = std::wstring(L"SystemFileAssociations\\") + e + L"\\shell\\CRuntime";
        ok &= regSet(v, nullptr, L"CRuntime으로 실행");
        ok &= regSet(v, L"Icon", exe + L",0");
        ok &= regSet(v + L"\\command", nullptr, cmd);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok;
}
void unregisterAssoc() {
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\CRuntime.Source");
    for (auto e : kExts) {
        RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\SystemFileAssociations\\") + e + L"\\shell\\CRuntime").c_str());
        HKEY k;
        std::wstring key = std::wstring(L"Software\\Classes\\") + e;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
            wchar_t v[64]; DWORD n = sizeof(v);
            if (RegQueryValueExW(k, nullptr, nullptr, nullptr, (BYTE*)v, &n) == ERROR_SUCCESS && wcscmp(v, L"CRuntime.Source") == 0)
                RegDeleteValueW(k, nullptr);
            RegCloseKey(k);
        }
        RegDeleteKeyValueW(HKEY_CURRENT_USER, (key + L"\\OpenWithProgids").c_str(), L"CRuntime.Source");
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

// Installs VS Build Tools (C++ workload, includes the Windows SDK) via winget in a visible console window.
void installMsvc(HWND owner) {
    if (MessageBoxW(owner,
                    L"표준 라이브러리 헤더(MSVC + Windows SDK)가 이 PC에 없습니다.\n"
                    L"Visual Studio Build Tools(C++ 작업 로드)를 지금 설치할까요? (약 3~6GB, 관리자 권한 필요)\n"
                    L"설치가 끝나면 다시 실행하세요.",
                    L"CRuntime", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    const wchar_t* args =
        L"/c winget install --id Microsoft.VisualStudio.2022.BuildTools -e --accept-package-agreements --accept-source-agreements "
        L"--override \"--passive --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended\" & pause";
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.hwnd = owner;
    sei.lpVerb = L"runas";
    sei.lpFile = L"cmd.exe";
    sei.lpParameters = args;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei))
        MessageBoxW(owner, L"설치를 시작하지 못했습니다. https://visualstudio.microsoft.com/downloads/ 에서\n\"Build Tools for Visual Studio\"를 받아 C++ 데스크톱 개발 작업 로드를 설치하세요.", L"CRuntime", MB_OK | MB_ICONWARNING);
}

std::wstring getText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s((size_t)n + 1, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    s.resize((size_t)n);
    return s;
}
std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
void setStatus(const std::wstring& s) { SetWindowTextW(g.status, s.c_str()); }

void appendOutput(const std::wstring& text) {
    // Edit controls want CRLF.
    std::wstring t;
    t.reserve(text.size() + 16);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r')) t += L'\r';
        t += text[i];
    }
    SendMessageW(g.out, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageW(g.out, EM_REPLACESEL, FALSE, (LPARAM)t.c_str());
}
void logLine(const std::wstring& s) { appendOutput(s + L"\n"); }

// Longest prefix of `s` that does not end in the middle of a UTF-8 sequence.
size_t validUtf8Prefix(const std::string& s) {
    size_t n = s.size();
    for (size_t back = 1; back <= 3 && back <= n; ++back) {
        unsigned char c = (unsigned char)s[n - back];
        if ((c & 0xC0) == 0x80) continue;  // continuation byte
        size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        return need > back ? n - back : n;
    }
    return n;
}

void setRunningUi(bool running) {
    g.running = running;
    EnableWindow(g.bRun, !running);
    EnableWindow(g.bStop, running);
    EnableWindow(g.in, running);
}

void updatePathLabel() {
    std::wstring s;
    for (size_t i = 0; i < g.inputs.size(); ++i) { if (i) s += L"; "; s += g.inputs[i]; }
    SetWindowTextW(g.path, s.c_str());
}

// ----------------------------------------------------------- child process ---
DWORD WINAPI readerThread(LPVOID p) {
    std::unique_ptr<ReaderCtx> c((ReaderCtx*)p);
    std::string carry;
    char buf[8192];
    DWORD n = 0;
    while (ReadFile(c->pipe, buf, sizeof(buf), &n, nullptr) && n > 0) {
        carry.append(buf, n);
        size_t k = validUtf8Prefix(carry);
        if (k == 0) continue;
        int wn = MultiByteToWideChar(CP_UTF8, 0, carry.data(), (int)k, nullptr, 0);
        auto* w = new std::wstring((size_t)wn, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, carry.data(), (int)k, w->data(), wn);
        carry.erase(0, k);
        if (!PostMessageW(c->hwnd, WM_APP_OUT, c->gen, (LPARAM)w)) delete w;
    }
    WaitForSingleObject(c->proc, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(c->proc, &code);
    PostMessageW(c->hwnd, WM_APP_EXIT, c->gen, (LPARAM)(int)code);
    CloseHandle(c->pipe);
    return 0;
}

void closeChild() {
    if (g.hStdin) { CloseHandle(g.hStdin); g.hStdin = nullptr; }
    if (g.hProc) { CloseHandle(g.hProc); g.hProc = nullptr; }
    if (g.hJob) { CloseHandle(g.hJob); g.hJob = nullptr; }  // KILL_ON_JOB_CLOSE reaps leftovers
}

void stopRun() {
    if (!g.running) return;
    if (g.hJob) TerminateJobObject(g.hJob, 1);
    else if (g.hProc) TerminateProcess(g.hProc, 1);
}

void startRun() {
    if (g.running) return;
    if (g.inputs.empty()) { setStatus(L"먼저 파일 또는 폴더를 선택하거나 드롭하세요."); return; }

    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, (DWORD)(sizeof(exe) / sizeof(exe[0])));
    int opt = (int)SendMessageW(g.combo, CB_GETCURSEL, 0, 0);
    if (opt < 0) opt = 2;

    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" run";
    for (auto in : g.inputs) {
        while (in.size() > 3 && (in.back() == L'\\' || in.back() == L'/')) in.pop_back();
        cmd += L" \"" + in + L"\"";
    }
    cmd += L" -O" + std::to_wstring(opt);
    std::wstring pargs = getText(g.args);
    if (!pargs.empty()) cmd += L" -- " + pargs;

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE outR = nullptr, outW = nullptr, inR = nullptr, inW = nullptr;
    if (!CreatePipe(&outR, &outW, &sa, 0) || !CreatePipe(&inR, &inW, &sa, 0)) {
        setStatus(L"파이프 생성 실패");
        return;
    }
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR;
    si.hStdOutput = outW;
    si.hStdError = outW;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                             nullptr, nullptr, &si, &pi);
    CloseHandle(outW);
    CloseHandle(inR);
    if (!ok) {
        CloseHandle(outR);
        CloseHandle(inW);
        setStatus(L"실행 프로세스를 시작하지 못했습니다.");
        return;
    }

    g.hJob = CreateJobObjectW(nullptr, nullptr);
    if (g.hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g.hJob, JobObjectExtendedLimitInformation, &info, sizeof(info));
        AssignProcessToJobObject(g.hJob, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    g.hProc = pi.hProcess;
    g.hStdin = inW;
    unsigned gen = ++g.gen;
    g.t0 = GetTickCount64();
    setRunningUi(true);
    setStatus(L"실행 중...");
    auto* ctx = new ReaderCtx{g.hwnd, outR, nullptr, gen};
    HANDLE dup = nullptr;
    DuplicateHandle(GetCurrentProcess(), g.hProc, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
    ctx->proc = dup;
    HANDLE th = CreateThread(nullptr, 0, readerThread, ctx, 0, nullptr);
    if (th) CloseHandle(th);
}

void sendInputLine() {
    if (!g.running || !g.hStdin) return;
    std::wstring line = getText(g.in);
    SetWindowTextW(g.in, L"");
    logLine(L"> " + line);
    std::string u = toUtf8(line) + "\n";
    DWORD w = 0;
    WriteFile(g.hStdin, u.data(), (DWORD)u.size(), &w, nullptr);
}

// ------------------------------------------------------------------ dialogs ---
bool pickPaths(HWND owner, bool folder, std::vector<std::wstring>& out) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    opts |= folder ? FOS_PICKFOLDERS : (FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM);
    dlg->SetOptions(opts);
    if (!folder) {
        COMDLG_FILTERSPEC fs[] = {
            {L"C/C++ 소스 · CMake", L"*.c;*.cpp;*.cc;*.cxx;*.h;*.hpp;CMakeLists.txt;CMake.txt"},
            {L"모든 파일", L"*.*"}};
        dlg->SetFileTypes(2, fs);
    }
    bool ok = false;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItemArray* arr = nullptr;
        if (SUCCEEDED(dlg->GetResults(&arr))) {
            DWORD n = 0;
            arr->GetCount(&n);
            for (DWORD i = 0; i < n; ++i) {
                IShellItem* it = nullptr;
                if (SUCCEEDED(arr->GetItemAt(i, &it))) {
                    PWSTR p = nullptr;
                    if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out.push_back(p); CoTaskMemFree(p); ok = true; }
                    it->Release();
                }
            }
            arr->Release();
        }
    }
    dlg->Release();
    return ok;
}

void setInputs(std::vector<std::wstring> v, bool autoRun) {
    if (v.empty()) return;
    if (g.running) { setStatus(L"실행 중입니다. 먼저 종료하세요."); return; }
    g.inputs = std::move(v);
    updatePathLabel();
    if (autoRun) startRun();
}

// ------------------------------------------------------------------- layout ---
void layout(int w, int h) {
    const int m = 8, bh = 28, gap = 6;
    int x = m, y = m;
    auto place = [&](int id, int width) {
        SetWindowPos(GetDlgItem(g.hwnd, id), nullptr, x, y, width, bh, SWP_NOZORDER);
        x += width + gap;
    };
    place(IDC_FILE, 96);
    place(IDC_DIR, 96);
    place(IDC_RUN, 72);
    place(IDC_STOP, 72);
    place(IDC_CLEAR, 72);
    x += 12;
    place(IDC_LBL_OPT, 62);
    place(IDC_OPT, 64);
    y += bh + gap;
    SetWindowPos(g.path, nullptr, m, y, w - 2 * m, bh, SWP_NOZORDER);
    y += bh + gap;
    SetWindowPos(GetDlgItem(g.hwnd, IDC_LBL_ARGS), nullptr, m, y + 4, 64, bh, SWP_NOZORDER);
    SetWindowPos(g.args, nullptr, m + 68, y, w - 2 * m - 68, bh, SWP_NOZORDER);
    y += bh + gap;
    int bottom = h - m - bh - gap - 22;
    SetWindowPos(g.out, nullptr, m, y, w - 2 * m, std::max(40, bottom - y), SWP_NOZORDER);
    int iy = bottom + gap;
    SetWindowPos(GetDlgItem(g.hwnd, IDC_LBL_IN), nullptr, m, iy + 4, 64, bh, SWP_NOZORDER);
    SetWindowPos(g.in, nullptr, m + 68, iy, w - 2 * m - 68, bh, SWP_NOZORDER);
    SetWindowPos(g.status, nullptr, m, h - 22 - 4, w - 2 * m, 22, SWP_NOZORDER);
}

LRESULT CALLBACK dropSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_DROPFILES) { SendMessageW(g.hwnd, WM_DROPFILES, wp, lp); return 0; }
    return DefSubclassProc(h, msg, wp, lp);
}
LRESULT CALLBACK inputSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN && wp == VK_RETURN) { sendInputLine(); return 0; }
    if (msg == WM_CHAR && wp == VK_RETURN) return 0;
    return DefSubclassProc(h, msg, wp, lp);
}

HWND mk(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g.hwnd, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g.ui, TRUE);
    return h;
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g.hwnd = hwnd;
            HDC dc = GetDC(nullptr);
            int dpi = GetDeviceCaps(dc, LOGPIXELSY);
            ReleaseDC(nullptr, dc);
            g.ui = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                               0, L"Segoe UI");
            g.mono = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                 CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
            mk(L"BUTTON", L"파일 열기...", BS_PUSHBUTTON | WS_TABSTOP, IDC_FILE);
            mk(L"BUTTON", L"폴더 열기...", BS_PUSHBUTTON | WS_TABSTOP, IDC_DIR);
            g.bRun = mk(L"BUTTON", L"▶ 실행", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_RUN);
            g.bStop = mk(L"BUTTON", L"■ 종료", BS_PUSHBUTTON | WS_TABSTOP, IDC_STOP);
            mk(L"BUTTON", L"지우기", BS_PUSHBUTTON | WS_TABSTOP, IDC_CLEAR);
            mk(L"STATIC", L"최적화:", SS_LEFT, IDC_LBL_OPT);
            g.combo = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_OPT);
            for (auto s : {L"O0", L"O1", L"O2", L"O3"}) SendMessageW(g.combo, CB_ADDSTRING, 0, (LPARAM)s);
            SendMessageW(g.combo, CB_SETCURSEL, 2, 0);
            g.path = mk(L"EDIT", L"여기에 파일/폴더를 드래그 앤 드롭하세요", ES_AUTOHSCROLL | ES_READONLY, IDC_PATH,
                        WS_EX_CLIENTEDGE);
            mk(L"STATIC", L"인자:", SS_LEFT, IDC_LBL_ARGS);
            g.args = mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_ARGS, WS_EX_CLIENTEDGE);
            g.out = mk(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL,
                       IDC_OUT, WS_EX_CLIENTEDGE);
            SendMessageW(g.out, WM_SETFONT, (WPARAM)g.mono, TRUE);
            SendMessageW(g.out, EM_SETLIMITTEXT, 0, 0);
            mk(L"STATIC", L"입력(stdin):", SS_LEFT, IDC_LBL_IN);
            g.in = mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_IN, WS_EX_CLIENTEDGE);
            g.status = mk(L"STATIC", L"준비됨", SS_LEFT, IDC_STATUS);
            for (HWND h : {g.hwnd, g.out, g.path, g.args}) DragAcceptFiles(h, TRUE);
            SetWindowSubclass(g.out, dropSubclass, 1, 0);
            SetWindowSubclass(g.path, dropSubclass, 1, 0);
            SetWindowSubclass(g.args, dropSubclass, 1, 0);
            SetWindowSubclass(g.in, inputSubclass, 2, 0);
            setRunningUi(false);
            HMENU bar = CreateMenu(), set = CreatePopupMenu();
            AppendMenuW(set, MF_STRING, IDM_ASSOC_ON, L".c/.cpp 더블클릭으로 실행 (탐색기 연결 등록)");
            AppendMenuW(set, MF_STRING, IDM_ASSOC_OFF, L"탐색기 연결 해제");
            AppendMenuW(set, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(set, MF_STRING, IDM_INSTALL_MSVC, L"Build Tools(C++/SDK) 설치...");
            AppendMenuW(bar, MF_POPUP, (UINT_PTR)set, L"설정");
            SetMenu(hwnd, bar);
            return 0;
        }
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) layout(LOWORD(lp), HIWORD(lp));
            return 0;
        case WM_GETMINMAXINFO:
            ((MINMAXINFO*)lp)->ptMinTrackSize = {640, 420};
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_FILE: {
                    std::vector<std::wstring> v;
                    if (pickPaths(hwnd, false, v)) setInputs(std::move(v), false);
                    return 0;
                }
                case IDC_DIR: {
                    std::vector<std::wstring> v;
                    if (pickPaths(hwnd, true, v)) setInputs(std::move(v), false);
                    return 0;
                }
                case IDC_RUN: startRun(); return 0;
                case IDC_STOP: stopRun(); return 0;
                case IDC_CLEAR: SetWindowTextW(g.out, L""); return 0;
                case IDM_ASSOC_ON:
                    MessageBoxW(hwnd, registerAssoc() ? L"등록했습니다. .c/.cpp 파일을 더블클릭하면 실행됩니다.\n(다른 프로그램이 기본값이면 우클릭 → \"CRuntime으로 실행\")"
                                                      : L"등록에 실패했습니다.", L"CRuntime", MB_OK);
                    return 0;
                case IDM_ASSOC_OFF: unregisterAssoc(); MessageBoxW(hwnd, L"연결을 해제했습니다.", L"CRuntime", MB_OK); return 0;
                case IDM_INSTALL_MSVC: installMsvc(hwnd); return 0;
            }
            break;
        case WM_DROPFILES: {
            HDROP d = (HDROP)wp;
            UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::wstring> v;
            for (UINT i = 0; i < n; ++i) {
                UINT len = DragQueryFileW(d, i, nullptr, 0);
                std::wstring s((size_t)len + 1, L'\0');
                DragQueryFileW(d, i, s.data(), len + 1);
                s.resize(len);
                v.push_back(s);
            }
            DragFinish(d);
            setInputs(std::move(v), true);
            return 0;
        }
        case WM_APP_OUT: {
            std::unique_ptr<std::wstring> w((std::wstring*)lp);
            if ((unsigned)wp == g.gen.load()) appendOutput(*w);
            return 0;
        }
        case WM_APP_EXIT: {
            if ((unsigned)wp != g.gen.load()) return 0;
            double sec = (double)(GetTickCount64() - g.t0) / 1000.0;
            wchar_t b[160];
            swprintf_s(b, L"종료됨 — 종료 코드 %d (%.2f초)", (int)lp, sec);
            logLine(std::wstring(L"\n[") + b + L"]");
            setStatus(b);
            closeChild();
            setRunningUi(false);
            if ((int)lp == 4) installMsvc(hwnd);  // toolchain headers missing
            return 0;
        }
        case WM_CLOSE:
            if (g.running && MessageBoxW(hwnd, L"실행 중인 프로그램을 종료하고 닫을까요?", L"CRuntime",
                                          MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
            stopRun();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            closeChild();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int runGui(HINSTANCE hInst, int showCmd, const std::vector<std::wstring>& initialInputs) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"CRuntimeMainWindow";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    HWND h = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"CRuntime — C/C++ JIT Runner", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 960, 640, nullptr, nullptr, hInst, nullptr);
    if (!h) return 1;
    ShowWindow(h, showCmd);
    UpdateWindow(h);
    if (!initialInputs.empty()) setInputs(initialInputs, true);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    CoUninitialize();
    return (int)m.wParam;
}

}  // namespace cr
