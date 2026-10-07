function(spool_configure_tvos_targets native_target core_target)
    set(SPOOL_APPLE_BUNDLE_IDENTIFIER "com.sachk.spool" CACHE STRING "Apple TV application identifier")
    if(NOT SPOOL_APPLE_BUNDLE_IDENTIFIER MATCHES "^[A-Za-z0-9_-]+(\\.[A-Za-z0-9_-]+)+$")
        message(FATAL_ERROR "SPOOL_APPLE_BUNDLE_IDENTIFIER must be a reverse-DNS application identifier")
    endif()
    spool_configure_apple_mobile_targets(${native_target} ${core_target})
    target_sources(${native_target} PRIVATE
        src/platform/tvos/TvOSRemoteInput.mm
        src/platform/tvos/TvOSRemoteInput.h
    )
    set_source_files_properties(src/platform/tvos/TvOSRemoteInput.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set_target_properties(${native_target} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/app/tvos/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${SPOOL_APPLE_BUNDLE_IDENTIFIER}"
        MACOSX_BUNDLE_BUNDLE_NAME "Spool"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "3"
        XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "App Icon & Top Shelf Image"
    )
    set_source_files_properties(app/tvos/PrivacyInfo.xcprivacy
        PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(${native_target} PRIVATE app/tvos/PrivacyInfo.xcprivacy)
    if(SPOOL_TVOS_ASSETS)
        set_source_files_properties("${SPOOL_TVOS_ASSETS}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        target_sources(${native_target} PRIVATE "${SPOOL_TVOS_ASSETS}")
    endif()
    # Exercise the existing renderer consumer test on the actual UIKit/EAGL
    # target, not a Linux/macOS substitute or an import-only Qt probe.
    qt_add_executable(spool-tvos-playback-smoke
        tests/TestRunner.cpp
        tests/player/MpvVideoItemTest.cpp
        tests/platform/TvOSRuntimeSmoke.mm
    )
    target_include_directories(spool-tvos-playback-smoke PRIVATE src tests "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(spool-tvos-playback-smoke PRIVATE ${core_target})
    set_target_properties(spool-tvos-playback-smoke PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.sachk.spool.playback-smoke"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "3"
    )
    install(TARGETS spool-tvos-playback-smoke BUNDLE DESTINATION smoke)
    set_source_files_properties(tests/platform/TvOSRuntimeSmoke.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endfunction()
