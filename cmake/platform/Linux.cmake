function(spool_resolve_linux_dependencies)
    pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
    find_package(OpenGL REQUIRED)
    # The compositor is the only thing on Linux that knows what the display can
    # show, and wp_color_manager_v1 is how it says so.
    pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
    # Generation runs on the build host, never through the target sysroot.
    find_program(WAYLAND_SCANNER_EXECUTABLE NAMES wayland-scanner
        REQUIRED NO_CMAKE_FIND_ROOT_PATH)
endfunction()

function(spool_configure_linux_targets native_target core_target)
    # Keep the pinned upstream XML in source; scanner output belongs to the
    # build tree. Provenance and its retained license live beside the XML.
    set(color_management_xml
        "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/linux/protocol/color-management-v1.xml")
    set(protocol_include_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/wayland")
    set(protocol_output_dir "${protocol_include_dir}/protocol")
    set(color_management_header "${protocol_output_dir}/color-management-v1-client-protocol.h")
    set(color_management_code "${protocol_output_dir}/color-management-v1-protocol.c")
    add_custom_command(
        OUTPUT "${color_management_header}" "${color_management_code}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${protocol_output_dir}"
        COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" client-header
            "${color_management_xml}" "${color_management_header}"
        COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code
            "${color_management_xml}" "${color_management_code}"
        DEPENDS "${color_management_xml}" "${WAYLAND_SCANNER_EXECUTABLE}"
        COMMENT "Generating Wayland color-management-v1 client bindings"
        VERBATIM
    )
    set_source_files_properties("${color_management_header}" "${color_management_code}"
        PROPERTIES SKIP_AUTOGEN ON)
    target_sources(${core_target} PRIVATE
        src/platform/linux/LinuxSettingsPolicy.cpp
        src/platform/linux/LinuxPlatformPaths.cpp
        src/platform/linux/LinuxCredentialStore.cpp
        src/platform/linux/LinuxSystemProbes.cpp
        src/platform/linux/WaylandColorInfo.cpp
        "${color_management_header}"
        "${color_management_code}"
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
    target_include_directories(${core_target} PRIVATE "${protocol_include_dir}")
    target_compile_options(${core_target} PUBLIC "-I${OPENGL_INCLUDE_DIR}")
endfunction()
