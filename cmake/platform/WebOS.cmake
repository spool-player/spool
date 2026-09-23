function(find_webos_sysroot_library out_var library_name)
    foreach(candidate
            "${WEBOS_SYSROOT_LIB_DIR}/lib${library_name}.so"
            "${WEBOS_SYSROOT_BASE_LIB_DIR}/lib${library_name}.so")
        if(EXISTS "${candidate}" OR IS_SYMLINK "${candidate}")
            set(${out_var} "${candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()

    find_library(${out_var}
        NAMES ${library_name}
        PATHS
            "${WEBOS_SYSROOT_LIB_DIR}"
            "${WEBOS_SYSROOT_BASE_LIB_DIR}"
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH
    )
    if(NOT ${out_var})
        file(GLOB matches
            "${WEBOS_SYSROOT_LIB_DIR}/lib${library_name}.so.*"
            "${WEBOS_SYSROOT_BASE_LIB_DIR}/lib${library_name}.so.*"
        )
        if(matches)
            list(SORT matches)
            list(GET matches 0 resolved_library)
            set(${out_var} "${resolved_library}" PARENT_SCOPE)
            return()
        endif()
    endif()
    if(NOT ${out_var})
        message(FATAL_ERROR "Could not find webOS sysroot library: ${library_name}")
    endif()
    set(${out_var} "${${out_var}}" PARENT_SCOPE)
endfunction()

function(spool_resolve_webos_dependencies)
    pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0)
    set(WEBOS_NATIVE_LIB_DIR "${CMAKE_SYSROOT}/usr/local/webos-native/lib")
    set(WEBOS_SYSROOT_LIB_DIR "${CMAKE_SYSROOT}/usr/lib")
    set(WEBOS_SYSROOT_BASE_LIB_DIR "${CMAKE_SYSROOT}/lib")

    find_webos_sysroot_library(EGL_LIB EGL)
    find_webos_sysroot_library(GLES2_LIB GLESv2)
    find_webos_sysroot_library(WAYLAND_EGL_LIB wayland-egl)
    find_webos_sysroot_library(WAYLAND_CLIENT_LIB wayland-client)
    find_webos_sysroot_library(WAYLAND_WEBOS_CLIENT_LIB wayland-webos-client)
    find_webos_sysroot_library(WEBOS_HELPERS_LIB helpers)
    find_webos_sysroot_library(LUNA_SERVICE_LIB luna-service2)
    find_webos_sysroot_library(ALSA_LIB asound)
    set(AVCODEC_LIB "${WEBOS_NATIVE_LIB_DIR}/libavcodec.so")
    set(AVFILTER_LIB "${WEBOS_NATIVE_LIB_DIR}/libavfilter.so")
    set(AVFORMAT_LIB "${WEBOS_NATIVE_LIB_DIR}/libavformat.so")
    set(AVUTIL_LIB "${WEBOS_NATIVE_LIB_DIR}/libavutil.so")
    set(SWRESAMPLE_LIB "${WEBOS_NATIVE_LIB_DIR}/libswresample.so")
    set(SWSCALE_LIB "${WEBOS_NATIVE_LIB_DIR}/libswscale.so")
    set(MPV_LIB "${MPV_WEBOS_BUILD_DIR}/libmpv.so")
    if(NOT EXISTS "${MPV_LIB}")
        message(FATAL_ERROR "Expected libmpv at ${MPV_LIB}")
    endif()
    foreach(required_lib
            AVCODEC_LIB
            AVFILTER_LIB
            AVFORMAT_LIB
            AVUTIL_LIB
            SWRESAMPLE_LIB
            SWSCALE_LIB)
        if(NOT EXISTS "${${required_lib}}")
            message(FATAL_ERROR "Expected dependency at ${${required_lib}}")
        endif()
    endforeach()

    foreach(variable
            WEBOS_NATIVE_LIB_DIR
            WEBOS_SYSROOT_LIB_DIR
            WEBOS_SYSROOT_BASE_LIB_DIR
            EGL_LIB
            GLES2_LIB
            WAYLAND_EGL_LIB
            WAYLAND_CLIENT_LIB
            WAYLAND_WEBOS_CLIENT_LIB
            WEBOS_HELPERS_LIB
            LUNA_SERVICE_LIB
            ALSA_LIB)
        set(${variable} "${${variable}}" PARENT_SCOPE)
    endforeach()
endfunction()

function(spool_configure_webos_targets native_target core_target)
    target_sources(${core_target} PRIVATE
        src/platform/webos/WebOSAudioSyncPolicy.cpp
        src/platform/webos/WebOSCredentialStore.cpp
        src/platform/webos/WebOSAudioSyncPolicy.h
        src/platform/webos/WebOSMpvConfigPolicy.cpp
        src/platform/webos/WebOSPlatformPaths.cpp
        src/platform/webos/WebOSSettingsPolicy.cpp
        src/platform/webos/WebOSPlaybackSurface.cpp
        src/platform/webos/WebOSSystemProbes.cpp
        src/platform/common/LinuxPerformanceSampler.cpp
    )
    target_sources(${native_target} PRIVATE
        src/platform/UpdateController.cpp
        src/platform/UpdateController.h
        src/platform/webos/WebOSUpdateInstaller.cpp
        src/platform/webos/WebOSUpdateInstaller.h
        src/platform/webos/WebOSNativeAppWindow.cpp
        src/platform/webos/WebOSApplicationServices.cpp
        src/platform/webos/WebOSAudioRoute.cpp
        src/platform/webos/WebOSAudioRoute.h
        src/platform/webos/WebOSMpvRuntime.cpp
        src/platform/webos/WebOSMpvRuntime.h
        src/platform/webos/WebOSPlatformCapabilities.cpp
        src/platform/webos/WebOSPlaybackRuntime.cpp
        src/platform/webos/WebOSPlatformStartup.cpp
        src/platform/webos/WebOSDeviceName.cpp
        src/platform/webos/WebOSDeviceName.h
        src/platform/webos/WebOSScreenSaverInhibitor.cpp
        src/platform/common/UnixProcessIntegration.cpp
    )
    foreach(target IN LISTS SPOOL_TARGETS)
        target_compile_definitions(${target} PRIVATE SPOOL_WEBOS=1)
    endforeach()

    message(STATUS "Using firmware-derived webOS input protocol tables; do not regenerate them from host Wayland XML")
    qt_add_plugin(webos-im-plugin STATIC
        CLASS_NAME WebOSPlatformInputContextPlugin
        PLUGIN_TYPE platforminputcontexts
    )
    target_sources(webos-im-plugin PRIVATE
        src/platform/webos/input/WebOSInputContext.cpp
        src/platform/webos/input/WebOSInputContext.h
        src/platform/webos/input/WebOSInputContextPlugin.cpp
        src/platform/webos/input/WebOSKeysymMap.h
        src/platform/webos/protocol/wayland-text-client-protocol.c
        src/platform/webos/protocol/wayland-text-client-protocol.h
    )
    target_include_directories(webos-im-plugin PRIVATE src/platform/webos)
    target_link_libraries(webos-im-plugin PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::GuiPrivate
        ${WAYLAND_CLIENT_LIB}
    )
    target_link_libraries(${native_target} PRIVATE
        webos-im-plugin
        PkgConfig::GLIB
        Qt6::GuiPrivate
        Qt6::WaylandClient
        ${EGL_LIB}
        ${GLES2_LIB}
        ${WAYLAND_EGL_LIB}
        ${WAYLAND_CLIENT_LIB}
        ${WAYLAND_WEBOS_CLIENT_LIB}
        ${WEBOS_HELPERS_LIB}
        ${LUNA_SERVICE_LIB}
        ${ALSA_LIB}
        ${CMAKE_DL_LIBS}
    )
    target_link_options(${native_target} PRIVATE
        "-Wl,-rpath-link,${WEBOS_SYSROOT_LIB_DIR}"
        "-Wl,-rpath-link,${WEBOS_SYSROOT_BASE_LIB_DIR}"
    )
endfunction()

function(spool_import_static_webos_plugins native_target)
    find_webos_sysroot_library(XKBCOMMON_LIB xkbcommon)
    target_link_libraries(${native_target} PRIVATE ${XKBCOMMON_LIB})
    qt_import_plugins(${native_target}
        NO_DEFAULT
        INCLUDE
            Qt6::QWaylandIntegrationPlugin
            Qt6::QTlsBackendOpenSSLPlugin
            Qt6::QWaylandEglClientBufferPlugin
            Qt6::QWaylandWlShellIntegrationPlugin
            Qt6::QSQLiteDriverPlugin
            Qt6::QJpegPlugin
            Qt6::QWebpPlugin
    )
endfunction()
