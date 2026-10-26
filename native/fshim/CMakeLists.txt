cmake_minimum_required(VERSION 3.22)
project(fshim C)

# Build a bionic-loadable shim plus a shell-runnable harness used for early
# on-device bring-up (adb push + adb shell, no APK required).

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(GAME_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../build"
    CACHE PATH "directory containing game_constants.h from tools/port.py")

add_library(fshim SHARED
    src/fshim.c
    src/tls.c
    src/compat.c
    src/glhook.c
    src/appmain.c)   # fshim_main: SDLActivity entry point (window selftest / game run)

target_include_directories(fshim PRIVATE src "${GAME_INCLUDE_DIR}")
target_compile_definitions(fshim PRIVATE _GNU_SOURCE)
target_compile_options(fshim PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(fshim PRIVATE log android dl)
set_target_properties(fshim PROPERTIES
    OUTPUT_NAME fshim
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

add_executable(fsrun src/fsrun.c)
target_compile_definitions(fsrun PRIVATE _GNU_SOURCE)
target_link_libraries(fsrun PRIVATE fshim log dl)
set_target_properties(fsrun PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
