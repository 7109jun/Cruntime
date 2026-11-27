# CRuntime

C/C++ 소스와 프로젝트를 **임시 EXE 없이, 메모리에서 바로 실행**하는 Windows 프로그램이다.
Clang 프론트엔드로 LLVM IR을 만들고, LLVM ORC JIT으로 네이티브 코드를 메모리에 올려 진입점을 호출한다. 디스크에는 아무것도 쓰지 않는다.

- 로드맵: [ROADMAP.md](ROADMAP.md)
- 구조: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- CMake 지원 범위: [docs/CMAKE.md](docs/CMAKE.md)
- 문제 해결: [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md)

## 받기

저장소의 Releases에서 `latest`의 `CRuntime-win64.zip`을 받는다. 이 파일은 GitHub Actions가 `main`에 푸시될 때마다 다시 만든다.
압축 안에는 `CRuntime.exe`, `clang_resource`(Clang 내장 헤더), 예제, 스모크 테스트 로그가 들어 있다.

## 요구 사항

- Windows 10/11 x64
- **Visual Studio Build Tools(C++ 작업 로드)와 Windows SDK.** 표준 라이브러리 헤더(`stdio.h`, `<iostream>` 등)를 여기서 읽는다.
  없으면 종료 코드 4로 끝나며, GUI는 설치 안내 창(`winget` 사용)을 띄운다.

## 사용

### GUI

`CRuntime.exe`를 실행한 뒤 파일이나 폴더를 창에 드래그 앤 드롭하면 바로 실행된다.
파일 열기, 폴더 열기, 실행, 종료, 지우기 버튼과 최적화 단계(O0~O3), 프로그램 인자, 표준 입력 창이 있다.
종료 버튼은 실행 중인 프로그램을 즉시 끝낸다. 프로그램이 멈추거나 죽어도 GUI는 영향을 받지 않는다.

### 더블클릭으로 실행

메뉴 **설정 → ".c/.cpp 더블클릭으로 실행 (탐색기 연결 등록)"**을 한 번 누른다.
이후 `.c`, `.cpp`, `.cc`, `.cxx` 파일을 더블클릭하면 CRuntime이 열리면서 바로 실행된다.
관리자 권한은 필요 없고, 해제는 같은 메뉴에서 한다.
다른 프로그램이 기본 앱으로 지정된 경우에는 파일을 우클릭해 **"CRuntime으로 실행"**을 고른다.

### 명령줄

```
CRuntime.exe run examples\hello.cpp
CRuntime.exe run examples\cproj -O2
CRuntime.exe run main.c util.c -lws2_32 -- arg1 arg2
```

| 옵션 | 설명 |
|---|---|
| `-O0` ~ `-O3` | 최적화 단계(기본 `-O2`) |
| `--target <이름>` | CMake 프로젝트에서 실행할 실행 파일 타깃 |
| `-I<dir>`, `-D<M[=V]>` | 추가 인클루드 경로, 매크로 |
| `-L<dir>`, `-l<lib>` | 추가 라이브러리 경로, 라이브러리(`.lib`/`.dll` 이름 또는 경로) |
| `--std <표준>` | 소스 언어의 표준(`c17`, `c++23` 등) |
| `--no-exceptions` | C++ 예외 없이 컴파일 |
| `--dump-ir <dir>` | 소스별 LLVM IR(`.ll`)을 해당 폴더에 기록(디버깅용) |
| `-v` | 자세한 출력 |
| `--` | 이후 인자는 실행되는 프로그램에 그대로 전달 |

입력으로는 `.c .cpp .cc .cxx .h .hpp`, 프로젝트 폴더, `CMakeLists.txt`, `CMake.txt`를 줄 수 있다. 폴더에 CMake 파일이 없으면 소스를 자동으로 모은다.
진입점은 `main`, `wmain`, `WinMain`, `wWinMain` 순서로 찾는다.
종료 코드는 컴파일 실패 2, 입력 오류 3, 툴체인 없음 4, JIT/링크 실패 5이며, 그 외에는 프로그램의 종료 코드를 따른다.

## 현재 상태

GitHub Actions(windows-2022)에서 다음 항목의 실행을 검증했다.

- C와 C++ 단일 파일, 여러 파일, CMake 프로젝트
- 표준 라이브러리(`iostream`, `vector`, `string`), `new`/`delete`
- C++ 예외(`throw`/`catch`)
- 여러 소스 파일의 병렬 컴파일
- `configure_file`, `file(WRITE)` 등 메모리 내 파일 생성

GUI의 드래그 앤 드롭, 종료 버튼, 표준 입력, 탐색기 연결은 빌드만 확인했고 실제 PC에서의 사용 검증은 아직 하지 않았다.

## 직접 빌드

1. LLVM/Clang 18.1.x를 소스에서 빌드한다(Release, X86만, RTTI 끔, `/MD`). 공식 바이너리에는 Clang 라이브러리와 헤더가 없어서 쓸 수 없다.
   ```
   cmake -S llvm -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
     -DLLVM_ENABLE_PROJECTS=clang -DLLVM_TARGETS_TO_BUILD=X86 ^
     -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_ENABLE_RTTI=OFF ^
     -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL -DCMAKE_INSTALL_PREFIX=C:\llvm18
   cmake --build build --target install
   ```
2. VS 2022 x64 Native Tools 프롬프트에서 CRuntime을 빌드한다.
   ```
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
     -DLLVM_DIR=C:\llvm18\lib\cmake\llvm -DClang_DIR=C:\llvm18\lib\cmake\clang
   cmake --build build
   ```
   빌드가 끝나면 `clang_resource\include`가 exe 옆에 복사된다.

`.github/workflows/build-windows.yml`이 같은 과정을 자동화한다. 첫 실행은 LLVM 빌드 때문에 오래 걸리고, 이후에는 캐시를 쓴다.

## 이식 가능한 코어 테스트

입력 분석기와 CMake 인터프리터는 LLVM 없이 Linux에서도 빌드해 시험할 수 있다.

```
cmake -S . -B build-core -DCRUNTIME_BUILD_CORE_TESTS=ON
cmake --build build-core --target cr_scan_test
build-core/cr_scan_test examples/cproj
```

## 한계

- x64 Windows만 지원한다.
- CMake는 인터프리터가 이해하는 부분집합만 지원한다([docs/CMAKE.md](docs/CMAKE.md)).
- 실행하는 PC에 Build Tools와 Windows SDK가 있어야 한다.
