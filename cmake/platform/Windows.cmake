function(jellyfin_resolve_windows_dependencies)
    set(MPV_ROOT "" CACHE PATH "Windows libmpv SDK prefix")
    find_path(MPV_INCLUDE_DIR mpv/client.h
        HINTS "${MPV_ROOT}/include"
        REQUIRED
    )
    find_library(MPV_LIBRARY
        NAMES mpv libmpv
        HINTS "${MPV_ROOT}/lib"
        REQUIRED
    )
    # The player's Vulkan path compiles only when the Vulkan headers are in this
    # prefix, which build-mpv.ps1 puts there -- the sources ask __has_include,
    # so a prefix without them yields a binary that offers Vulkan and then has
    # no renderer for it. Nothing rebuilds when headers appear beside an already
    # configured build, so say which way it went while it can still be acted on.
    find_path(MPV_VULKAN_INCLUDE_DIR vulkan/vulkan.h HINTS "${MPV_ROOT}/include")
    if(MPV_VULKAN_INCLUDE_DIR)
        message(STATUS "libmpv prefix carries the Vulkan headers: the player's Vulkan path will build")
    else()
        message(STATUS
            "libmpv prefix has no Vulkan headers: the player will build without Vulkan. "
            "Run tools\\windows\\build-mpv.ps1 and configure again to change that.")
    endif()

    if(NOT TARGET MPV::MPV)
        add_library(MPV::MPV UNKNOWN IMPORTED)
        set_target_properties(MPV::MPV PROPERTIES
            IMPORTED_LOCATION "${MPV_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${MPV_INCLUDE_DIR}"
        )
    endif()
endfunction()

function(jellyfin_configure_windows_targets native_target core_target)
    # Packaged desktop launches must not allocate a console. Diagnostics are
    # written to the app-owned log directory even when no stderr handle exists.
    set_target_properties(${native_target} PROPERTIES WIN32_EXECUTABLE TRUE)

    target_sources(${core_target} PRIVATE
        src/platform/windows/WindowsDisplayOutput.cpp
        src/platform/windows/WindowsDisplayOutput.h
        src/platform/windows/WindowsSettingsPolicy.cpp
        src/platform/windows/WindowsCredentialStore.cpp
        src/platform/windows/WindowsSystemProbes.cpp
        src/platform/windows/WindowsPlatformPaths.cpp
        src/platform/desktop/UnsupportedPerformanceSampler.cpp
    )
    target_sources(${native_target} PRIVATE
        src/platform/windows/WindowsPlatformCapabilities.cpp
        src/platform/windows/WindowsScreenSaverInhibitor.cpp
        src/platform/windows/WindowsPlatformStartup.cpp
        src/platform/windows/WindowsProcessIntegration.cpp
    )
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/platform/windows-version.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/jellyfin-native-version.rc"
        @ONLY
    )
    target_sources(${native_target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/jellyfin-native-version.rc")
    target_link_libraries(${core_target} PUBLIC MPV::MPV Advapi32 PRIVATE User32)
endfunction()
