include(${CMAKE_CURRENT_LIST_DIR}/AppleMobile.cmake)
function(spool_resolve_ios_dependencies)
    spool_resolve_apple_mobile_dependencies()
endfunction()

function(spool_configure_ios_targets native_target core_target)
    spool_configure_apple_mobile_targets(${native_target} ${core_target})
    target_compile_definitions(${native_target} PRIVATE TOUCHSCREEN)
    set_target_properties(${native_target} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/app/ios/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.sachk.spool"
        MACOSX_BUNDLE_BUNDLE_NAME "Spool"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
    )
    set_source_files_properties(app/ios/PrivacyInfo.xcprivacy PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(${native_target} PRIVATE app/ios/PrivacyInfo.xcprivacy)
    if(SPOOL_IOS_ASSETS)
        target_sources(${native_target} PRIVATE "${SPOOL_IOS_ASSETS}")
    endif()
endfunction()
