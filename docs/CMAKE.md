# CMake 지원 범위

CRuntime은 cmake를 실행하지 않고 자체 인터프리터로 `CMakeLists.txt`를 읽는다. 목적은 어떤 소스를 어떤 옵션으로 컴파일하고 무엇과 링크할지 알아내는 것이다. 디스크에는 아무것도 쓰지 않는다.

## 지원하는 것

- 흐름 제어: `if`/`elseif`/`else`, `foreach`, `while`, `function`, `macro`, `return`, `break`, `continue`
- 변수와 문자열: `set`, `unset`, `option`, `list`, `string`, `math(EXPR)`, `message`, `get_filename_component`
- 파일 탐색: `file(GLOB)`, `file(GLOB_RECURSE)`, `file(READ)`, `file(TO_CMAKE_PATH)`, `file(REAL_PATH)`
- 타깃: `add_executable`, `add_library`, `target_sources`, `target_include_directories`, `target_compile_definitions`, `target_compile_options`, `target_link_directories`, `target_link_libraries`, `set_target_properties`
- 전역 설정: `include_directories`, `link_directories`, `link_libraries`, `add_definitions`, `add_compile_definitions`, `add_compile_options`
- 구성: `project`, `include`, `add_subdirectory`, `find_package(Threads)`
- 메모리 내 파일 생성: `configure_file`(`@VAR@`, `${VAR}`, `#cmakedefine`, `#cmakedefine01`, `COPYONLY`, `@ONLY`), `file(WRITE)`, `file(APPEND)`
- 제너레이터 식: `BUILD_INTERFACE`, `LINK_ONLY`, `CONFIG`, `CXX_COMPILER_ID`, `C_COMPILER_ID`, `PLATFORM_ID`, `COMPILE_LANGUAGE`, `BOOL`, `IF`, `NOT`, `AND`, `OR`, `STREQUAL`, `EQUAL`, `TARGET_EXISTS`, `JOIN`

## 메모리 내 파일

`configure_file`과 `file(WRITE/APPEND)`의 결과는 파일로 쓰지 않는다. 인터프리터가 내용을 보관하고, Clang이 `#include`로 읽을 때 메모리 파일 시스템에서 제공한다.
빌드 폴더 변수(`CMAKE_BINARY_DIR`, `CMAKE_CURRENT_BINARY_DIR`, `PROJECT_BINARY_DIR`)는 `<소스 루트>\.cruntime-build`라는 가상 경로를 가리킨다. 이 폴더는 실제로 만들어지지 않는다.
생성된 헤더를 쓰려면 일반 CMake와 똑같이 `include_directories(${CMAKE_CURRENT_BINARY_DIR})`로 경로를 추가한다.

## 지원하지 않는 것

다음 명령은 경고를 출력하고 무시한다. 프로그램 실행에는 영향이 없다.

- `add_custom_target`, `add_custom_command`
- `file(COPY)`, `file(REMOVE)`, `file(MAKE_DIRECTORY)`, `file(DOWNLOAD)`, `file(GENERATE)` 등 디스크를 바꾸는 명령
- `install`, `enable_testing`, `add_test` 등 실행과 무관한 명령
- `Threads` 외의 `find_package`: 해당 패키지는 찾지 못한 것으로 처리한다. 필요한 헤더와 라이브러리는 `-I`, `-L`, `-l`로 직접 준다.

## 예시

`examples\cfg`는 `configure_file`, `file(WRITE)`, 제너레이터 식을 함께 쓰는 프로젝트다.

```
CRuntime.exe run examples\cfg
```
