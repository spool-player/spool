function(spool_resolve_apple_mobile_dependencies)
    # App Store review limits what a build may bundle and download. An
    # unsigned build for sideloading never goes through review, so it may be
    # an ordinary open build; tvOS and signed builds keep the store's rules.
    if(NOT SPOOL_APPLE_APP_STORE AND NOT IOS)
        message(FATAL_ERROR "Apple TV builds require SPOOL_APPLE_APP_STORE=ON")
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

# Both the production application and each isolated native test bundle need
# their own application identifier in the simulator's Keychain entitlement.
function(spool_configure_apple_simulator_entitlements)
    string(TOLOWER "${CMAKE_OSX_SYSROOT}" tvos_sysroot)
    if(tvos_sysroot MATCHES "(appletvsimulator|iphonesimulator)")
        # Simulator application/Keychain entitlements belong in Mach-O sections,
        # not the macOS code signature, where these are restricted entitlements.
        foreach(target IN LISTS ARGN)
            get_target_property(identifier ${target} MACOSX_BUNDLE_GUI_IDENTIFIER)
            set(xml "${CMAKE_CURRENT_BINARY_DIR}/${target}-simulator.xcent")
            set(der "${xml}.der")
            file(CONFIGURE OUTPUT "${xml}" CONTENT
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\"><dict>
<key>application-identifier</key><string>SPOOLSMOKE.${identifier}</string>
<key>com.apple.developer.team-identifier</key><string>SPOOLSMOKE</string>
<key>keychain-access-groups</key><array><string>SPOOLSMOKE.${identifier}</string></array>
</dict></plist>
")
            add_custom_command(OUTPUT "${der}"
                COMMAND xcrun derq query -f xml -i "${xml}" -o "${der}" --raw
                DEPENDS "${xml}"
                VERBATIM)
            target_sources(${target} PRIVATE "${der}")
            target_link_options(${target} PRIVATE
                "LINKER:-sectcreate,__TEXT,__entitlements,${xml}"
                "LINKER:-sectcreate,__TEXT,__ents_der,${der}")
            set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${xml}" "${der}")
        endforeach()
    endif()
endfunction()
