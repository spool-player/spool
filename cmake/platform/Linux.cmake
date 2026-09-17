function(jellyfin_resolve_linux_dependencies)
    pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
    find_package(OpenGL REQUIRED)
    # The compositor is the only thing on Linux that knows what the display can
    # show, and wp_color_manager_v1 is how it says so.
    pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
endfunction()

function(jellyfin_configure_linux_targets native_target core_target)
    target_sources(${core_target} PRIVATE
        src/platform/linux/LinuxSettingsPolicy.cpp
        src/platform/linux/LinuxPlatformPaths.cpp
        src/platform/linux/LinuxCredentialStore.cpp
        src/platform/linux/LinuxSystemProbes.cpp
        src/platform/linux/WaylandColorInfo.cpp
        src/platform/linux/protocol/color-management-v1-protocol.c
        src/platform/common/LinuxPerformanceSampler.cpp
    )
    target_sources(${native_target} PRIVATE
        src/platform/linux/LinuxPlatformCapabilities.cpp
        src/platform/linux/LinuxScreenSaverInhibitor.cpp
        src/platform/linux/LinuxPlatformStartup.cpp
        src/platform/common/UnixProcessIntegration.cpp
    )
    target_link_libraries(${native_target} PRIVATE Qt6::DBus)
    target_link_libraries(${core_target} PUBLIC PkgConfig::MPV OpenGL::GL)
    target_link_libraries(${core_target} PRIVATE PkgConfig::WAYLAND_CLIENT Qt6::GuiPrivate)
    target_include_directories(${core_target} PUBLIC "${OPENGL_INCLUDE_DIR}")
    target_compile_options(${core_target} PUBLIC "-I${OPENGL_INCLUDE_DIR}")
endfunction()
