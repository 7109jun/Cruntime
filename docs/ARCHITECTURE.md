# 구조

## 전체 흐름

```
CRuntime.exe (GUI) ── 드롭/선택 ──▶ CRuntime.exe run <입력>   (자기 자신을 작업 프로세스로 실행, Job Object로 묶음)
                                       │  stdout/stderr/stdin은 파이프
 Scanner ── 단일 파일 / 폴더 / 여러 파일
   └ CMakeInterp ── CMakeLists.txt / CMake.txt를 메모리에서 해석
 Toolchain ── MSVC STL, UCRT, Windows SDK 헤더 경로 탐지
 Compiler  ── clang::CompilerInstance + EmitLLVMOnlyAction → llvm::Module (메모리)
 JitEngine ── LLJIT: IR 모듈 추가 → 네이티브 코드(메모리) → 진입점 호출
```

GUI가 작업 프로세스를 따로 띄우는 이유는 세 가지다. 종료 버튼을 `TerminateJobObject`로 안전하게 처리할 수 있고, JIT 코드가 죽어도 GUI는 살아 있으며, `exit()`가 GUI를 끝내지 않는다.

## 모듈

| 경로 | 역할 |
|---|---|
| `src/main.cpp` | 진입점. `run` 하위 명령, `--help`, GUI 분기. GUI 서브시스템 exe의 stdio 바인딩 처리 |
| `src/gui/MainWindow.*` | Win32 GUI, 작업 프로세스 실행과 종료, 출력 읽기, 탐색기 연결 등록 |
| `src/core/Scanner.*` | 입력 종류 판별, 소스 수집 |
| `src/core/CMakeInterp.*` | CMake 부분집합 인터프리터 |
| `src/core/Toolchain.*` | `INCLUDE` 환경 변수 또는 VS 설치 폴더에서 헤더 경로 탐지 |
| `src/core/Compiler.*` | 프로세스 내 Clang, 가상 파일 시스템 덧씌우기 |
| `src/core/JitEngine.*` | ORC JIT, 심볼 해석, 라이브러리 로드, 진입점 실행 |
| `src/core/HostSymbols.*` | JIT 코드에 제공하는 호스트 심볼(`operator new/delete`, `type_info`, `_CxxThrowException`) |
| `src/core/SehMemoryManager.h` | JIT 이미지의 언와인드 테이블 등록 |
| `src/core/ImageRegistry.h` | JIT 이미지의 주소 범위와 기준 주소 기록 |
| `src/core/Worker.*` | 작업 프로세스의 전체 파이프라인, 옵션 파싱 |

## 컴파일

- Clang을 드라이버 없이 프로세스 안에서 호출한다. 타깃은 `x86_64-pc-windows-msvc`이고 `-fms-compatibility`를 켠다.
- 소스 파일은 각자 `LLVMContext`를 가지며 여러 스레드에서 병렬로 컴파일한다. 진단은 소스 순서대로 출력한다.
- `configure_file`과 `file(WRITE)`가 만든 파일은 디스크에 쓰지 않고, 메모리 파일 시스템을 실제 파일 시스템 위에 덧씌워 Clang에 전달한다.

## JIT와 심볼 해석

- `RTDyldObjectLinkingLayer`(COFF)와 `SectionMemoryManager`를 상속한 메모리 관리자를 쓴다. 코드 모델은 Large다.
- 심볼은 UCRT를 우선해서 찾는다(`ucrtbase`, `vcruntime140`, `msvcp140` 등). 구형 `msvcrt.dll`의 심볼은 거부한다.
  힙이 서로 달라 `new`와 `free`가 어긋나는 문제를 막기 위해서다.
- `__imp_` 심볼은 호스트가 포인터 셀을 만들어 제공한다.
- `operator new/delete`는 정적 CRT에 있어서 DLL에서 찾을 수 없으므로 호스트가 UCRT 힙 기준으로 직접 제공한다.
- 외부 라이브러리는 `.dll`은 `LoadLibrary`, 정적 `.lib`는 `StaticLibraryDefinitionGenerator`로 연결한다.

## C++ 예외

Windows x64의 예외 처리는 JIT 코드에서 다음이 모두 맞아야 동작한다.

1. 각 오브젝트의 `.pdata`를 `RtlAddFunctionTable`로 등록한다. 기준 주소는 RuntimeDyld의 ImageBase(가장 낮은 섹션 주소)와 같아야 한다. 섹션 끝의 0 패딩은 잘라 내고 정렬 상태를 유지한다.
2. 예외 핸들러 주소는 32비트 RVA로 저장되는데, 실제 `__CxxFrameHandler3`는 4GB 밖에 있을 수 있다. 모듈마다 내부 전달 스텁을 만들어 RVA가 JIT 이미지 안을 가리키게 한다.
3. `_CxxThrowException`은 ThrowInfo의 이미지 기준 주소를 `RtlPcToFileHeader`로 구하는데 JIT 메모리에서는 얻을 수 없다. JIT 이미지 안의 ThrowInfo는 호스트가 직접 예외를 일으키며, 이때 매직 값은 `0x19930520`을 쓴다.

## 작업 프로세스와 GUI

- GUI는 `CRuntime.exe run ...`을 Job Object 안에서 실행하고 `KILL_ON_JOB_CLOSE`로 잔여 프로세스를 정리한다.
- 출력은 파이프로 읽어 UTF-8을 변환해 출력창에 붙인다. 표준 입력은 입력창의 한 줄씩 파이프로 쓴다.
- GUI 서브시스템 exe는 콘솔이 없으므로, 이미 열린 CRT 파일 디스크립터는 건드리지 않고 사용할 수 없는 것만 다시 연결한다.
