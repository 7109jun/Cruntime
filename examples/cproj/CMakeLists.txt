cmake_minimum_required(VERSION 3.20)
project(CDemo LANGUAGES C)

set(CMAKE_C_STANDARD 11)

file(GLOB LIB_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
add_library(mathlib STATIC ${LIB_SRC})
target_include_directories(mathlib PUBLIC include)
target_compile_definitions(mathlib PUBLIC USE_FAST=1)

add_executable(cdemo main.c)
if(WIN32)
    target_compile_definitions(cdemo PRIVATE ON_WINDOWS)
endif()
target_link_libraries(cdemo PRIVATE mathlib)
