cmake_minimum_required(VERSION 3.20)
project(CRuntime VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

option(CRUNTIME_BUILD_CORE_TESTS "Build only the portable core (scanner + CMake interpreter) and its test driver" OFF)

# ---------------------------------------------------------------------------
# Portable core: input scanner + in-memory CMake interpreter (no LLVM needed)
# ---------------------------------------------------------------------------
add_library(cr_core STATIC
    src/core/Scanner.cpp
    src/core/CMakeInterp.cpp)
target_include_directories(cr_core PUBLIC src/core)

add_executable(cr_scan_test tests/scan_test.cpp)
target_link_libraries(cr_scan_test PRIVATE cr_core)
set_target_properties(cr_scan_test PROPERTIES EXCLUDE_FROM_ALL $<NOT:$<BOOL:${CRUNTIME_BUILD_CORE_TESTS}>>)

if(NOT WIN32)
    if(NOT CRUNTIME_BUILD_CORE_TESTS)
        message(FATAL_ERROR "CRuntime targets Windows. Use -DCRUNTIME_BUILD_CORE_TESTS=ON to build the portable core only.")
    endif()
    return()
endif()

# ---------------------------------------------------------------------------
# LLVM / Clang (build from source, Release, static libs, X86 target)
#   -DLLVM_DIR=<prefix>/lib/cmake/llvm  -DClang_DIR=<prefix>/lib/cmake/clang
# ---------------------------------------------------------------------------
find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

if(LLVM_VERSION_MAJOR LESS 18 OR LLVM_VERSION_MAJOR GREATER 19)
    message(WARNING "CRuntime is written against LLVM/Clang 18.x-19.x APIs (found ${LLVM_VERSION}).")
endif()

message(STATUS "LLVM ${LLVM_VERSION} (${LLVM_DIR})")
separate_arguments(LLVM_DEFS NATIVE_COMMAND "${LLVM_DEFINITIONS}")

llvm_map_components_to_libnames(LLVM_LIBS
    core support orcjit native passes irreader target mc object)

set(CLANG_RESOURCE_SRC "${LLVM_LIBRARY_DIR}/clang/${LLVM_VERSION_MAJOR}/include")
if(NOT EXISTS "${CLANG_RESOURCE_SRC}/stddef.h")
    message(FATAL_ERROR "Clang resource headers not found: ${CLANG_RESOURCE_SRC}")
endif()

add_executable(CRuntime WIN32
    src/main.cpp
    src/core/Toolchain.cpp
    src/core/Compiler.cpp
    src/core/JitEngine.cpp
    src/core/Worker.cpp
    src/gui/MainWindow.cpp
    res/CRuntime.manifest)

target_include_directories(CRuntime PRIVATE
    src src/core src/gui ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
target_compile_definitions(CRuntime PRIVATE
    ${LLVM_DEFS} NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
# LLVM/Clang are built without RTTI: subclasses of LLVM types must match.
if(MSVC)
    target_compile_options(CRuntime PRIVATE /GR- /EHsc /utf-8 /permissive- /W3 /wd4624 /wd4244 /wd4267 /wd4141 /wd4146)
endif()

target_link_libraries(CRuntime PRIVATE
    cr_core
    clangFrontend clangCodeGen clangDriver
    ${LLVM_LIBS}
    comctl32 ole32 shell32 shlwapi uuid version psapi advapi32 ws2_32 ntdll)

# Clang resource headers (stddef.h, intrinsics...) next to the exe; read-only at runtime.
add_custom_command(TARGET CRuntime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CLANG_RESOURCE_SRC}" "$<TARGET_FILE_DIR:CRuntime>/clang_resource/include")

install(TARGETS CRuntime RUNTIME DESTINATION .)
install(DIRECTORY "${CLANG_RESOURCE_SRC}/" DESTINATION clang_resource/include)
