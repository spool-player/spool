include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/tools/manifests/android-third-party.json" spool_dependency_manifest)
string(JSON spool_zstd_url GET "${spool_dependency_manifest}" sources zstd url)
string(JSON spool_zstd_sha256 GET "${spool_dependency_manifest}" sources zstd sha256)

# Compile the unmodified upstream library with the active platform toolchain.
# Nix supplies the same digest-pinned source outside the build sandbox.
if(NOT "$ENV{SPOOL_ZSTD_SOURCE_DIR}" STREQUAL "")
    set(FETCHCONTENT_SOURCE_DIR_SPOOL_ZSTD "$ENV{SPOOL_ZSTD_SOURCE_DIR}")
endif()
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_COMPRESSION OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_DECOMPRESSION ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_DICTBUILDER OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spool_zstd
    URL "${spool_zstd_url}"
    URL_HASH "SHA256=${spool_zstd_sha256}"
    SOURCE_SUBDIR build/cmake
)
FetchContent_MakeAvailable(spool_zstd)
set_property(DIRECTORY "${spool_zstd_SOURCE_DIR}/build/cmake" PROPERTY EXCLUDE_FROM_ALL TRUE)
set_target_properties(libzstd_static PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
if(SPOOL_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(libzstd_static PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()
