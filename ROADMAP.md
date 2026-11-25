# CRuntime 개발 로드맵

확정 결정: **자체 CMake 인터프리터**(디스크 쓰기 0, cmake 불필요) · **LLVM 직접 빌드 + 정적 링크**, 표준 라이브러리 헤더는 설치된 **VS Build Tools + Windows SDK** 사용.

## 아키텍처

```
CRuntime.exe (GUI)  ── 드롭/선택 ──▶  CRuntime.exe run <input>   (자기 자신을 작업 프로세스로 실행, Job Object로 묶음)
                                         │  stdout/stderr/stdin = 파이프 (디스크 파일 없음)
   Scanner ─ 단일 파일 / 폴더 / 여러 파일
      └─ CMakeInterp ─ CMakeLists.txt / CMake.txt (메모리 내 해석)
   Toolchain ─ MSVC STL · UCRT · SDK 경로 탐지 (INCLUDE/LIB 또는 VS 설치 탐색)
   Compiler  ─ clang::CompilerInstance + EmitLLVMOnlyAction  → llvm::Module (메모리)
   JitEngine ─ LLJIT: IR 모듈 추가 → 네이티브 코드(메모리) → initialize → main 호출
                ├ DynamicLibrarySearchGenerator (프로세스/CRT DLL 심볼)
                ├ ImportGenerator (__imp_ 심볼 합성)
                └ 외부 라이브러리: .dll(LoadLibrary) / .lib(StaticLibraryDefinitionGenerator)
```

작업 프로세스 분리 이유: `종료` = `TerminateJobObject`로 안전 종료, JIT 코드가 죽어도 GUI는 생존, `exit()`/크래시 격리.

## 단계

| 단계 | 내용 | 상태 |
|---|---|---|
| M0 | 프로젝트 골격, CMake 빌드, manifest | 완료 |
| M1 | 입력 분석(Scanner) + CMake 인터프리터 (if/foreach/function/macro/file(GLOB)/add_subdirectory/타깃 속성 전파) | 완료 · Linux에서 검증 |
| M2 | Clang 프론트엔드 → IR, 진단 출력 | 작성 완료 · **Windows 빌드 검증 필요** |
| M3 | ORC LLJIT, 다중 모듈 링크, __imp_ 처리, 외부 라이브러리, 진입점 탐지 | 작성 완료 · **Windows 빌드 검증 필요** |
| M4 | Win32 GUI: 드래그앤드롭, 실행/종료, 출력/stdin, 옵션 | 작성 완료 · **Windows 빌드 검증 필요** |
| M5 | 안정화: LLVM 버전 고정 후 컴파일 오류 수정, 예제 전체 실행 | 다음 |
| M6 | **C++ 예외/SEH 언와인드**: `SehMemoryManager`가 JIT 객체의 `.pdata`를 `RtlAddFunctionTable`로 등록 | 작성 완료 · LLVM 18 헤더 컴파일 검증, **Windows 실행 검증 필요** (`examples\exceptions.cpp`) |
| M7 | 병렬 컴파일(TU별 스레드), 증분 캐시(메모리 한정) | 계획 |
| M8 | CMake 확장: `find_package` 일부, 제너레이터 식 확대, `configure_file`(메모리 내) | 계획 |

## 알려진 제약
- 대상: x64 Windows, LLVM/Clang **18.x–19.x** (API 버전 의존).
- CMake는 인터프리터 부분집합. 지원하지 않는 명령은 경고 후 무시.
- `.h` 단일 입력은 C, `.hpp/.hh/.hxx`는 C++ 로 컴파일(헤더 안에 `main`이 있어야 실행됨).
- 사용자 PC에 VS Build Tools(C++) + Windows SDK 필요.
