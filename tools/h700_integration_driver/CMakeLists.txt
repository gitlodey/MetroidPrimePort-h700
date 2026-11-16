cmake_minimum_required(VERSION 3.25)
project(h700_integration_driver LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
   NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$" OR
   NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "The integration driver requires native Linux ARM64")
endif()
if(NOT H700_INPUTS_STAGED OR NOT IS_DIRECTORY "${H700_INPUTS_DIR}")
  message(FATAL_ERROR "Use build.py: verified, independently staged inputs are required")
endif()
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
   NOT CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL "16.0.6")
  message(FATAL_ERROR "The approved integration profile requires Clang 16.0.6")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(AURORA_ENABLE_TESTS)
  enable_testing()
endif()
# The release config declares directory-local imported targets. Import them in
# the frontend scope so Aurora's sibling runtime libraries see the same package.
find_package(nod REQUIRED CONFIG GLOBAL PATHS "${H700_INPUTS_DIR}/nod" NO_DEFAULT_PATH)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../extern/aurora" aurora EXCLUDE_FROM_ALL)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/h700/VerifyProviders.cmake")

add_executable(h700_integration_driver main.cpp scene.cpp ../../platform/glibc_compat.c)
target_link_libraries(h700_integration_driver PRIVATE aurora::main
  "$<LINK_GROUP:RESCAN,aurora::core,aurora::gx,aurora::vi,aurora::mtx,aurora::os>")
target_compile_options(h700_integration_driver PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(h700_integration_driver PRIVATE H700_DRIVER_REVISION="IG-13"
  $<$<BOOL:${AURORA_H700}>:AURORA_H700>)
set_target_properties(h700_integration_driver PROPERTIES BUILD_RPATH_USE_ORIGIN ON)
option(H700_STAGE_PACKAGE "Link diagnostic driver for the IG13 local SDL staging layout" OFF)
if(H700_STAGE_PACKAGE)
  set_target_properties(h700_integration_driver PROPERTIES
    BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "$ORIGIN/lib")
endif()
if(AURORA_ENABLE_TESTS)
  include(GoogleTest)
  find_package(Threads REQUIRED)
  add_executable(h700_driver_scene_tests scene_test.cpp)
  target_compile_definitions(h700_driver_scene_tests PRIVATE DRIVER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(h700_driver_scene_tests PRIVATE gtest_main Threads::Threads)
  gtest_discover_tests(h700_driver_scene_tests PROPERTIES TIMEOUT 10)
endif()
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/artifacts.json" CONTENT
  "{\"executable\":\"$<TARGET_FILE:h700_integration_driver>\",\"sdl\":\"$<TARGET_FILE:SDL3::SDL3>\"}\n")
