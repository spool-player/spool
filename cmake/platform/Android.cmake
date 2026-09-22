function(jellyfin_resolve_android_dependencies)
    set(ANDROID_DEPS_PREFIX "" CACHE PATH "Android media dependency prefix")
    find_path(MPV_INCLUDE_DIR mpv/client.h
        HINTS "${ANDROID_DEPS_PREFIX}/include"
        REQUIRED
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH
    )
    find_library(MPV_LIBRARY
        NAMES mpv libmpv
        HINTS "${ANDROID_DEPS_PREFIX}/lib"
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH
        REQUIRED
    )
    if(NOT TARGET MPV::MPV)
        add_library(MPV::MPV SHARED IMPORTED)
        set_target_properties(MPV::MPV PROPERTIES
            IMPORTED_LOCATION "${MPV_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${MPV_INCLUDE_DIR}"
        )
    endif()
endfunction()

function(jellyfin_configure_android_targets native_target core_target)
    target_sources(${core_target} PRIVATE
        src/platform/android/AndroidPlatform.cpp
        # Android is Linux, and every file this reads -- /proc/stat,
        # /proc/self/stat, the per-thread task entries, loadavg and meminfo --
        # is readable by an ordinary app. It was only ever on the unsupported
        # sampler because nobody had tried, which is why the diagnostics
        # overlay showed no CPU figures on the one platform where they matter
        # most.
        src/platform/common/LinuxPerformanceSampler.cpp
        src/platform/desktop/DesktopMpvConfigPolicy.cpp
        src/platform/android/AndroidPlaybackSurface.cpp
        src/platform/desktop/DesktopPlaybackRuntime.cpp
    )
    target_sources(${native_target} PRIVATE
        src/platform/android/AndroidNativeAppWindow.cpp
        src/platform/android/AndroidApplicationServices.cpp
        src/platform/android/AndroidLocalMediaSession.cpp
        src/platform/android/AndroidLocalMediaSession.h
        src/platform/android/AndroidProcessIntegration.cpp
        src/platform/UpdateController.cpp
        src/platform/UpdateController.h
        src/platform/android/AndroidUpdateInstaller.cpp
        src/platform/android/AndroidUpdateInstaller.h
    )
    target_link_libraries(${core_target} PUBLIC MPV::MPV)
    # liblog carries diagnostics to logcat, the only readable output on Android.
    target_link_libraries(${native_target} PRIVATE log)
    target_compile_definitions(${core_target} PRIVATE SPOOL_ANDROID=1)
    target_compile_definitions(${native_target} PRIVATE SPOOL_ANDROID=1)
    set(SPOOL_ANDROID_VERSION_QUALIFIER "99" CACHE STRING
        "Android version qualifier (0-98 prerelease, 99 release)")
    if(NOT SPOOL_ANDROID_VERSION_QUALIFIER MATCHES "^[0-9]+$"
            OR SPOOL_ANDROID_VERSION_QUALIFIER GREATER 99)
        message(FATAL_ERROR "SPOOL_ANDROID_VERSION_QUALIFIER must be an integer from 0 to 99")
    endif()
    math(EXPR android_version_code
        "${PROJECT_VERSION_MAJOR} * 100000000 + ${PROJECT_VERSION_MINOR} * 100000 + ${PROJECT_VERSION_PATCH} * 100 + ${SPOOL_ANDROID_VERSION_QUALIFIER}")
    target_compile_definitions(${native_target} PRIVATE
        SPOOL_ANDROID_ABI="${ANDROID_ABI}"
        SPOOL_ANDROID_VERSION_CODE=${android_version_code}
    )
    # One package for televisions and handsets. What genuinely differs between
    # them is a bool resource and a launch screen, both of which the platform
    # picks with the -television qualifier; everything else is asked of the
    # system at runtime. See androidIsTelevision() in AndroidPlatform.cpp.
    set(android_package_name "com.sachk.spool")
    set(android_package_source "${CMAKE_CURRENT_BINARY_DIR}/android-package")
    file(REMOVE_RECURSE "${android_package_source}")
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/app/android-common/"
        DESTINATION "${android_package_source}")
    # The launch screen is rendered per build because it carries the version.
    file(COPY "${SPOOL_SPLASH_DIR}/res/"
        DESTINATION "${android_package_source}/res")

    # Android rejects versioned sonames, so bundle only plain .so files, and only
    # from the Android dependency prefix.
    file(GLOB ANDROID_EXTRA_LIBS "${ANDROID_DEPS_PREFIX}/lib/*.so")
    set_property(TARGET ${native_target} PROPERTY QT_ANDROID_EXTRA_LIBS "${ANDROID_EXTRA_LIBS}")
    # APKs are direct downloads, not Play-delivered splits. Compress native
    # libraries and let Android extract them at install time; storing them
    # uncompressed more than doubles the per-ABI download.
    set_property(TARGET ${native_target} PROPERTY QT_ANDROID_LEGACY_PACKAGING TRUE)
    set_property(TARGET ${native_target} PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR "${android_package_source}")
    set_property(TARGET ${native_target} PROPERTY QT_ANDROID_PACKAGE_NAME "${android_package_name}")
    set_property(TARGET ${native_target} PROPERTY QT_ANDROID_APP_NAME "Spool for Jellyfin")
    set_property(TARGET ${native_target} PROPERTY QT_ANDROID_MIN_SDK_VERSION 28)
    set_property(TARGET ${native_target} PROPERTY QT_ANDROID_TARGET_SDK_VERSION 36)
    set_property(TARGET ${native_target} PROPERTY QT_ANDROID_VERSION_NAME "${PROJECT_VERSION}")
    set_property(TARGET ${native_target} PROPERTY QT_ANDROID_VERSION_CODE "${android_version_code}")
endfunction()
