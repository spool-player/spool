function(spool_resolve_apple_mobile_dependencies)
    if(NOT SPOOL_APPLE_APP_STORE)
        message(FATAL_ERROR "Apple mobile builds require SPOOL_APPLE_APP_STORE=ON")
    endif()
    set(PKG_CONFIG_ARGN --static)
    pkg_check_modules(MPV REQUIRED IMPORTED_TARGET GLOBAL mpv)
endfunction()

function(spool_configure_apple_mobile_targets native_target core_target)
    set(SPOOL_APPLE_CREDENTIAL_SERVICE "com.sachk.spool" CACHE STRING "Apple mobile Keychain service")
    target_sources(${core_target} PRIVATE
        src/platform/apple/AppleCredentialStore.mm
        src/platform/apple/AppleMobilePaths.mm
        src/platform/apple/AppleMobilePlatform.mm
        src/platform/apple/AppleMobileApplicationServices.mm
        src/platform/desktop/UnsupportedPerformanceSampler.cpp
        src/platform/desktop/DesktopNativeAppWindow.cpp
        src/platform/desktop/DesktopMpvConfigPolicy.cpp
        src/platform/desktop/DesktopPlaybackSurface.cpp
        src/platform/desktop/DesktopPlaybackRuntime.cpp
    )
    target_sources(${native_target} PRIVATE src/platform/common/UnixProcessIntegration.cpp)
    target_compile_definitions(${core_target} PUBLIC SPOOL_APPLE_MOBILE=1
        "SPOOL_APPLE_CREDENTIAL_SERVICE=\"${SPOOL_APPLE_CREDENTIAL_SERVICE}\"")
    set_source_files_properties(
        src/platform/apple/AppleCredentialStore.mm src/platform/apple/AppleMobilePaths.mm
        src/platform/apple/AppleMobilePlatform.mm src/platform/apple/AppleMobileApplicationServices.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(${core_target} PUBLIC PkgConfig::MPV
        "-framework Security" "-framework UIKit" "-framework AVFoundation" "-framework MediaPlayer")
    file(GLOB fonts "${CMAKE_CURRENT_SOURCE_DIR}/qml/fonts/*.otf" "${CMAKE_CURRENT_SOURCE_DIR}/qml/fonts/*.ttf")
    set_source_files_properties(${fonts} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/fonts")
    target_sources(${native_target} PRIVATE ${fonts})
endfunction()
