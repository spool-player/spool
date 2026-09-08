if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD ${CMAKE_CXX_STANDARD})
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
endif()

function(jellyfin_resolve_macos_dependencies)
    pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
endfunction()

function(jellyfin_configure_macos_targets native_target core_target)
    if(NOT SPOOL_MACOS_CREDENTIAL_SERVICE MATCHES "^[A-Za-z0-9_-]+(\\.[A-Za-z0-9_-]+)+$")
        message(FATAL_ERROR
            "SPOOL_MACOS_CREDENTIAL_SERVICE must be a non-empty reverse-DNS service name")
    endif()

    target_sources(${core_target} PRIVATE
        src/platform/macos/MacOSSettingsPolicy.cpp
        src/platform/macos/MacOSPlatformPaths.cpp
        src/platform/macos/MacOSCredentialStore.cpp
        src/platform/macos/MacOSSystemProbes.cpp
        src/platform/macos/MacOSTitlebar.mm
        src/platform/desktop/UnsupportedPerformanceSampler.cpp
    )
    target_compile_definitions(${core_target} PRIVATE
        "SPOOL_MACOS_CREDENTIAL_SERVICE=\"${SPOOL_MACOS_CREDENTIAL_SERVICE}\""
    )
    target_sources(${native_target} PRIVATE
        src/platform/macos/MacOSPlatformCapabilities.cpp
        src/platform/macos/MacOSScreenSaverInhibitor.cpp
        src/platform/macos/MacOSPlatformStartup.cpp
        src/platform/common/UnixProcessIntegration.cpp
    )
    if(JELLYFIN_MACOS_ICON)
        set_source_files_properties("${JELLYFIN_MACOS_ICON}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        target_sources(${native_target} PRIVATE "${JELLYFIN_MACOS_ICON}")
    endif()
    target_link_libraries(${core_target} PUBLIC PkgConfig::MPV "-framework Security" "-framework AppKit")
    target_link_libraries(${native_target} PRIVATE "-framework IOKit")
    set_target_properties(${native_target} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Spool for Jellyfin"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.sachk.spool"
        MACOSX_BUNDLE_ICON_FILE "jellyfin-native.icns"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        INSTALL_RPATH "@executable_path/../Frameworks"
    )
endfunction()
