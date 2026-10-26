cmake_minimum_required(VERSION 3.22)
project(glcompat C)

# Host-side GL 3.3 desktop -> GLES 3.2 compatibility layer for the Factorio
# Android port.  Built with the NDK for arm64-v8a / API 26.  The exported
# entry point is fsgl_lookup(); SDL_GL_GetProcAddress calls it before falling
# back to eglGetProcAddress.  It never calls itself and never returns a
# pointer to one of its own wrapper lookups.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(GLCompat SHARED
    src/glcompat.c
    src/error.c
    src/profile.c
    src/translate.c
    src/etc2.c
    src/wrap_gl.c
    src/stream.c
    src/wrap_tex.c)

target_compile_definitions(GLCompat PRIVATE _GNU_SOURCE)
target_include_directories(GLCompat PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/SDL-release-3.4.14/include")
target_compile_options(GLCompat PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type
    -fvisibility=hidden)
target_link_libraries(GLCompat PRIVATE log dl m)
target_link_options(GLCompat PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/glcompat.map")
set_target_properties(GLCompat PROPERTIES
    OUTPUT_NAME GLCompat
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    C_VISIBILITY_PRESET hidden)
