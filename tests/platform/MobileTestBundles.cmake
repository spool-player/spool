# Mobile selectors run through Qt's real Activity/UIKit entry point. The host
# supervisor launches a fresh instance of the same phase bundle per selector;
# it never executes a desktop substitute or relies on an app launch exit code.
function(spool_add_mobile_test_bundles)
    if(NOT BUILD_TESTING OR NOT (ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "tvOS"))
        return()
    endif()
    if(ANDROID)
        # Qt requires this in the cache to keep three APK deployment trees apart.
        set(QT_USE_TARGET_ANDROID_BUILD_DIR TRUE CACHE BOOL "Separate Android deployment trees" FORCE)
    endif()

    set(traditional_sources ${SPOOL_TRADITIONAL_TEST_SOURCES})
    # These are desktop native APIs, not mobile implementations of the contract.
    list(FILTER traditional_sources EXCLUDE REGEX "MacOSTitlebarTest\\.mm$")
    list(FILTER traditional_sources EXCLUDE REGEX "LinuxMemoryPolicyTest\\.cpp$")
    # Mobile OSes do not support the host supervisor's fork/exec contract.
    list(FILTER traditional_sources EXCLUDE REGEX "TestSupervisorTest\\.cpp$")
    list(REMOVE_DUPLICATES traditional_sources)
    qt_add_executable(spool-tests tests/TestRunner.cpp tests/TestVariants.cpp
        sdk/provider-contract-runner.cpp tests/platform/MobileTestFixtures.cpp ${traditional_sources})
    qt_add_executable(spool-e2e-tests tests/TestRunner.cpp
        tests/player/MpvVideoItemTest.cpp)
    if(ANDROID)
        # Android's NativeAppWindow is an application source, not in spool-core.
        foreach(target IN ITEMS spool-tests spool-e2e-tests)
            target_sources(${target} PRIVATE src/platform/android/AndroidNativeAppWindow.cpp)
        endforeach()
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "tvOS")
        target_sources(spool-e2e-tests PRIVATE tests/platform/TvOSRuntimeSmoke.mm)
        target_sources(spool-tests PRIVATE tests/platform/TvOSRuntimeSmoke.mm)
        target_compile_definitions(spool-tests PRIVATE SPOOL_TRADITIONAL_TEST_BUNDLE=1)
        set_source_files_properties(tests/platform/TvOSRuntimeSmoke.mm
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    endif()

    # Keep the tests' real filesystem consumers intact: LocalProvider scans and
    # FFmpeg open paths, while QML loads fromLocalFile URLs. Resource-only paths
    # would silently exercise a different boundary. The runner extracts these
    # resources into an isolated sandbox before dispatching a traditional case.
    file(GLOB_RECURSE fixture_files CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/*"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/media/fixtures/*"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/providers/fixtures/*"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/diagnostics/fixtures/*"
        "${CMAKE_CURRENT_SOURCE_DIR}/providers/bundled/*.tar.zst"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/fonts/*.ttf"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/fonts/*.otf")
    list(APPEND fixture_files
        "${CMAKE_CURRENT_SOURCE_DIR}/providers/lock.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/shell/RouteStack.qml")
    qt_add_resources(spool-tests spool_mobile_test_fixtures
        PREFIX "/spool-mobile-fixtures" BASE "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES ${fixture_files})
    target_link_libraries(spool-tests PRIVATE ${SPOOL_PROVIDER_BUNDLE_TARGET})
    target_compile_definitions(spool-tests PRIVATE SPOOL_MOBILE_TEST_BUNDLE=1 SPOOL_TEST_RUNNER=1 TEST_SOURCE_DIR=".")

    get_target_property(scan_args spool QT_QML_IMPORT_SCANNER_EXTRA_ARGS)
    foreach(target IN ITEMS spool-tests spool-e2e-tests)
        target_include_directories(${target} PRIVATE src tests "${CMAKE_CURRENT_BINARY_DIR}/generated")
        target_link_libraries(${target} PRIVATE spool-core)
        set_target_properties(${target} PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION OFF
            INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF
            INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL OFF)
        if(scan_args)
            set_property(TARGET ${target} PROPERTY QT_QML_IMPORT_SCANNER_EXTRA_ARGS ${scan_args})
        endif()
        if(ANDROID)
            if(target STREQUAL "spool-tests")
                set(SPOOL_MOBILE_TEST_IDENTIFIER "com.sachk.spool.tests")
                set(SPOOL_MOBILE_TEST_NAME "Spool traditional tests")
            else()
                set(SPOOL_MOBILE_TEST_IDENTIFIER "com.sachk.spool.e2e_tests")
                set(SPOOL_MOBILE_TEST_NAME "Spool GUI tests")
            endif()
            set(package_dir "${CMAKE_CURRENT_BINARY_DIR}/android-package-${target}")
            file(MAKE_DIRECTORY "${package_dir}")
            configure_file(tests/platform/android/AndroidManifest.xml.in
                "${package_dir}/AndroidManifest.xml" @ONLY)
            get_target_property(extra_libs spool QT_ANDROID_EXTRA_LIBS)
            set_target_properties(${target} PROPERTIES
                QT_ANDROID_PACKAGE_SOURCE_DIR "${package_dir}"
                QT_ANDROID_PACKAGE_NAME "${SPOOL_MOBILE_TEST_IDENTIFIER}"
                QT_ANDROID_APP_NAME "${SPOOL_MOBILE_TEST_NAME}"
                QT_ANDROID_MIN_SDK_VERSION 28
                QT_ANDROID_TARGET_SDK_VERSION 36
                QT_ANDROID_VERSION_NAME "${PROJECT_VERSION}"
                QT_ANDROID_VERSION_CODE 1
                QT_ANDROID_LEGACY_PACKAGING TRUE
                QT_ANDROID_EXTRA_LIBS "${extra_libs}")
            target_compile_definitions(${target} PRIVATE SPOOL_ANDROID=1)
            target_link_libraries(${target} PRIVATE log)
        else()
            if(target STREQUAL "spool-tests")
                set(identifier "com.sachk.spool.tests")
            else()
                set(identifier "com.sachk.spool.e2e-tests")
            endif()
            set_target_properties(${target} PROPERTIES
                MACOSX_BUNDLE TRUE
                MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/tests/platform/tvos/Info.plist.in"
                MACOSX_BUNDLE_GUI_IDENTIFIER "${identifier}"
                MACOSX_BUNDLE_BUNDLE_NAME "${target}"
                MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
                MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
                XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "3")
            spool_configure_tvos_simulator_entitlements(${target})
            install(TARGETS ${target} BUNDLE DESTINATION tests COMPONENT NativeTests)
        endif()
    endforeach()
endfunction()
