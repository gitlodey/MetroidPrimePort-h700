# Root-game additions to the approved native ARM64 profile. Verified input
# locations still come from the existing staging helper's Inputs.cmake.
include("${CMAKE_CURRENT_LIST_DIR}/InitialCache.cmake")

set(MP_BUNDLED_DEPS ON CACHE BOOL "")
set(MP_EMBED_RESOURCES ON CACHE BOOL "")
set(MP_ANDROID_NOD_STUB OFF CACHE BOOL "")
set(MP_ENABLE_SMOKE_DRIVER OFF CACHE BOOL "")
set(CMAKE_DISABLE_FIND_PACKAGE_OpenSSL ON CACHE BOOL "")
set(BUILD_TESTING OFF CACHE BOOL "")
set(MP_BUILD_TESTS OFF CACHE BOOL "")
set(MP_USE_CCACHE OFF CACHE BOOL "")
set(MP_USE_MOLD OFF CACHE BOOL "")
