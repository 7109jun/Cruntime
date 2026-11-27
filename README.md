# CRuntime

C/C++ 소스·프로젝트를 제출하면 **임시 EXE 없이 LLVM ORC JIT로 메모리에서 즉시 실행**하는 Windows 프로그램.
로드맵: [ROADMAP.md](ROADMAP.md)

## 사용

```
CRuntime.exe                              GUI (파일/폴더를 창에 드래그 앤 드롭 → 즉시 실행)
CRuntime.exe run examples\hello.cpp
CRuntime.exe run examples\cproj -O2       CMakeLists.txt 자동 인식
CRuntime.exe run main.c util.c -lws2_32 -- arg1 arg2
옵션: -O0..-O3  --target <name>  -I -D -L -l  --std c++23  --no-exceptions  -v
```

지원 입력: `.c .cpp .cc .cxx .h .hpp`, 프로젝트 폴더(CMake 없으면 소스 자동 수집), `CMakeLists.txt`, `CMake.txt`.
진입점: `main` → `wmain` → `WinMain` → `wWinMain` 순으로 탐지.

## 빌드 (Windows, x64)

1. **LLVM/Clang 18.1.x 를 직접 빌드** (Release, 정적, X86만). 바이너리 설치본에는 Clang 라이브러리/헤더가 없습니다.
   ```
   cmake -S llvm -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
     -DLLVM_ENABLE_PROJECTS=clang -DLLVM_TARGETS_TO_BUILD=X86 ^
     -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_ENABLE_RTTI=OFF ^
     -DCMAKE_INSTALL_PREFIX=C:\llvm18
   cmake --build build --target install
   ```
2. CRuntime 빌드 (VS 2022 x64 Native Tools 프롬프트):
   ```
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
     -DLLVM_DIR=C:\llvm18\lib\cmake\llvm -DClang_DIR=C:\llvm18\lib\cmake\clang
   cmake --build build
   ```
   빌드 후 `clang_resource\include`(Clang 내장 헤더)가 exe 옆에 복사됩니다. 실행 시에는 읽기만 합니다.
3. 실행 PC에는 **Visual Studio Build Tools(C++ 워크로드) + Windows SDK** 가 필요합니다 (표준 라이브러리 헤더/CRT).

## GitHub Actions로 exe 받기
`.github/workflows/build-windows.yml`: 저장소에 올리고 Actions → build-windows → Run workflow. 첫 실행은 LLVM 빌드로 1~2시간, 이후 캐시. 결과물 `CRuntime-win64`(exe + clang_resource + 스모크 테스트 로그)를 다운로드.

## 이식 가능한 코어 테스트 (LLVM 불필요, Linux/Windows)

```
cmake -S . -B build-core -DCRUNTIME_BUILD_CORE_TESTS=ON && cmake --build build-core --target cr_scan_test
build-core/cr_scan_test examples/cproj
```

## 상태
Windows(GitHub Actions, windows-2022)에서 실행 검증 완료: C/C++ 단일·다중 파일, CMake 프로젝트, STL(iostream/vector/string), new/delete, **C++ 예외(throw/catch)**.
GUI(드래그 앤 드롭/중지/stdin)는 빌드만 검증되었고 실사용 테스트는 아직입니다.
최신 exe: Releases → `latest` (CRuntime-win64.zip). 실행 PC에는 VS Build Tools(C++) + Windows SDK 필요.
