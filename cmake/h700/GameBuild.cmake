# Included in root scope before Aurora, only for the approved H700 game path.
if(CMAKE_CROSSCOMPILING OR
   NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" OR
   NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$" OR
   NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
   NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$" OR
   NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "The H700 game profile requires native Linux ARM64")
endif()
if(NOT H700_INPUTS_STAGED OR NOT IS_DIRECTORY "${H700_INPUTS_DIR}")
  message(FATAL_ERROR "The H700 game profile requires verified, independently staged inputs (Inputs.cmake)")
endif()
foreach(language C CXX)
  if(NOT CMAKE_${language}_COMPILER_ID STREQUAL "Clang" OR
     NOT CMAKE_${language}_COMPILER_VERSION VERSION_EQUAL "16.0.6")
    message(FATAL_ERROR "The approved H700 game profile requires Clang 16.0.6 for ${language}")
  endif()
endforeach()

# The release package creates directory-local imported targets. Import it here
# so both the game and Aurora's sibling libraries see the same static package.
find_package(nod REQUIRED CONFIG GLOBAL PATHS "${H700_INPUTS_DIR}/nod" NO_DEFAULT_PATH)
