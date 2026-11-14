cmake_minimum_required(VERSION 3.25)
project(h700_graphics_probe LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
   NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$" OR
   NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "This standalone probe requires Linux ARM64")
endif()
set(H700_DEPS "" CACHE PATH "Read-only cached dependency sources")
if(NOT EXISTS "${H700_DEPS}/sdl-src/src/video/sdl2" OR
   NOT EXISTS "${H700_DEPS}/dawn_prebuilt-src/lib/cmake/Dawn/DawnConfig.cmake")
  message(FATAL_ERROR "Run build.py --check-inputs; cached SDL2-backed SDL and Dawn are required")
endif()

# Dawn's imported target references Threads::Threads at import time.
find_package(Threads REQUIRED)
find_package(Dawn CONFIG REQUIRED
  PATHS "${H700_DEPS}/dawn_prebuilt-src/lib/cmake/Dawn" NO_DEFAULT_PATH)

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_SDL2_BACKEND ON CACHE BOOL "" FORCE)
# The firmware SDL2 driver owns display output; desktop window systems are absent.
set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)
foreach(option GPU RENDER_GPU X11 WAYLAND KMSDRM TESTS TEST_LIBRARY INSTALL INSTALL_TESTS)
  set(SDL_${option} OFF CACHE BOOL "" FORCE)
endforeach()
# No FetchContent or reuse of the old sdl-build: rebuild the same source here.
add_subdirectory("${H700_DEPS}/sdl-src" "${CMAKE_CURRENT_BINARY_DIR}/sdl" EXCLUDE_FROM_ALL)

find_library(H700_EGL EGL REQUIRED)
find_library(H700_GLES GLESv2 REQUIRED)
add_executable(h700_graphics_probe probe.cpp)
set_target_properties(h700_graphics_probe PROPERTIES
  CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
  BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN")
target_compile_options(h700_graphics_probe PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(h700_graphics_probe PRIVATE
  SDL3::SDL3 dawn::webgpu_dawn ${H700_EGL} ${H700_GLES} ${CMAKE_DL_LIBS} Threads::Threads)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/artifacts.json" CONTENT
  "{\"executable\":\"$<TARGET_FILE:h700_graphics_probe>\",\"sdl\":\"$<TARGET_FILE:SDL3::SDL3>\"}\n")
