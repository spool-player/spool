cmake_minimum_required(VERSION 3.22)

file(READ "${CMAKE_CURRENT_LIST_DIR}/VERSION" JELLYFIN_VERSION)
string(STRIP "${JELLYFIN_VERSION}" JELLYFIN_VERSION)
if(NOT JELLYFIN_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must contain a semantic version such as 0.2.1")
endif()

project(JellyfinNativeWebOS VERSION "${JELLYFIN_VERSION}" LANGUAGES C CXX)

if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

set(JELLYFIN_NATIVE_WEBOS_DEFAULT OFF)
if(CMAKE_CROSSCOMPILING AND NOT ANDROID)
    set(JELLYFIN_NATIVE_WEBOS_DEFAULT ON)
endif()
option(JELLYFIN_NATIVE_WEBOS "Build the webOS Starfish/native-surface client" ${JELLYFIN_NATIVE_WEBOS_DEFAULT})
option(TOUCHSCREEN "Enable mobile touchscreen interactions (runtime-disabled on TVs)" ${ANDROID})
option(JELLYFIN_NATIVE_DEV_BUILD "Favor faster local development builds over smallest release artifacts" OFF)
option(JELLYFIN_DIAGNOSTICS "Build debug lifecycle/process/watchdog diagnostics" OFF)
option(JELLYFIN_DIAGNOSTICS_STACKDUMP "Allow diagnostics to request gdb stack dumps" OFF)
option(JELLYFIN_DIAGNOSTICS_ABORT_ON_HANG "Abort after watchdog hang diagnostics are written" OFF)
option(JELLYFIN_ARTWORK_ASPECT_DIAGNOSTICS "Log artwork decode requests whose target aspect would stretch the source" OFF)
option(JELLYFIN_IMAGE_SUBTITLE_DIAGNOSTICS
    "Log image subtitle crop, segmentation, geometry, and recoloring per subtitle event" OFF)
option(JELLYFIN_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(SPOOL_MACOS_CREDENTIAL_SERVICE "com.sachk.spool.dev" CACHE STRING
    "macOS Keychain service used for persisted session tokens")
option(JELLYFIN_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
include(CTest)

set(MPV_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/mpv" CACHE PATH "mpv source tree")
set(MPV_ACTIVE_SOURCE_DIR "${MPV_SOURCE_DIR}")
set(MPV_WEBOS_BUILD_DIR "${MPV_SOURCE_DIR}/build/webos-libmpv" CACHE PATH "webOS libmpv build directory")

if(JELLYFIN_NATIVE_DEV_BUILD)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    endif()
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    find_package(PkgConfig REQUIRED)
endif()

set(JELLYFIN_QT_COMPONENTS
    Core
    Gui
    Network
    Qml
    Quick
    Sql
    OpenGL
    LinguistTools
    ShaderTools
    WebSockets
    Svg
)
if(JELLYFIN_NATIVE_WEBOS)
    set(OPENSSL_USE_STATIC_LIBS FALSE)
    set(JELLYFIN_QT_MIN_VERSION 6.11)
    list(APPEND JELLYFIN_QT_COMPONENTS
        GuiPrivate
        QuickLayouts
        WaylandClient
    )
else()
    set(JELLYFIN_QT_MIN_VERSION 6.11)
    # rhi/qrhi.h is how the video item reaches the graphics device it shares
    # with libplacebo, and how the swapchain is asked what the display can show.
    if(NOT ANDROID)
        list(APPEND JELLYFIN_QT_COMPONENTS GuiPrivate)
    endif()
    if(UNIX AND NOT APPLE AND NOT ANDROID)
        list(APPEND JELLYFIN_QT_COMPONENTS DBus)
    endif()
endif()
find_package(Qt6 ${JELLYFIN_QT_MIN_VERSION} REQUIRED COMPONENTS ${JELLYFIN_QT_COMPONENTS})
find_package(QCoro6 0.13 REQUIRED COMPONENTS Core Network)

qt_standard_project_setup(
    REQUIRES ${JELLYFIN_QT_MIN_VERSION}
    I18N_TRANSLATED_LANGUAGES en_US en_GB en_AU
)
include(cmake/platform/Common.cmake)

# The launch screen carries the version, so it is rendered per build rather
# than kept in the tree. tools/generate-splash.sh is the whole of the drawing;
# configuring is enough to get it, so no platform needs a second command.
if(ANDROID)
    set(spool_splash_platform "android")
elseif(JELLYFIN_NATIVE_WEBOS)
    set(spool_splash_platform "webos")
else()
    set(spool_splash_platform "desktop")
endif()
set(SPOOL_SPLASH_DIR "${CMAKE_CURRENT_BINARY_DIR}/splash/${spool_splash_platform}" CACHE PATH
    "Directory the generated launch screen is written to")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/VERSION"
    "${CMAKE_CURRENT_SOURCE_DIR}/app/icons/spool.svg"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate-splash.sh"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/manifests/splash.json"
)
execute_process(
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate-splash.sh"
        --platform "${spool_splash_platform}"
        --out "${SPOOL_SPLASH_DIR}"
        --version "${PROJECT_VERSION}"
    RESULT_VARIABLE spool_splash_result
    OUTPUT_VARIABLE spool_splash_output
    ERROR_VARIABLE spool_splash_output
)
if(NOT spool_splash_result EQUAL 0)
    message(FATAL_ERROR "Rendering the launch screen failed:\n${spool_splash_output}")
endif()
message(STATUS "${spool_splash_output}")


jellyfin_resolve_platform_dependencies()

# The source groups below mark the seam a spool-jellyfin / spool-plex /
# spool-stremio split would cut along. They all compile into one library for
# now; keeping them apart here is what makes the split mechanical later.
#
# The rule that matters: nothing in the platform, player or shell groups may
# include src/api/, src/discovery/ or src/providers/. Core reaches a media
# source only through the interfaces in src/provider/;
# tools/check-module-seam.sh enforces it as the module-seam test.

# Per-OS abstractions. No backend and no UI knowledge.
set(SPOOL_PLATFORM_SOURCES
    src/platform/MpvConfigPolicy.h
    src/platform/PlatformDisplayOutput.h
    src/platform/NativeAppWindow.h
    src/platform/PlatformApplicationServices.h
    src/platform/PlatformCapabilities.h
    src/platform/PlatformRemoteMediaSession.h
    src/platform/PlatformPaths.h
    src/platform/PlatformPerformanceSampler.h
    src/platform/PlatformPlaybackRuntime.h
    src/platform/PlatformPlaybackSurface.h
    src/platform/PlatformProcess.h
    src/platform/PlatformSettingsPolicy.h
    src/platform/PlatformStartup.h
    src/platform/PlatformSystemProbes.h
    src/platform/ScreenSaverInhibitor.h
    src/platform/common/NativeAppWindowCommon.cpp
    src/platform/common/PlatformDisplayOutput.cpp
    src/platform/common/ScreenSaverInhibitor.cpp
    src/platform/common/PlatformRemoteMediaSession.cpp
)

# libmpv facade and playback state.
set(SPOOL_PLAYER_SOURCES
    src/player/MpvLifecycle.cpp
    src/player/MpvLifecycle.h
    src/player/MpvOptionProfile.cpp
    src/player/MpvOptionProfile.h
    src/player/MpvVideoItem.cpp
    src/player/MpvVideoItem.h
    src/player/PlayQueueController.cpp
    src/player/PlayQueueController.h
    src/player/PlayQueueOutlineModel.cpp
    src/player/PlayQueueOutlineModel.h
    src/player/PlaybackFailurePolicy.cpp
    src/player/PlaybackFailurePolicy.h
    src/player/PlaybackPositionTracker.cpp
    src/player/PlaybackPositionTracker.h
    src/player/PlaybackReporter.cpp
    src/player/PlaybackReporter.h
    src/player/PlaybackTimeline.cpp
    src/player/PlaybackTimeline.h
    src/player/PlaybackTrackParser.cpp
    src/player/PlaybackTrackParser.h
    src/player/PlaybackTrackState.cpp
    src/player/PlaybackTrackState.h
    src/player/PlayerController.cpp
    src/player/PlayerController.h
    src/player/RenderTargetProfile.cpp
    src/player/RenderTargetProfile.h
    src/common/SeriesAudioSelection.cpp
    src/common/SeriesAudioSelection.h
)

# Application plumbing that does not name a media backend.
set(SPOOL_SHELL_SOURCES
    src/app/AccountProfile.cpp
    src/app/AccountProfile.h
    src/app/ArtworkImageProvider.cpp
    src/app/ArtworkImageProvider.h
    src/app/ArtworkPrefetcher.h
    src/app/ArtworkService.cpp
    src/app/ArtworkService.h
    src/app/BrowseSessionController.cpp
    src/app/BrowseSessionController.h
    src/app/ContentModelController.cpp
    src/app/ContentModelController.h
    src/app/HomeModelController.cpp
    src/app/HomeModelController.h
    src/app/LibraryPrefetchController.cpp
    src/app/LibraryPrefetchController.h
    src/app/SearchController.cpp
    src/app/SearchController.h
    src/app/UserItemStateController.cpp
    src/app/UserItemStateController.h
    src/app/CpuTopology.cpp
    src/app/CpuTopology.h
    src/app/LocalizationManager.cpp
    src/app/LocalizationManager.h
    src/app/LibraryQuery.cpp
    src/app/LibraryQuery.h
    src/app/MemoryBudget.cpp
    src/app/MemoryBudget.h
    src/app/RouterController.cpp
    src/app/RouterController.h
    src/app/SettingsController.cpp
    src/app/SettingsController.h
    src/app/SettingsSchema.cpp
    src/app/SettingsSchema.h
    src/app/SpoolLinkCodec.cpp
    src/app/SpoolLinkCodec.h
    src/app/SpoolRemoteProtocol.cpp
    src/app/SpoolRemoteProtocol.h
    src/app/UpdateManifest.cpp
    src/app/UpdateManifest.h
    src/app/AppController.cpp
    src/app/AppController.h
    src/cache/DatabaseManager.cpp
    src/cache/DatabaseManager.h
    src/common/AsyncTask.h
    src/common/LogRotation.h
    src/common/MetaJson.h
    src/common/RequestGeneration.h
    src/common/TlsTrust.cpp
    src/common/TlsTrust.h
    src/common/VariantUtils.cpp
    src/common/VariantUtils.h
    src/media/MediaTypes.cpp
    src/media/MediaTypes.h
    src/models/LibraryListModel.cpp
    src/models/LibraryListModel.h
    src/models/MovieGridModel.cpp
    src/models/MovieGridModel.h
    src/provider/ArtworkSource.h
    src/provider/Catalog.h
    src/provider/GroupPlayback.h
    src/provider/PlaybackSource.h
    src/provider/Provider.h
    src/provider/ProviderQmlCache.cpp
    src/provider/ProviderQmlCache.h
    src/provider/ProviderUiContext.cpp
    src/provider/ProviderUiContext.h
    src/provider/ProviderMediaPage.cpp
    src/provider/ProviderMediaPage.h
    src/provider/ScriptRuntime.cpp
    src/provider/ScriptRuntime.h
    src/provider/ProviderRegistry.cpp
    src/provider/ProviderRegistrySources.cpp
    src/provider/ProviderRegistry.h
    src/provider/RemotePlayback.h
    src/provider/SearchSource.h
    src/provider/StreamQualityControl.h
    src/provider/UserItemStateSink.h
    src/diagnostics/Diagnostics.cpp
    src/diagnostics/Diagnostics.h
    src/diagnostics/InputLatencyMonitor.cpp
    src/diagnostics/InputLatencyMonitor.h
    src/diagnostics/RenderBenchmark.cpp
    src/diagnostics/RenderBenchmark.h
    src/diagnostics/SystemPerformanceMonitor.cpp
    src/diagnostics/SystemPerformanceMonitor.h
)

# Jellyfin REST, server discovery, and the controllers and models shaped
# by them. This is the group a second backend would grow a sibling to.
set(SPOOL_JELLYFIN_SOURCES
    src/api/HttpRequestPolicy.h
    src/api/ItemsQuery.cpp
    src/api/ItemsQuery.h
    src/api/JellyfinApiFacade.cpp
    src/api/JellyfinApiFacade.h
    src/api/JellyfinSession.h
    src/api/JellyfinSettingsBridge.cpp
    src/api/JellyfinProvider.cpp
    src/api/JellyfinProvider.h
    src/api/JellyfinSettingsBridge.h
    src/api/PlaybackBandwidthPolicy.cpp
    src/api/PlaybackBandwidthPolicy.h
    src/api/PlaybackNegotiation.cpp
    src/api/PlaybackNegotiation.h
    src/app/LibraryManagementController.cpp
    src/app/LibraryManagementController.h
    src/app/QuickConnectController.cpp
    src/app/QuickConnectController.h
    src/app/RemoteControlController.cpp
    src/app/RemoteControlController.h
    src/app/SpoolLink.cpp
    src/app/SpoolLink.h
    src/app/SessionController.cpp
    src/app/SessionController.h
    src/app/SyncPlayClock.cpp
    src/app/SyncPlayClock.h
    src/app/SyncPlayController.cpp
    src/app/SyncPlayController.h
    src/discovery/DiscoveredServer.h
    src/discovery/DiscoveryController.cpp
    src/discovery/DiscoveryController.h
    src/models/DiscoveredServerModel.cpp
    src/models/DiscoveredServerModel.h
)

# The folder-of-files provider: the second implementation of the provider
# contract, kept as its permanent fixture. Nothing in core may include it.
set(SPOOL_LOCAL_SOURCES
    src/providers/local/LocalProvider.cpp
    src/providers/local/LocalProvider.h
)

set(JELLYFIN_CORE_SOURCES
    ${SPOOL_PLATFORM_SOURCES}
    ${SPOOL_PLAYER_SOURCES}
    ${SPOOL_SHELL_SOURCES}
    ${SPOOL_JELLYFIN_SOURCES}
    ${SPOOL_LOCAL_SOURCES}
)

add_library(jellyfin-core STATIC ${JELLYFIN_CORE_SOURCES})

qt_add_executable(jellyfin-native
    src/main.cpp
    src/app/GraphicsStartup.cpp
    src/app/GraphicsStartup.h
)

# Bundle immutable, verified source bytes. This resource path also exercises
# source loading with no developer-generated QML/JS cache in provider tests.
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
set(SPOOL_JELLYFIN_SOURCE_DIR "" CACHE PATH "Explicit development override for the portable Jellyfin source")
set(SPOOL_PROVIDER_RESOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/portable-providers")
set(spool_provider_bundle_args
    "${CMAKE_CURRENT_SOURCE_DIR}/providers/lock.json" "${SPOOL_PROVIDER_RESOURCE_DIR}")
if(SPOOL_JELLYFIN_SOURCE_DIR)
    list(APPEND spool_provider_bundle_args --jellyfin-source "${SPOOL_JELLYFIN_SOURCE_DIR}")
    file(GLOB_RECURSE spool_provider_override_files CONFIGURE_DEPENDS
        "${SPOOL_JELLYFIN_SOURCE_DIR}/logic/*"
        "${SPOOL_JELLYFIN_SOURCE_DIR}/ui/*"
        "${SPOOL_JELLYFIN_SOURCE_DIR}/resources/*")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${spool_provider_override_files}
        "${SPOOL_JELLYFIN_SOURCE_DIR}/manifest.json"
        "${SPOOL_JELLYFIN_SOURCE_DIR}/LICENSE"
        "${SPOOL_JELLYFIN_SOURCE_DIR}/NOTICE")
endif()
file(GLOB spool_provider_archives CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/providers/bundled/*.zip")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/providers/lock.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/bundle-providers.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/provider-package.py"
    ${spool_provider_archives})
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/bundle-providers.py"
        ${spool_provider_bundle_args}
    RESULT_VARIABLE spool_provider_bundle_result
    ERROR_VARIABLE spool_provider_bundle_error
    OUTPUT_QUIET)
if(NOT spool_provider_bundle_result EQUAL 0)
    message(FATAL_ERROR "Bundling provider sources failed: ${spool_provider_bundle_error}")
endif()
qt_add_resources(spool_provider_bundle_sources "${SPOOL_PROVIDER_RESOURCE_DIR}/providers.qrc")
target_sources(jellyfin-native PRIVATE ${spool_provider_bundle_sources})

set(JELLYFIN_NATIVE_TARGETS jellyfin-core jellyfin-native)
jellyfin_configure_platform_targets(jellyfin-native jellyfin-core)
foreach(target IN LISTS JELLYFIN_NATIVE_TARGETS)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(JELLYFIN_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
        target_link_options(${target} PRIVATE
            "$<$<CONFIG:Release,MinSizeRel>:/OPT:REF>"
            "$<$<CONFIG:Release,MinSizeRel>:/OPT:ICF>"
        )
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(JELLYFIN_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
        if(JELLYFIN_SANITIZERS)
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endif()
    endif()
endforeach()

if(JELLYFIN_SANITIZERS AND MSVC)
    message(FATAL_ERROR "JELLYFIN_SANITIZERS requires GCC or Clang")
endif()


if(NOT JELLYFIN_NATIVE_DEV_BUILD)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT JELLYFIN_IPO_SUPPORTED OUTPUT JELLYFIN_IPO_ERROR)
    if(JELLYFIN_IPO_SUPPORTED)
        foreach(target IN LISTS JELLYFIN_NATIVE_TARGETS)
            set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
            set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL TRUE)
        endforeach()
    else()
        message(STATUS "Interprocedural optimization unavailable: ${JELLYFIN_IPO_ERROR}")
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        foreach(target IN LISTS JELLYFIN_NATIVE_TARGETS)
            target_compile_options(${target} PRIVATE -ffunction-sections -fdata-sections)
            if(ANDROID)
                target_compile_options(${target} PRIVATE -fPIC)
                target_link_options(${target} PRIVATE -Wl,--gc-sections -Wl,--as-needed -Wl,-z,relro,-z,now)
            elseif(UNIX AND NOT APPLE)
                if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|ARM)")
                    target_compile_options(${target} PRIVATE -fPIC)
                    if(target STREQUAL "jellyfin-native")
                        target_link_options(${target} PRIVATE -fPIC)
                    endif()
                else()
                    target_compile_options(${target} PRIVATE -fPIE)
                endif()
                target_link_options(${target} PRIVATE -pie -Wl,--gc-sections -Wl,--as-needed -Wl,-z,relro,-z,now)
            elseif(APPLE)
                target_link_options(${target} PRIVATE -Wl,-dead_strip)
            endif()
        endforeach()
    endif()
endif()
# Desktop renders the video item through the RHI, so the scene graph can be
# Vulkan and the target can be a floating-point image an HDR swapchain will
# accept. Android and webOS keep the framebuffer item.
#
# moc has to agree with the compiler about which class the item derives from,
# so the choice cannot be a platform check in the header -- moc knows the
# target platform only through what the build tells it. It is written to a
# generated header rather than passed as a compile definition because only a
# file is a dependency the generator can see: definitions reach moc through
# autogen's settings, so flipping one in a build tree that already exists
# (a restored CI cache, a local tree switching branches) can recompile every
# translation unit against the new base class while leaving the moc output
# generated for the old one in place. That mismatch fails the build with
# `use of undeclared identifier 'QQuickFramebufferObject'`.
if(ANDROID OR JELLYFIN_NATIVE_WEBOS)
    set(JELLYFIN_MPV_ITEM_RHI_ENABLED 0)
else()
    set(JELLYFIN_MPV_ITEM_RHI_ENABLED 1)
endif()
file(CONFIGURE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/MpvVideoItemBase.h"
    CONTENT "// Generated by CMake. Do not edit.
#pragma once

#define JELLYFIN_MPV_ITEM_RHI @JELLYFIN_MPV_ITEM_RHI_ENABLED@
"
    @ONLY)
foreach(target IN LISTS JELLYFIN_NATIVE_TARGETS)
    target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/generated")
endforeach()
if(JELLYFIN_DIAGNOSTICS)
    foreach(target IN LISTS JELLYFIN_NATIVE_TARGETS)
        target_compile_definitions(${target} PRIVATE JELLYFIN_DIAGNOSTICS=1)
    endforeach()
endif()
if(JELLYFIN_DIAGNOSTICS_STACKDUMP)
    foreach(target IN LISTS JELLYFIN_NATIVE_TARGETS)
        target_compile_definitions(${target} PRIVATE JELLYFIN_DIAGNOSTICS_STACKDUMP=1)
    endforeach()
endif()
if(JELLYFIN_DIAGNOSTICS_ABORT_ON_HANG)
    foreach(target IN LISTS JELLYFIN_NATIVE_TARGETS)
        target_compile_definitions(${target} PRIVATE JELLYFIN_DIAGNOSTICS_ABORT_ON_HANG=1)
    endforeach()
endif()
if(JELLYFIN_ARTWORK_ASPECT_DIAGNOSTICS)
    foreach(target IN LISTS JELLYFIN_NATIVE_TARGETS)
        target_compile_definitions(${target} PRIVATE JELLYFIN_ARTWORK_ASPECT_DIAGNOSTICS=1)
    endforeach()
endif()
if(JELLYFIN_IMAGE_SUBTITLE_DIAGNOSTICS)
    foreach(target IN LISTS JELLYFIN_NATIVE_TARGETS)
        target_compile_definitions(${target} PRIVATE JELLYFIN_IMAGE_SUBTITLE_DIAGNOSTICS=1)
    endforeach()
endif()


set_source_files_properties(
    qml/primitives/InputKeys.qml
    qml/theme/Metrics.qml
    qml/theme/Theme.qml
    qml/theme/Typography.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE
)

qt_add_translations(jellyfin-native
    TS_FILES
        i18n/qtfin_en_US.ts
        i18n/qtfin_en_GB.ts
    RESOURCE_PREFIX /i18n
)

set(JELLYFIN_QML_MODULE_OPTIONS)
if(QT6_IS_SHARED_LIBS_BUILD)
    list(APPEND JELLYFIN_QML_MODULE_OPTIONS NO_IMPORT_SCAN)
else()
    list(APPEND JELLYFIN_QML_MODULE_OPTIONS DISCARD_QML_CONTENTS)
endif()

qt_add_qml_module(jellyfin-native
    ${JELLYFIN_QML_MODULE_OPTIONS}
    URI JellyfinWebOS
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/browse/BrowseSurface.qml
        qml/browse/ListBrowseView.qml
        qml/browse/RowStackView.qml
        qml/pages/HomePage.qml
        qml/pages/ItemDetailsPage.qml
        qml/pages/LibraryGridPage.qml
        qml/pages/LibraryNavigation.js
        qml/shell/ItemActivation.js
        qml/primitives/ModelAccess.js
        qml/pages/OpenSourceNoticesPage.qml
        qml/pages/PersonDetailsPage.qml
        qml/pages/PlayerBrowsePanel.qml
        qml/pages/PlayerAudioSyncPanel.qml
        qml/pages/PlayerMusicStage.qml
        qml/pages/PlayerOverlayChrome.qml
        qml/pages/PlayerOverlayInput.qml
        qml/pages/PlayerOverlayPage.qml
        qml/pages/PlayerQueuePanel.qml
        qml/pages/PlayerQueueRow.qml
        qml/pages/PlayerSeekBar.qml
        qml/pages/PlayerSkipSegmentCard.qml
        qml/pages/PlayerStatsPanel.qml
        qml/pages/PlayerTransportBar.qml
        qml/pages/PlayerTrickplayPreview.qml
        qml/pages/SearchPage.qml
        qml/pages/SettingsPage.qml
        qml/pages/SettingsNavigation.js
        qml/pages/SubtitleSettingsPanel.qml
        qml/primitives/ActionButton.qml
        qml/providers/jellyfin/LoginPage.qml
        qml/providers/jellyfin/LoginAccountStep.qml
        qml/providers/jellyfin/LoginBrandPanel.qml
        qml/providers/jellyfin/LoginProfileDialogs.qml
        qml/providers/jellyfin/LoginProfilePicker.qml
        qml/providers/jellyfin/LoginServerStep.qml
        qml/providers/jellyfin/ProfileNavigation.js
        qml/providers/jellyfin/RemoteControlPage.qml
        qml/providers/jellyfin/RemoteControlMenu.qml
        qml/providers/jellyfin/RemoteNowPlayingBar.qml
        qml/providers/jellyfin/SyncPlayMenu.qml
        qml/providers/jellyfin/ManagementDialog.qml
        qml/primitives/AppText.qml
        qml/primitives/AtomicViewReveal.qml
        qml/primitives/BusySpinner.qml
        qml/primitives/ChoiceStrip.qml
        qml/primitives/ConfirmationDialog.qml
        qml/primitives/EmptyPlaceholder.qml
        qml/primitives/FastWheelHandler.qml
        qml/primitives/GridPointerArea.qml
        qml/primitives/GroupHeader.qml
        qml/primitives/HoldNavigationController.qml
        qml/primitives/InlineSlider.qml
        qml/primitives/IconButton.qml
        qml/primitives/ImageCard.qml
        qml/primitives/LazyMenuPanel.qml
        qml/primitives/OptionPickerDialog.qml
        qml/primitives/InputKeys.qml
        qml/primitives/MediaItemCard.qml
        qml/primitives/MediaListRow.qml
        qml/primitives/MediaRow.qml
        qml/primitives/MetadataChip.qml
        qml/primitives/MenuListView.qml
        qml/primitives/ListScrollBar.qml
        qml/primitives/MenuRow.qml
        qml/primitives/SecondaryText.qml
        qml/primitives/NavGrid.qml
        qml/primitives/MaterialIcon.qml
        qml/primitives/PopupMenuPanel.qml
        qml/primitives/PopupShield.qml
        qml/primitives/ProfileTile.qml
        qml/primitives/SectionHeader.qml
        qml/primitives/ServerCard.qml
        qml/primitives/SelectRow.qml
        qml/primitives/SettingRow.qml
        qml/primitives/SliderRow.qml
        qml/primitives/Surface.qml
        qml/primitives/TechMetadataLine.qml
        qml/primitives/TechnicalDetailsBar.qml
        qml/primitives/TextFieldRow.qml
        qml/primitives/ToggleRow.qml
        qml/shell/AppShell.qml
        qml/shell/HdrUiLayer.qml
        qml/shell/KeyRouter.qml
        qml/shell/BusyOverlay.qml
        qml/shell/DiagnosticsOverlay.qml
        qml/shell/InputLatencyWarning.qml
        qml/shell/ItemContextMenu.qml
        qml/shell/RememberedCertificatesDialog.qml
        qml/shell/MediaInfoOverlay.qml
        qml/shell/OverlayDialog.qml
        qml/shell/PageReadiness.js
        qml/shell/RoutePolicy.js
        qml/shell/RouteStack.qml
        qml/shell/TlsTrustDialog.qml
        qml/shell/UpdateDialog.qml
        qml/shell/ToastLayer.qml
        qml/shell/TopBar.qml
        qml/shell/TopBarNavigation.js
        qml/shell/VideoSurface.qml
        qml/theme/Metrics.qml
        qml/theme/Theme.qml
        qml/theme/Typography.qml
)

# Qt Quick consumes baked shader packages on every RHI backend. Keep the
# resource independent of the QML module so the early startup frame can use it.
qt_add_shaders(jellyfin-native "hdr_ui_shaders"
    PREFIX "/shaders"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/qml/shell/shaders"
    FILES qml/shell/shaders/sdr-ui-to-scrgb.frag
)
if(NOT JELLYFIN_NATIVE_WEBOS)
    qt_target_qml_sources(jellyfin-native
        QML_FILES
            qml/pages/DesktopFolderDialog.qml
    )
endif()

if(JELLYFIN_NATIVE_WEBOS)
    qt_add_resources(jellyfin-native "platform_branding"
        PREFIX "/branding"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/app/icons/png/spool"
        FILES app/icons/png/spool/64.png
    )
endif()

# The mark is part of the interface now, not only the desktop entry, so every
# platform carries its SVG source and requires QtSvg to render it.
qt_add_resources(jellyfin-native "application_icons"
    PREFIX "/icons"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/app/icons"
    FILES
        app/icons/spool.svg
        app/icons/spool-film.svg
)

# The dp the Android system frame is drawn at, so Qt's first frame lands on
# the same pixels. Read from the same manifest the drawable was written from.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/tools/manifests/splash.json" spool_splash_manifest)
string(JSON spool_splash_core_width GET "${spool_splash_manifest}" core width)
string(JSON spool_splash_core_height GET "${spool_splash_manifest}" core height)
string(JSON spool_splash_fraction GET "${spool_splash_manifest}" desktop coreWidthFraction)
if(ANDROID)
    # One package draws either launch screen, so both sizes are compiled in and
    # the running device picks between them the same way the resource qualifier
    # does. See splashCoreWidthDp() in main.cpp.
    string(JSON spool_splash_tv_dp GET "${spool_splash_manifest}" android tvCoreWidthDp)
    string(JSON spool_splash_phone_dp GET "${spool_splash_manifest}" android phoneCoreWidthDp)
    target_compile_definitions(jellyfin-native PRIVATE
        SPOOL_SPLASH_CORE_WIDTH_DP_TV=${spool_splash_tv_dp}
        SPOOL_SPLASH_CORE_WIDTH_DP_PHONE=${spool_splash_phone_dp}
    )
endif()
target_compile_definitions(jellyfin-native PRIVATE
    SPOOL_SPLASH_CORE_ASPECT=${spool_splash_core_width}.0/${spool_splash_core_height}.0
    SPOOL_SPLASH_CORE_WIDTH_FRACTION=${spool_splash_fraction}
)

qt_add_resources(jellyfin-native "startup_splash_qml"
    PREFIX "/startup"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/qml/shell"
    FILES
        qml/shell/StartupSplash.qml
        qml/shell/SplashContent.qml
        qml/shell/HdrUiLayer.qml
)
# webOS reads its launch screen off disk, where appinfo.json already names it.
if(NOT JELLYFIN_NATIVE_WEBOS)
    qt_add_resources(jellyfin-native "startup_splash_image"
        PREFIX "/startup"
        BASE "${SPOOL_SPLASH_DIR}"
        FILES "${SPOOL_SPLASH_DIR}/splash-core.png"
    )
endif()

if(ANDROID)
    qt_add_resources(jellyfin-native "android_fonts"
        PREFIX "/fonts"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/qml/fonts"
        FILES
            qml/fonts/AtkinsonHyperlegible-Bold.otf
            qml/fonts/AtkinsonHyperlegible-Regular.otf
            qml/fonts/IBMPlexSans-Variable.ttf
            qml/fonts/PTRootUI-Variable.ttf
            qml/fonts/MaterialIcons-Regular.ttf
    )
endif()


add_custom_target(jellyfin-native_qmllint_strict
    COMMAND Qt6::qmllint
        --unqualified disable
        --missing-property disable
        --property-override warning
        "@${CMAKE_CURRENT_BINARY_DIR}/.rcc/qmllint/jellyfin-native.rsp"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Linting QML property overrides"
    VERBATIM
)
add_dependencies(jellyfin-native_qmllint_strict all_qmltyperegistrations)

set_target_properties(jellyfin-native PROPERTIES
    QT_QMLCACHEGEN_DIRECT_CALLS ON
)

# qmlimportscanner also scans the target's SOURCE_DIR recursively. This project
# keeps build trees and vendored sources below the repository root, so exclude
# them and let the generated QRC files provide the application's exact QML set.
set_property(TARGET jellyfin-native APPEND PROPERTY
    QT_QML_IMPORT_SCANNER_EXTRA_ARGS
        -exclude build
        -exclude build-arm
        -exclude tests
        -exclude build-linux-dev
        -exclude dist
        -exclude mpv
        -exclude squashfs-root
        -exclude third_party
)
if(JELLYFIN_NATIVE_WEBOS)
    set_property(TARGET jellyfin-native APPEND PROPERTY
        QT_QML_IMPORT_SCANNER_EXTRA_ARGS
            -exclude qml/pages/DesktopFolderDialog.qml
    )
endif()


get_target_property(JELLYFIN_NATIVE_INCLUDE_DIRS jellyfin-native INCLUDE_DIRECTORIES)
if(JELLYFIN_NATIVE_INCLUDE_DIRS)
    list(REMOVE_ITEM JELLYFIN_NATIVE_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}"
    )
    set_property(TARGET jellyfin-native PROPERTY
        INCLUDE_DIRECTORIES "${JELLYFIN_NATIVE_INCLUDE_DIRS}"
    )
endif()

target_include_directories(jellyfin-core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/player"
    "${MPV_ACTIVE_SOURCE_DIR}"
    "${MPV_ACTIVE_SOURCE_DIR}/include"
)

target_link_libraries(jellyfin-core PUBLIC
    QCoro::Core
    QCoro::Network
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Qml
    Qt6::Quick
    Qt6::Sql
    Qt6::OpenGL
    Qt6::WebSockets
)
if(NOT ANDROID)
    target_link_libraries(jellyfin-core PUBLIC Qt6::GuiPrivate)
endif()

target_link_libraries(jellyfin-native PRIVATE
    jellyfin-core
    Qt6::Svg
)


if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    # Tests are grouped into one executable per directory. Each test keeps its
    # own ctest entry; tests/TestRunner.cpp dispatches on the selector passed
    # by add_test(). One executable per test used to cost more to build on
    # Windows than the application itself, because every one of them repeated a
    # whole-program-optimized link against all of jellyfin-core.
    set(JELLYFIN_TEST_GROUPS api app cache common diagnostics discovery platform player providers)

    set(JELLYFIN_TESTS_api
        http-request-policy tests/api/HttpRequestPolicyTest.cpp
        playback-bandwidth-policy tests/api/PlaybackBandwidthPolicyTest.cpp
        items-query tests/api/ItemsQueryTest.cpp
        playback-negotiation tests/api/PlaybackNegotiationTest.cpp
        jellyfin-api-facade-url tests/api/JellyfinApiFacadeUrlTest.cpp
    )
    set(JELLYFIN_TESTS_app
        browse-session-controller tests/app/BrowseSessionControllerTest.cpp
        settings-schema tests/app/SettingsSchemaTest.cpp
        settings-controller tests/app/SettingsControllerTest.cpp
        session-controller tests/app/SessionControllerTest.cpp
        router-controller tests/app/RouterControllerTest.cpp
        library-query tests/app/LibraryQueryTest.cpp
        content-model-controller tests/app/ContentModelControllerTest.cpp
        search-controller tests/app/SearchControllerTest.cpp
        remote-control-controller tests/app/RemoteControlControllerTest.cpp
        spool-remote-protocol tests/app/SpoolRemoteProtocolTest.cpp
        spool-link-codec tests/app/SpoolLinkCodecTest.cpp
        sync-play-clock tests/app/SyncPlayClockTest.cpp
        sync-play-queue-handoff tests/app/SyncPlayQueueHandoffTest.cpp
        sync-play-drift-policy tests/app/SyncPlayDriftPolicyTest.cpp
        update-manifest tests/app/UpdateManifestTest.cpp
        artwork-url tests/app/ArtworkUrlTest.cpp
        provider-registry tests/app/ProviderRegistryTest.cpp
    )
    set(JELLYFIN_TESTS_cache
        database-manager tests/cache/DatabaseManagerTest.cpp
    )
    set(JELLYFIN_TESTS_common
        request-generation tests/common/RequestGenerationTest.cpp
        diagnostic-redaction tests/common/DiagnosticRedactionTest.cpp
        tls-trust tests/common/TlsTrustTest.cpp
        browse-descriptor tests/common/BrowseDescriptorTest.cpp
        meta-json tests/common/MetaJsonTest.cpp
    )
    set(JELLYFIN_TESTS_diagnostics
        input-latency-monitor tests/diagnostics/InputLatencyMonitorTest.cpp
        route-transition-timing tests/diagnostics/RouteTransitionTimingTest.cpp
        system-performance-monitor tests/diagnostics/SystemPerformanceMonitorTest.cpp
    )
    set(JELLYFIN_TESTS_discovery
        discovery-controller tests/discovery/DiscoveryControllerTest.cpp
    )
    set(JELLYFIN_TESTS_platform
        screensaver-inhibitor tests/platform/ScreenSaverInhibitorTest.cpp
        native-window-input tests/platform/NativeAppWindowInputTest.cpp
        platform-paths tests/platform/PlatformPathsTest.cpp
    )
    if(APPLE)
        list(APPEND JELLYFIN_TESTS_platform
            macos-titlebar tests/platform/MacOSTitlebarTest.mm
        )
    endif()
    set(JELLYFIN_TESTS_providers
        local-provider tests/providers/LocalProviderTest.cpp
        script-runtime tests/providers/ScriptRuntimeTest.cpp
        provider-media-page tests/providers/ProviderMediaPageTest.cpp
        source-registry tests/providers/SourceRegistryTest.cpp
        provider-ui-context tests/providers/ProviderUiContextTest.cpp
        provider-qml-cache tests/providers/ProviderQmlCacheTest.cpp
    )
    set(JELLYFIN_TESTS_player
        playback-timeline tests/player/PlaybackTimelineTest.cpp
        playback-track-parser tests/player/PlaybackTrackParserTest.cpp
        playback-track-state tests/player/PlaybackTrackStateTest.cpp
        playback-position-tracker tests/player/PlaybackPositionTrackerTest.cpp
        audio-sync-policy tests/player/AudioSyncPolicyTest.cpp
        mpv-option-profile tests/player/MpvOptionProfileTest.cpp
        render-target-profile tests/player/RenderTargetProfileTest.cpp
        playback-failure-policy tests/player/PlaybackFailurePolicyTest.cpp
        play-queue-controller tests/player/PlayQueueControllerTest.cpp
        play-queue-outline-model tests/player/PlayQueueOutlineModelTest.cpp
        series-audio-selection tests/player/SeriesAudioSelectionTest.cpp
    )
    # Both of these assert nothing away from Linux: the memory policy parses
    # /proc and cgroup text, and the mpv item needs a real GL surface.
    if(UNIX AND NOT APPLE)
        list(APPEND JELLYFIN_TESTS_platform
            linux-memory-policy tests/platform/LinuxMemoryPolicyTest.cpp
        )
        list(APPEND JELLYFIN_TESTS_player
            mpv-video-item tests/player/MpvVideoItemTest.cpp
        )
        # The same check over the Vulkan scene graph, which is the one an HDR
        # swapchain can be asked for. Its own entry rather than a loop inside
        # the test, so a machine with no Vulkan driver fails only this one.
        list(APPEND JELLYFIN_TESTS_player
            mpv-video-item-vulkan tests/player/MpvVideoItemTest.cpp
        )
    endif()

    # WebOSAudioSyncPolicy is a webOS platform source, so desktop builds of
    # jellyfin-core do not carry it; the player group compiles it directly.
    set(JELLYFIN_TEST_EXTRA_SOURCES_player src/platform/webos/WebOSAudioSyncPolicy.cpp)

    get_target_property(JELLYFIN_QT_CORE_LOCATION Qt6::Core IMPORTED_LOCATION_RELEASE)
    if(NOT JELLYFIN_QT_CORE_LOCATION)
        get_target_property(JELLYFIN_QT_CORE_LOCATION Qt6::Core IMPORTED_LOCATION)
    endif()
    set(JELLYFIN_TEST_ENV)
    if(JELLYFIN_QT_CORE_LOCATION)
        get_filename_component(JELLYFIN_QT_LIB_DIR "${JELLYFIN_QT_CORE_LOCATION}" DIRECTORY)
        get_filename_component(JELLYFIN_QT_PREFIX "${JELLYFIN_QT_LIB_DIR}" DIRECTORY)
        set(JELLYFIN_QT_PLUGIN_DIR "${JELLYFIN_QT_PREFIX}/lib/qt-6/plugins")
        if(EXISTS "${JELLYFIN_QT_PLUGIN_DIR}")
            list(APPEND JELLYFIN_TEST_ENV "QT_PLUGIN_PATH=${JELLYFIN_QT_PLUGIN_DIR}")
        endif()
    endif()
    foreach(test_group IN LISTS JELLYFIN_TEST_GROUPS)
        set(group_target "${test_group}-tests")
        set(group_sources tests/TestRunner.cpp ${JELLYFIN_TEST_EXTRA_SOURCES_${test_group}})
        set(group_names)
        list(LENGTH JELLYFIN_TESTS_${test_group} group_entry_count)
        math(EXPR group_entry_last "${group_entry_count} - 1")
        foreach(test_index RANGE 0 ${group_entry_last} 2)
            math(EXPR source_index "${test_index} + 1")
            list(GET JELLYFIN_TESTS_${test_group} ${test_index} test_name)
            list(GET JELLYFIN_TESTS_${test_group} ${source_index} test_source)
            list(APPEND group_sources "${test_source}")
            list(APPEND group_names "${test_name}")
        endforeach()

        add_executable(${group_target} ${group_sources})
        target_include_directories(${group_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        target_compile_definitions(${group_target} PRIVATE TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        # No test declares Q_OBJECT, and none of them ships, so skip both the
        # per-target autogen pass and whole-program optimization.
        set_target_properties(${group_target} PROPERTIES
            AUTOMOC OFF
            AUTOUIC OFF
            AUTORCC OFF
            INTERPROCEDURAL_OPTIMIZATION OFF
            INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF
            INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL OFF)
        target_link_libraries(${group_target} PRIVATE jellyfin-core)

        foreach(test_name IN LISTS group_names)
            add_test(NAME ${test_name} COMMAND ${group_target} ${test_name})
            if(JELLYFIN_TEST_ENV)
                set_tests_properties(${test_name} PROPERTIES ENVIRONMENT "${JELLYFIN_TEST_ENV}")
            endif()
        endforeach()
    endforeach()

    add_test(NAME provider-media-page-interpreter COMMAND providers-tests provider-media-page)
    set_tests_properties(provider-media-page-interpreter PROPERTIES ENVIRONMENT "QV4_FORCE_INTERPRETER=1;${JELLYFIN_TEST_ENV}")
    target_sources(providers-tests PRIVATE ${spool_provider_bundle_sources})
    add_test(NAME provider-package
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/provider_package_test.py")
    add_executable(provider-contract-runner sdk/provider-contract-runner.cpp)
    target_link_libraries(provider-contract-runner PRIVATE Qt6::Core Qt6::Qml)
    target_sources(provider-contract-runner PRIVATE ${spool_provider_bundle_sources})
    add_test(NAME bundled-jellyfin
        COMMAND provider-contract-runner "${CMAKE_CURRENT_SOURCE_DIR}/tests/providers/fixtures/bundled-jellyfin.mjs")
    add_test(NAME bundled-jellyfin-interpreter
        COMMAND provider-contract-runner "${CMAKE_CURRENT_SOURCE_DIR}/tests/providers/fixtures/bundled-jellyfin.mjs")
    set_tests_properties(bundled-jellyfin-interpreter PROPERTIES ENVIRONMENT "QV4_FORCE_INTERPRETER=1")
    if(JELLYFIN_NATIVE_WEBOS)
        set(JELLYFIN_FFMPEG_AUDIT_PLATFORM webos)
    elseif(APPLE)
        set(JELLYFIN_FFMPEG_AUDIT_PLATFORM macos)
    elseif(WIN32)
        set(JELLYFIN_FFMPEG_AUDIT_PLATFORM windows)
    else()
        set(JELLYFIN_FFMPEG_AUDIT_PLATFORM linux)
    endif()
    set(JELLYFIN_FFMPEG_CAPABILITY_HEADER
        "${CMAKE_CURRENT_BINARY_DIR}/generated/FfmpegCapabilities.h")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/ffmpeg-capabilities.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/manifests/ffmpeg-capabilities.json")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/ffmpeg-capabilities.py"
            cpp-header
            --platform "${JELLYFIN_FFMPEG_AUDIT_PLATFORM}"
            --output "${JELLYFIN_FFMPEG_CAPABILITY_HEADER}"
        RESULT_VARIABLE JELLYFIN_FFMPEG_HEADER_RESULT
        ERROR_VARIABLE JELLYFIN_FFMPEG_HEADER_ERROR
    )
    if(NOT JELLYFIN_FFMPEG_HEADER_RESULT EQUAL 0)
        message(FATAL_ERROR "FFmpeg capability header generation failed: ${JELLYFIN_FFMPEG_HEADER_ERROR}")
    endif()

    if(NOT WIN32)
        # Windows packages expose libmpv but do not ship FFmpeg development
        # libraries or pkg-config; keep its manifest generator test below,
        # but only build the linked compliance test where FFmpeg is available.
        # pkg_check_modules caches its result, so a tree carried over from an
        # earlier build keeps resolving whichever FFmpeg it first found. The
        # compliance test then audits a library the app no longer links, and
        # reports the previous build's licence and feature set. The capability
        # manifest is already a configure dependency; make the lookup follow it
        # rather than the cache.
        # pkg_check_modules caches both its search result and the absolute path
        # it resolved for every library, so a build tree carried over from an
        # earlier run keeps linking whichever FFmpeg it first found. The
        # compliance test then audits a library the app no longer links and
        # reports the previous build's licence and feature set. Clearing the
        # prefix is the documented way to make the lookup happen again; the
        # capability manifest is already a configure dependency, so this is what
        # makes the audit follow it.
        get_cmake_property(JELLYFIN_CACHE_VARIABLES CACHE_VARIABLES)
        foreach(cache_variable IN LISTS JELLYFIN_CACHE_VARIABLES)
            if(cache_variable MATCHES "^(pkgcfg_lib_FFMPEG_AUDIT_|__pkg_config_checked_FFMPEG_AUDIT|FFMPEG_AUDIT_)")
                unset(${cache_variable} CACHE)
            endif()
        endforeach()
        pkg_check_modules(FFMPEG_AUDIT REQUIRED IMPORTED_TARGET libavutil libavcodec libavformat libavfilter)
        add_executable(ffmpeg-compliance-tests tests/media/FfmpegComplianceTest.cpp)
        set_target_properties(ffmpeg-compliance-tests PROPERTIES
            AUTOMOC OFF
            AUTOUIC OFF
            AUTORCC OFF
            INTERPROCEDURAL_OPTIMIZATION OFF
            INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF
            INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL OFF)
        target_include_directories(ffmpeg-compliance-tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
        target_compile_definitions(ffmpeg-compliance-tests PRIVATE
            TEST_MEDIA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/media/fixtures")
        target_link_libraries(ffmpeg-compliance-tests PRIVATE PkgConfig::FFMPEG_AUDIT)
        add_test(NAME ffmpeg-compliance COMMAND ffmpeg-compliance-tests)
    endif()
    add_test(NAME ffmpeg-capability-generator
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/FfmpegCapabilitiesTest.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/ffmpeg-capabilities.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/manifests/ffmpeg-capabilities.json"
    )
    add_test(NAME package-audit
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/PackageAuditTest.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/package-audit.py"
    )
    add_test(NAME module-seam
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tools/check-module-seam.sh"
    )

    add_test(NAME network-shaper
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/NetworkShaperTest.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/network-shaper.py"
    )

    add_test(NAME render-benchmark-report
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/RenderBenchmarkReportTest.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/compare-render-benchmark.py"
    )

    find_program(QMLTESTRUNNER_EXECUTABLE NAMES qmltestrunner)
    if(QMLTESTRUNNER_EXECUTABLE)
        set(JELLYFIN_QML_TESTS
            route-policy tests/qml/tst_RoutePolicy.qml
            key-router tests/qml/tst_KeyRouter.qml
            player-overlay-input tests/qml/tst_PlayerOverlayInput.qml
            popup-menu-input tests/qml/tst_PopupMenuInput.qml
            option-picker-input tests/qml/tst_OptionPickerInput.qml
            media-row tests/qml/tst_MediaRow.qml
            metrics tests/qml/tst_Metrics.qml
            item-activation tests/qml/tst_ItemActivation.qml
            model-access tests/qml/tst_ModelAccess.qml
            tls-trust-dialogs tests/qml/tst_TlsTrustDialogs.qml
            nav-grid tests/qml/tst_NavGrid.qml
            grid-pointer-area tests/qml/tst_GridPointerArea.qml
            viewport-focus-recovery tests/qml/tst_ViewportFocusRecovery.qml
            hold-navigation-controller tests/qml/tst_HoldNavigationController.qml
            library-navigation tests/qml/tst_LibraryNavigation.qml
            profile-navigation tests/qml/tst_ProfileNavigation.qml
            settings-navigation tests/qml/tst_SettingsNavigation.qml
            settings-disclosure tests/qml/tst_SettingsDisclosure.qml
            settings-row-identity tests/qml/tst_SettingsRowIdentity.qml
            atomic-view-reveal tests/qml/tst_AtomicViewReveal.qml
            input-latency-warning tests/qml/tst_InputLatencyWarning.qml
            player-seek-bar-hover tests/qml/tst_PlayerSeekBarHover.qml
            list-scroll-bar tests/qml/tst_ListScrollBar.qml
            inline-slider tests/qml/tst_InlineSlider.qml
            provider-gating tests/qml/tst_ProviderGating.qml
        )
        # The subtitle panel test has never run on macOS; it was narrowed to
        # Windows alone while chasing that, which silently dropped Linux too.
        if(NOT APPLE)
            list(APPEND JELLYFIN_QML_TESTS
                subtitle-settings-panel tests/qml/tst_SubtitleSettingsPanel.qml
            )
        endif()

        get_filename_component(JELLYFIN_QML_PREFIX "${Qt6Qml_DIR}/../../.." ABSOLUTE)
        set(JELLYFIN_QML_IMPORT_DIR "${JELLYFIN_QML_PREFIX}/lib/qt-6/qml")
        set(JELLYFIN_QML_TEST_ENV
            "QT_QPA_PLATFORM=offscreen"
            "QT_QUICK_BACKEND=software"
            "QML2_IMPORT_PATH=${JELLYFIN_QML_IMPORT_DIR}"
            "QML_IMPORT_PATH=${JELLYFIN_QML_IMPORT_DIR}"
            ${JELLYFIN_TEST_ENV}
        )

        list(LENGTH JELLYFIN_QML_TESTS JELLYFIN_QML_TEST_COUNT)
        math(EXPR JELLYFIN_QML_TEST_LAST "${JELLYFIN_QML_TEST_COUNT} - 1")
        foreach(qml_index RANGE 0 ${JELLYFIN_QML_TEST_LAST} 2)
            math(EXPR qml_source_index "${qml_index} + 1")
            list(GET JELLYFIN_QML_TESTS ${qml_index} qml_test_name)
            list(GET JELLYFIN_QML_TESTS ${qml_source_index} qml_test_source)
            add_test(NAME ${qml_test_name}
                COMMAND "${QMLTESTRUNNER_EXECUTABLE}"
                    -input "${CMAKE_CURRENT_SOURCE_DIR}/${qml_test_source}"
            )
            set_tests_properties(${qml_test_name} PROPERTIES ENVIRONMENT "${JELLYFIN_QML_TEST_ENV}")
        endforeach()
    endif()
endif()

# Static Qt: import plugins that the linker can't auto-discover. BUILD_SHARED_LIBS
# controls this project's libraries, not whether the imported Qt build is static.
get_target_property(JELLYFIN_QT_CORE_TARGET_TYPE Qt6::Core TYPE)
if(JELLYFIN_QT_CORE_TARGET_TYPE STREQUAL "STATIC_LIBRARY")
    jellyfin_import_static_platform_plugins(jellyfin-native)

    # Let qmlimportscanner derive the static QML plugin link set from the
    # application's imports. New imports now fail when their plugin is absent.
    qt_import_qml_plugins(jellyfin-native)

    # Add linker GC to strip unused code from static libs.
    # macOS ld64 doesn't accept --gc-sections (uses -dead_strip instead, on by
    # default at -O), so only apply this on non-Apple platforms.
    if(NOT APPLE)
        target_link_options(jellyfin-native PRIVATE
            "-Wl,--gc-sections"
        )
        target_compile_options(jellyfin-native PRIVATE
            "-ffunction-sections"
            "-fdata-sections"
        )
    endif()
endif()

set_target_properties(jellyfin-native PROPERTIES
    OUTPUT_NAME "jellyfin-native"
)
if(APPLE)
    set_property(TARGET jellyfin-native PROPERTY OUTPUT_NAME "Spool")
endif()
if(UNIX AND NOT APPLE AND NOT ANDROID)
    set_property(TARGET jellyfin-native PROPERTY INSTALL_RPATH "$ORIGIN/../lib")
endif()
target_compile_definitions(jellyfin-native PRIVATE
    JELLYFIN_VERSION="${PROJECT_VERSION}"
    $<$<BOOL:${TOUCHSCREEN}>:TOUCHSCREEN>
)

install(TARGETS jellyfin-native
    LIBRARY DESTINATION lib
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)

if(NOT ANDROID)
    set(JELLYFIN_FONT_INSTALL_DIRECTORY "fonts")
    if(APPLE)
        set(JELLYFIN_FONT_INSTALL_DIRECTORY "Spool.app/Contents/fonts")
    endif()
    install(FILES
        qml/fonts/AtkinsonHyperlegible-Bold.otf
        qml/fonts/AtkinsonHyperlegible-Regular.otf
        qml/fonts/IBMPlexSans-Variable.ttf
        qml/fonts/PTRootUI-Variable.ttf
        qml/fonts/MaterialIcons-Regular.ttf
        DESTINATION "${JELLYFIN_FONT_INSTALL_DIRECTORY}"
    )
endif()

if(UNIX AND NOT APPLE AND NOT ANDROID AND NOT JELLYFIN_NATIVE_WEBOS)
    install(FILES app/com.sachk.spool.desktop
        DESTINATION share/applications
    )
    install(FILES app/com.sachk.spool.metainfo.xml
        DESTINATION share/metainfo
        RENAME com.sachk.spool.appdata.xml
    )
    install(FILES app/icons/spool.svg
        DESTINATION share/icons/hicolor/scalable/apps
        RENAME com.sachk.spool.svg
    )
    foreach(icon_size 16 22 24 32 48 64 128 256 512)
        install(FILES "app/icons/png/spool/${icon_size}.png"
            DESTINATION "share/icons/hicolor/${icon_size}x${icon_size}/apps"
            RENAME com.sachk.spool.png
        )
    endforeach()
endif()
