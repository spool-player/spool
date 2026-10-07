cmake_minimum_required(VERSION 3.22)

file(READ "${CMAKE_CURRENT_LIST_DIR}/VERSION" SPOOL_VERSION)
string(STRIP "${SPOOL_VERSION}" SPOOL_VERSION)
if(NOT SPOOL_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must contain a semantic version such as 0.2.1")
endif()

project(SpoolWebOS VERSION "${SPOOL_VERSION}" LANGUAGES C CXX)

set(SPOOL_APPLE_MOBILE OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "tvOS")
    set(SPOOL_APPLE_MOBILE ON)
endif()

if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

set(SPOOL_WEBOS_DEFAULT OFF)
if(CMAKE_CROSSCOMPILING AND NOT ANDROID AND NOT SPOOL_APPLE_MOBILE)
    set(SPOOL_WEBOS_DEFAULT ON)
endif()
option(SPOOL_WEBOS "Build the webOS Starfish/native-surface client" ${SPOOL_WEBOS_DEFAULT})
option(TOUCHSCREEN "Enable mobile touchscreen interactions (runtime-disabled on TVs)" ${ANDROID})
option(SPOOL_DEV_BUILD "Favor faster local development builds over smallest release artifacts" OFF)
option(SPOOL_DIAGNOSTICS "Build debug lifecycle/process/watchdog diagnostics" OFF)
option(SPOOL_DIAGNOSTICS_STACKDUMP "Allow diagnostics to request gdb stack dumps" OFF)
option(SPOOL_DIAGNOSTICS_ABORT_ON_HANG "Abort after watchdog hang diagnostics are written" OFF)
option(SPOOL_ARTWORK_ASPECT_DIAGNOSTICS "Log artwork decode requests whose target aspect would stretch the source" OFF)
option(SPOOL_IMAGE_SUBTITLE_DIAGNOSTICS
    "Log image subtitle crop, segmentation, geometry, and recoloring per subtitle event" OFF)
option(SPOOL_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(SPOOL_MACOS_CREDENTIAL_SERVICE "com.sachk.spool.dev" CACHE STRING
    "macOS Keychain service used for persisted session tokens")
option(SPOOL_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
# Where the app may take provider code from. Release builds are open; a build
# submitted to Google Play or the App Store is curated (store catalogues only)
# or bundled (no downloads). See docs/providers.md.
set(SPOOL_PROVIDER_SOURCES "open" CACHE STRING "Provider sources: open, curated or bundled")
set_property(CACHE SPOOL_PROVIDER_SOURCES PROPERTY STRINGS open curated bundled)
option(SPOOL_APPLE_APP_STORE "Bundle only providers approved for Apple App Store builds" OFF)
if(SPOOL_APPLE_APP_STORE)
    set(SPOOL_PROVIDER_SOURCES "bundled" CACHE STRING "Provider sources: open, curated or bundled" FORCE)
endif()
if(NOT SPOOL_PROVIDER_SOURCES MATCHES "^(open|curated|bundled)$")
    message(FATAL_ERROR "SPOOL_PROVIDER_SOURCES must be open, curated or bundled, not '${SPOOL_PROVIDER_SOURCES}'")
endif()
include(CTest)

set(MPV_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/mpv" CACHE PATH "mpv source tree")
set(MPV_ACTIVE_SOURCE_DIR "${MPV_SOURCE_DIR}")
set(MPV_WEBOS_BUILD_DIR "${MPV_SOURCE_DIR}/build/webos-libmpv" CACHE PATH "webOS libmpv build directory")

if(SPOOL_DEV_BUILD)
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

set(SPOOL_QT_COMPONENTS
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
if(SPOOL_WEBOS)
    set(OPENSSL_USE_STATIC_LIBS FALSE)
    set(SPOOL_QT_MIN_VERSION 6.11)
    list(APPEND SPOOL_QT_COMPONENTS
        GuiPrivate
        QuickLayouts
        WaylandClient
    )
else()
    set(SPOOL_QT_MIN_VERSION 6.11)
    # rhi/qrhi.h is how the video item reaches the graphics device it shares
    # with libplacebo, and how the swapchain is asked what the display can show.
    if(NOT ANDROID)
        list(APPEND SPOOL_QT_COMPONENTS GuiPrivate)
    endif()
    if(UNIX AND NOT APPLE AND NOT ANDROID)
        list(APPEND SPOOL_QT_COMPONENTS DBus)
    endif()
endif()
find_package(Qt6 ${SPOOL_QT_MIN_VERSION} REQUIRED COMPONENTS ${SPOOL_QT_COMPONENTS})
if(SPOOL_APPLE_MOBILE)
    set(SPOOL_APPLE_QML_IMPORT_SCANNER "" CACHE FILEPATH "Patched pinned macOS QML import scanner")
    if(NOT EXISTS "${SPOOL_APPLE_QML_IMPORT_SCANNER}")
        message(FATAL_ERROR "Build tvOS with tools/build-tvos.sh to supply the patched host QML import scanner")
    endif()
    set_target_properties(Qt6::qmlimportscanner PROPERTIES
        IMPORTED_LOCATION "${SPOOL_APPLE_QML_IMPORT_SCANNER}"
        IMPORTED_LOCATION_RELEASE "${SPOOL_APPLE_QML_IMPORT_SCANNER}"
        IMPORTED_LOCATION_DEBUG "${SPOOL_APPLE_QML_IMPORT_SCANNER}"
        IMPORTED_LOCATION_RELWITHDEBINFO "${SPOOL_APPLE_QML_IMPORT_SCANNER}"
        IMPORTED_LOCATION_MINSIZEREL "${SPOOL_APPLE_QML_IMPORT_SCANNER}")
endif()
find_package(QCoro6 0.13 REQUIRED COMPONENTS Core Network)

qt_standard_project_setup(
    REQUIRES ${SPOOL_QT_MIN_VERSION}
    I18N_TRANSLATED_LANGUAGES en_US en_GB en_AU
)
include(cmake/platform/Common.cmake)

# The launch screen carries the version, so it is rendered per build rather
# than kept in the tree. tools/generate-splash.sh is the whole of the drawing;
# configuring is enough to get it, so no platform needs a second command.
if(ANDROID)
    set(spool_splash_platform "android")
elseif(SPOOL_WEBOS)
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


spool_resolve_platform_dependencies()

# Core never includes src/providers/ (native providers); every media source
# is reached through src/provider/. tools/check-module-seam.sh enforces it
# as the module-seam test.

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
    src/app/ArtworkImageProvider.cpp
    src/app/ArtworkImageProvider.h
    src/app/ArtworkPrefetcher.h
    src/app/ArtworkService.cpp
    src/app/ArtworkService.h
    src/app/TrickplayDecoder.cpp
    src/app/TrickplayDecoder.h
    src/app/TrickplayService.cpp
    src/app/TrickplayService.h
    src/app/TrickplayPreviewItem.cpp
    src/app/TrickplayPreviewItem.h
    src/app/BrowseSessionController.cpp
    src/app/BrowseSessionController.h
    src/app/ContentModelController.cpp
    src/app/ContentModelController.h
    src/app/CollectionEditingController.cpp
    src/app/CollectionEditingController.h
    src/app/DownloadManager.cpp
    src/app/DownloadManager.h
    src/platform/AndroidDownloadStorage.cpp
    src/platform/AndroidDownloadStorage.h
    src/app/RemoteTargetsController.cpp
    src/app/RemoteTargetsController.h
    src/app/HomeModelController.cpp
    src/app/HomeModelController.h
    src/app/LocalThumbnail.cpp
    src/app/LocalThumbnail.h
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
    src/app/SettingsSyncController.cpp
    src/app/SettingsSyncController.h
    src/app/SettingsSyncDocument.cpp
    src/app/SettingsSyncDocument.h
    src/app/UpdateManifest.cpp
    src/app/UpdateManifest.h
    src/app/AppController.cpp
    src/app/AppController.h
    src/app/AppControllerRemote.cpp
    src/app/GroupClock.cpp
    src/app/GroupClock.h
    src/app/GroupPlaybackController.cpp
    src/app/GroupPlaybackController.h
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
    src/provider/PortableProvider.cpp
    src/provider/PortableProvider.h
    src/provider/Provider.h
    src/provider/ProviderCapabilities.h
    src/provider/ProviderCapabilityContract.cpp
    src/provider/ProviderCapabilityContract.h
    src/provider/ProviderExtensionData.cpp
    src/provider/ProviderExtensionData.h
    src/provider/ProviderLogging.h
    src/provider/ProviderMediaPage.cpp
    src/provider/ProviderMediaPage.h
    src/provider/ProviderPackage.cpp
    src/provider/ProviderPackage.h
    src/provider/ProviderQmlCache.cpp
    src/provider/ProviderQmlCache.h
    src/provider/ProviderRegistry.cpp
    src/provider/ProviderRegistry.h
    src/provider/ProviderStore.cpp
    src/provider/ProviderStore.h
    src/provider/ProviderUiContext.cpp
    src/provider/ProviderUiContext.h
    src/provider/ScriptBridge.cpp
    src/provider/ScriptBridge.h
    src/provider/LanProbe.cpp
    src/provider/LanProbe.h
    src/provider/ScriptRuntime.cpp
    src/provider/ScriptRuntime.h
    src/provider/SpeedTest.cpp
    src/provider/SpeedTest.h
    src/provider/SearchSource.h
    src/provider/DownloadSource.h
    src/provider/SourceHub.cpp
    src/provider/SourceHubRemote.cpp
    src/provider/SourceHub.h
    src/provider/StreamQualityControl.h
    src/provider/UserItemStateSink.h
    third_party/zstd/bounded_zstd.c
    third_party/zstd/bounded_zstd.h
    src/diagnostics/Diagnostics.cpp
    src/diagnostics/Diagnostics.h
    src/diagnostics/InputLatencyMonitor.cpp
    src/diagnostics/InputLatencyMonitor.h
    src/diagnostics/RenderBenchmark.cpp
    src/diagnostics/RenderBenchmark.h
    src/diagnostics/SystemPerformanceMonitor.cpp
    src/diagnostics/SystemPerformanceMonitor.h
)

# The one native provider: media in a folder, and the contract's fixture.
set(SPOOL_LOCAL_SOURCES
    src/providers/local/LocalProvider.cpp
    src/providers/local/LocalProvider.h
)

set(SPOOL_CORE_SOURCES
    ${SPOOL_PLATFORM_SOURCES}
    ${SPOOL_PLAYER_SOURCES}
    ${SPOOL_SHELL_SOURCES}
    ${SPOOL_LOCAL_SOURCES}
)

add_library(spool-core STATIC ${SPOOL_CORE_SOURCES})

# Raw preview decoding uses the same libjpeg selected by the Qt toolchain,
# never a bundled second codec. The compact scene-graph textures use QRhi.
find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)
find_package(Qt6BundledLibjpeg QUIET CONFIG)
if(TARGET Qt6::BundledLibjpeg)
    find_package(Qt6 REQUIRED COMPONENTS JpegPrivate)
    target_link_libraries(spool-core PUBLIC Qt6::BundledLibjpeg Qt6::JpegPrivate Qt6::GuiPrivate)
    target_compile_definitions(spool-core PUBLIC SPOOL_QT_BUNDLED_JPEG=1)
else()
    find_package(JPEG REQUIRED)
    target_link_libraries(spool-core PUBLIC JPEG::JPEG Qt6::GuiPrivate)
endif()

qt_add_executable(spool
    src/main.cpp
    src/app/GraphicsStartup.cpp
    src/app/GraphicsStartup.h
)

if(NOT SPOOL_WEBOS)
    target_sources(spool PRIVATE
        src/automation/LocalCommandServer.cpp
        src/automation/LocalCommandServer.h
        src/automation/LocalControlProtocol.cpp
        src/automation/LocalControlProtocol.h
    )
endif()
if(NOT ANDROID AND NOT SPOOL_WEBOS AND NOT SPOOL_APPLE_MOBILE)
    add_executable(spoolet
        src/automation/SpooletMain.cpp
        src/automation/LocalControlProtocol.cpp
        src/automation/LocalControlProtocol.h
    )
    target_link_libraries(spoolet PRIVATE Qt6::Core Qt6::Network)
    target_include_directories(spoolet PRIVATE src)
    if(UNIX AND NOT APPLE)
        set_target_properties(spoolet PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
    endif()
    install(TARGETS spoolet RUNTIME DESTINATION bin)
endif()

find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
include(cmake/ProviderBundle.cmake)
spool_bundle_providers(spool)

set(SPOOL_TARGETS spool-core spool)
if(TARGET spoolet)
    list(APPEND SPOOL_TARGETS spoolet)
endif()
spool_configure_platform_targets(spool spool-core)
foreach(target IN LISTS SPOOL_TARGETS)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(SPOOL_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
        target_link_options(${target} PRIVATE
            "$<$<CONFIG:Release,MinSizeRel>:/OPT:REF>"
            "$<$<CONFIG:Release,MinSizeRel>:/OPT:ICF>"
        )
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(SPOOL_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
        if(SPOOL_SANITIZERS)
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endif()
    endif()
endforeach()

if(SPOOL_SANITIZERS AND MSVC)
    message(FATAL_ERROR "SPOOL_SANITIZERS requires GCC or Clang")
endif()


if(NOT SPOOL_DEV_BUILD)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT SPOOL_IPO_SUPPORTED OUTPUT SPOOL_IPO_ERROR)
    if(SPOOL_IPO_SUPPORTED)
        foreach(target IN LISTS SPOOL_TARGETS)
            set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
            set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL TRUE)
        endforeach()
    else()
        message(STATUS "Interprocedural optimization unavailable: ${SPOOL_IPO_ERROR}")
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        foreach(target IN LISTS SPOOL_TARGETS)
            target_compile_options(${target} PRIVATE -ffunction-sections -fdata-sections)
            if(ANDROID)
                target_compile_options(${target} PRIVATE -fPIC)
                target_link_options(${target} PRIVATE -Wl,--gc-sections -Wl,--as-needed -Wl,-z,relro,-z,now)
            elseif(UNIX AND NOT APPLE)
                if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|ARM)")
                    target_compile_options(${target} PRIVATE -fPIC)
                    if(target STREQUAL "spool")
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
if(ANDROID OR SPOOL_WEBOS OR SPOOL_APPLE_MOBILE)
    set(SPOOL_MPV_ITEM_RHI_ENABLED 0)
else()
    set(SPOOL_MPV_ITEM_RHI_ENABLED 1)
endif()
file(CONFIGURE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/MpvVideoItemBase.h"
    CONTENT "// Generated by CMake. Do not edit.
#pragma once

#define SPOOL_MPV_ITEM_RHI @SPOOL_MPV_ITEM_RHI_ENABLED@
"
    @ONLY)
foreach(target IN LISTS SPOOL_TARGETS)
    target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/generated")
endforeach()
if(SPOOL_DIAGNOSTICS)
    foreach(target IN LISTS SPOOL_TARGETS)
        target_compile_definitions(${target} PRIVATE SPOOL_DIAGNOSTICS=1)
    endforeach()
endif()
if(SPOOL_DIAGNOSTICS_STACKDUMP)
    foreach(target IN LISTS SPOOL_TARGETS)
        target_compile_definitions(${target} PRIVATE SPOOL_DIAGNOSTICS_STACKDUMP=1)
    endforeach()
endif()
if(SPOOL_DIAGNOSTICS_ABORT_ON_HANG)
    foreach(target IN LISTS SPOOL_TARGETS)
        target_compile_definitions(${target} PRIVATE SPOOL_DIAGNOSTICS_ABORT_ON_HANG=1)
    endforeach()
endif()
if(SPOOL_ARTWORK_ASPECT_DIAGNOSTICS)
    foreach(target IN LISTS SPOOL_TARGETS)
        target_compile_definitions(${target} PRIVATE SPOOL_ARTWORK_ASPECT_DIAGNOSTICS=1)
    endforeach()
endif()
if(SPOOL_IMAGE_SUBTITLE_DIAGNOSTICS)
    foreach(target IN LISTS SPOOL_TARGETS)
        target_compile_definitions(${target} PRIVATE SPOOL_IMAGE_SUBTITLE_DIAGNOSTICS=1)
    endforeach()
endif()


set_source_files_properties(
    qml/primitives/InputKeys.qml
    qml/theme/Metrics.qml
    qml/theme/Theme.qml
    qml/theme/Typography.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE
)

qt_add_translations(spool
    TS_FILES
        i18n/qtfin_en_US.ts
        i18n/qtfin_en_GB.ts
    RESOURCE_PREFIX /i18n
)

set(SPOOL_QML_MODULE_OPTIONS)
if(QT6_IS_SHARED_LIBS_BUILD)
    list(APPEND SPOOL_QML_MODULE_OPTIONS NO_IMPORT_SCAN)
else()
    list(APPEND SPOOL_QML_MODULE_OPTIONS DISCARD_QML_CONTENTS)
endif()

qt_add_qml_module(spool
    ${SPOOL_QML_MODULE_OPTIONS}
    URI Spool
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/browse/BrowseSurface.qml
        qml/browse/ListBrowseView.qml
        qml/browse/RowStackView.qml
        qml/pages/HomePage.qml
        qml/pages/ItemDetailsPage.qml
        qml/pages/CollectionEditorPage.qml
        qml/pages/LocalFolders.qml
        qml/pages/RemoteControlPage.qml
        qml/pages/LibraryGridPage.qml
        qml/pages/LibraryNavigation.js
        qml/shell/ItemActivation.js
        qml/primitives/ModelAccess.js
        qml/pages/OpenSourceNoticesPage.qml
        qml/pages/PersonDetailsPage.qml
        qml/pages/PlayerBrowsePanel.qml
        qml/pages/SettingsSyncPage.qml
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
        qml/pages/AccountsPage.qml
        qml/pages/AddProviderPage.qml
        qml/pages/ProviderScreenPage.qml
        qml/primitives/ActionButton.qml
        qml/primitives/AppText.qml
        qml/primitives/AtomicViewReveal.qml
        qml/primitives/BusySpinner.qml
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
        qml/primitives/ProviderIcon.qml
        qml/primitives/SectionHeader.qml
        qml/primitives/SourceBadge.qml
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
        qml/shell/GroupMenu.qml
        qml/shell/InputLatencyWarning.qml
        qml/shell/ItemContextMenu.qml
        qml/shell/DownloadsDialog.qml
        qml/shell/RememberedCertificatesDialog.qml
        qml/shell/MediaInfoOverlay.qml
        qml/shell/OverlayDialog.qml
        qml/shell/ProviderSurface.qml
        qml/shell/ServerLogin.qml
        qml/shell/ServerIdentityRow.qml
        qml/shell/ProviderCodePanel.qml
        qml/shell/ProviderLinkScreen.qml
        qml/shell/ProviderActionPicker.qml
        qml/shell/ProviderRemoteControls.qml
        qml/shell/RemoteNowPlaying.qml
        qml/shell/ProviderUpdatePrompt.qml
        qml/shell/PageReadiness.js
        qml/shell/RoutePolicy.js
        qml/shell/RouteStack.qml
        qml/shell/TlsTrustDialog.qml
        qml/shell/UpdateDialog.qml
        qml/shell/ProviderInstallDialog.qml
        qml/shell/ToastLayer.qml
        qml/shell/TopBar.qml
        qml/shell/VideoSurface.qml
        qml/theme/Metrics.qml
        qml/theme/Theme.qml
        qml/theme/Typography.qml
)

# Qt Quick consumes baked shader packages on every RHI backend. Keep the
# resource independent of the QML module so the early startup frame can use it.
qt_add_shaders(spool "hdr_ui_shaders"
    PREFIX "/shaders"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/qml/shell/shaders"
    FILES qml/shell/shaders/sdr-ui-to-scrgb.frag
)

qt_add_shaders(spool "trickplay_preview_shaders"
    PREFIX "/trickplay/shaders"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/src/app/shaders"
    BATCHABLE
    GLSL "100 es,120,150"
    FILES
        src/app/shaders/preview.vert
        src/app/shaders/rgb.frag
        src/app/shaders/yuv.frag
)
if(NOT SPOOL_WEBOS)
    qt_target_qml_sources(spool
        QML_FILES
            qml/pages/DesktopFolderDialog.qml
    )
endif()

if(SPOOL_WEBOS)
    qt_add_resources(spool "platform_branding"
        PREFIX "/branding"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/app/icons/png/spool"
        FILES app/icons/png/spool/64.png
    )
endif()

# The mark is part of the interface now, not only the desktop entry, so every
# platform carries its SVG source and requires QtSvg to render it.
qt_add_resources(spool "application_icons"
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
    target_compile_definitions(spool PRIVATE
        SPOOL_SPLASH_CORE_WIDTH_DP_TV=${spool_splash_tv_dp}
        SPOOL_SPLASH_CORE_WIDTH_DP_PHONE=${spool_splash_phone_dp}
    )
endif()
target_compile_definitions(spool PRIVATE
    SPOOL_SPLASH_CORE_ASPECT=${spool_splash_core_width}.0/${spool_splash_core_height}.0
    SPOOL_SPLASH_CORE_WIDTH_FRACTION=${spool_splash_fraction}
)

qt_add_resources(spool "startup_splash_qml"
    PREFIX "/startup"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/qml/shell"
    FILES
        qml/shell/StartupSplash.qml
        qml/shell/SplashContent.qml
        qml/shell/HdrUiLayer.qml
)
# webOS reads its launch screen off disk, where appinfo.json already names it.
if(NOT SPOOL_WEBOS)
    qt_add_resources(spool "startup_splash_image"
        PREFIX "/startup"
        BASE "${SPOOL_SPLASH_DIR}"
        FILES "${SPOOL_SPLASH_DIR}/splash-core.png"
    )
endif()

if(ANDROID)
    qt_add_resources(spool "android_fonts"
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


add_custom_target(spool_qmllint_strict
    COMMAND Qt6::qmllint
        --unqualified disable
        --missing-property disable
        --property-override warning
        "@${CMAKE_CURRENT_BINARY_DIR}/.rcc/qmllint/spool.rsp"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Linting QML property overrides"
    VERBATIM
)
add_dependencies(spool_qmllint_strict all_qmltyperegistrations)

set_target_properties(spool PROPERTIES
    QT_QMLCACHEGEN_DIRECT_CALLS ON
)

# qmlimportscanner also scans the target's SOURCE_DIR recursively. This project
# keeps build trees and vendored sources below the repository root, so exclude
# them and let the generated QRC files provide the application's exact QML set.
set_property(TARGET spool APPEND PROPERTY
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
if(SPOOL_WEBOS)
    set_property(TARGET spool APPEND PROPERTY
        QT_QML_IMPORT_SCANNER_EXTRA_ARGS
            -exclude qml/pages/DesktopFolderDialog.qml
    )
endif()


get_target_property(SPOOL_INCLUDE_DIRS spool INCLUDE_DIRECTORIES)
if(SPOOL_INCLUDE_DIRS)
    list(REMOVE_ITEM SPOOL_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}"
    )
    set_property(TARGET spool PROPERTY
        INCLUDE_DIRECTORIES "${SPOOL_INCLUDE_DIRS}"
    )
endif()

target_include_directories(spool-core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/player"
    "${MPV_ACTIVE_SOURCE_DIR}"
    "${MPV_ACTIVE_SOURCE_DIR}/include"
)

target_link_libraries(spool-core PUBLIC
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
if(ANDROID)
    target_link_libraries(spool-core PUBLIC Qt6::CorePrivate)
endif()
if(NOT ANDROID)
    target_link_libraries(spool-core PUBLIC Qt6::GuiPrivate)
endif()

target_link_libraries(spool PRIVATE
    spool-core
    Qt6::Svg
)


if(BUILD_TESTING AND NOT SPOOL_WEBOS)
    # Registry sources are shared by host binaries and real emulator bundles.
    # Each phase links once; selectors run in isolated instances of that binary.
    set(SPOOL_TEST_GROUPS app cache common diagnostics platform player providers)

    set(SPOOL_TESTS_app
        browse-session-controller tests/app/BrowseSessionControllerTest.cpp
        settings-schema tests/app/SettingsSchemaTest.cpp
        settings-controller tests/app/SettingsControllerTest.cpp
        settings-sync-controller tests/app/SettingsSyncControllerTest.cpp
        settings-sync-document tests/app/SettingsSyncDocumentTest.cpp
        router-controller tests/app/RouterControllerTest.cpp
        library-query tests/app/LibraryQueryTest.cpp
        library-list-model tests/app/LibraryListModelTest.cpp
        content-model-controller tests/app/ContentModelControllerTest.cpp
        collection-editing tests/app/CollectionEditingControllerTest.cpp
        download-manager tests/app/DownloadManagerTest.cpp
        remote-targets tests/app/RemoteTargetsControllerTest.cpp
        search-controller tests/app/SearchControllerTest.cpp
        group-clock tests/app/GroupClockTest.cpp
        group-playback-policy tests/app/GroupPlaybackPolicyTest.cpp
        update-manifest tests/app/UpdateManifestTest.cpp
        artwork-url tests/app/ArtworkUrlTest.cpp
        trickplay tests/app/TrickplayTest.cpp
    )
    set(SPOOL_TESTS_cache
        database-manager tests/cache/DatabaseManagerTest.cpp
    )
    set(SPOOL_TESTS_common
        request-generation tests/common/RequestGenerationTest.cpp
        diagnostic-redaction tests/common/DiagnosticRedactionTest.cpp
        tls-trust tests/common/TlsTrustTest.cpp
        browse-descriptor tests/common/BrowseDescriptorTest.cpp
        meta-json tests/common/MetaJsonTest.cpp
        bounded-zstd tests/common/BoundedZstdTest.cpp
        test-supervisor tests/common/TestSupervisorTest.cpp
    )
    set(SPOOL_TESTS_diagnostics
        input-latency-monitor tests/diagnostics/InputLatencyMonitorTest.cpp
        route-transition-timing tests/diagnostics/RouteTransitionTimingTest.cpp
        system-performance-monitor tests/diagnostics/SystemPerformanceMonitorTest.cpp
    )
    set(SPOOL_TESTS_platform
        screensaver-inhibitor tests/platform/ScreenSaverInhibitorTest.cpp
        native-window-input tests/platform/NativeAppWindowInputTest.cpp
        platform-paths tests/platform/PlatformPathsTest.cpp
    )
    if(APPLE)
        list(APPEND SPOOL_TESTS_platform
            macos-titlebar tests/platform/MacOSTitlebarTest.mm
        )
    endif()
    set(SPOOL_TESTS_providers
        local-provider tests/providers/LocalProviderTest.cpp
        script-runtime tests/providers/ScriptRuntimeTest.cpp
        provider-logging tests/providers/ProviderLoggingTest.cpp
        script-runtime-network tests/providers/ScriptRuntimeNetworkTest.cpp
        provider-media-page tests/providers/ProviderMediaPageTest.cpp
        provider-registry tests/providers/ProviderRegistryTest.cpp
        provider-activation tests/providers/ProviderActivationTest.cpp
        artwork-authentication tests/providers/ArtworkAuthenticationTest.cpp
        provider-extension-data tests/providers/ProviderExtensionDataTest.cpp
        source-hub tests/providers/SourceHubTest.cpp
        source-hub-speed-test tests/providers/SourceHubSpeedTest.cpp
        provider-store tests/providers/ProviderStoreTest.cpp
        provider-ui-context tests/providers/ProviderUiContextTest.cpp
        provider-qml-cache tests/providers/ProviderQmlCacheTest.cpp
        provider-package-unpack tests/providers/ProviderPackageTest.cpp
        stream-quality-control tests/providers/StreamQualityControlTest.cpp
        live-jellyfin tests/providers/LiveJellyfinTest.cpp
    )
    set(SPOOL_TESTS_player
        playback-timeline tests/player/PlaybackTimelineTest.cpp
        playback-track-parser tests/player/PlaybackTrackParserTest.cpp
        playback-track-state tests/player/PlaybackTrackStateTest.cpp
        playback-position-tracker tests/player/PlaybackPositionTrackerTest.cpp
        audio-sync-policy tests/player/AudioSyncPolicyTest.cpp
        mpv-option-profile tests/player/MpvOptionProfileTest.cpp
        render-target-profile tests/player/RenderTargetProfileTest.cpp
        playback-failure-policy tests/player/PlaybackFailurePolicyTest.cpp
        player-controller tests/player/PlayerControllerTest.cpp
        play-queue-controller tests/player/PlayQueueControllerTest.cpp
        play-queue-outline-model tests/player/PlayQueueOutlineModelTest.cpp
        series-audio-selection tests/player/SeriesAudioSelectionTest.cpp
    )
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND SPOOL_TESTS_platform
            linux-memory-policy tests/platform/LinuxMemoryPolicyTest.cpp
        )
    endif()
    set(SPOOL_TEST_RENDER_BACKEND "all" CACHE STRING "GPU tests: all or opengl")
    set_property(CACHE SPOOL_TEST_RENDER_BACKEND PROPERTY STRINGS all opengl)
    if(NOT SPOOL_TEST_RENDER_BACKEND MATCHES "^(all|opengl)$")
        message(FATAL_ERROR "SPOOL_TEST_RENDER_BACKEND must be all or opengl")
    endif()


    get_target_property(SPOOL_QT_CORE_LOCATION Qt6::Core IMPORTED_LOCATION_RELEASE)
    if(NOT SPOOL_QT_CORE_LOCATION)
        get_target_property(SPOOL_QT_CORE_LOCATION Qt6::Core IMPORTED_LOCATION)
    endif()
    # Tests load both bundled resources with reproducible timestamps and
    # mutable fixture packages. Keep every test process independent of the
    # persistent QML/JavaScript cache, not only the provider test group.
    set(SPOOL_TEST_ENV "QML_DISABLE_DISK_CACHE=1")
    if(SPOOL_QT_CORE_LOCATION)
        get_filename_component(SPOOL_QT_LIB_DIR "${SPOOL_QT_CORE_LOCATION}" DIRECTORY)
        get_filename_component(SPOOL_QT_PREFIX "${SPOOL_QT_LIB_DIR}" DIRECTORY)
        set(SPOOL_QT_PLUGIN_DIR "${SPOOL_QT_PREFIX}/lib/qt-6/plugins")
        if(EXISTS "${SPOOL_QT_PLUGIN_DIR}")
            list(APPEND SPOOL_TEST_ENV "QT_PLUGIN_PATH=${SPOOL_QT_PLUGIN_DIR}")
        endif()
    endif()
    set(SPOOL_TRADITIONAL_TEST_SOURCES src/platform/webos/WebOSAudioSyncPolicy.cpp)
    foreach(test_group IN LISTS SPOOL_TEST_GROUPS)
        list(LENGTH SPOOL_TESTS_${test_group} group_entry_count)
        math(EXPR group_entry_last "${group_entry_count} - 1")
        foreach(test_index RANGE 0 ${group_entry_last} 2)
            math(EXPR source_index "${test_index} + 1")
            list(GET SPOOL_TESTS_${test_group} ${source_index} test_source)
            list(APPEND SPOOL_TRADITIONAL_TEST_SOURCES "${test_source}")
        endforeach()
    endforeach()

    if(ANDROID OR SPOOL_APPLE_MOBILE)
        include(tests/platform/MobileTestBundles.cmake)
        spool_add_mobile_test_bundles()
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/spool-device-tests.json" "{\"format\":1}\n")
        if(ANDROID)
            set(SPOOL_TEST_DEVICE_PLATFORM android)
            set(SPOOL_TEST_DEVICE_ARTIFACTS "${CMAKE_CURRENT_SOURCE_DIR}/build/android/launch-test")
        else()
            set(SPOOL_TEST_DEVICE_PLATFORM tvos)
            set(SPOOL_TEST_DEVICE_ARTIFACTS "${CMAKE_CURRENT_SOURCE_DIR}/build/tvos/smoke")
        endif()
        foreach(phase IN ITEMS traditional e2e)
            add_test(NAME "${phase}-native-device"
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/run-device-tests.py"
                    --platform "${SPOOL_TEST_DEVICE_PLATFORM}" --phase "${phase}"
                    --artifact-dir "${SPOOL_TEST_DEVICE_ARTIFACTS}")
            set_tests_properties("${phase}-native-device" PROPERTIES LABELS "${phase}" TIMEOUT 0)
        endforeach()
    else()
    find_package(Threads REQUIRED)
    find_package(Qt6 REQUIRED COMPONENTS QuickTest)
    add_executable(spool-tests tests/TestRunner.cpp tests/TestVariants.cpp ${SPOOL_TRADITIONAL_TEST_SOURCES})
    target_include_directories(spool-tests PRIVATE tests src "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_compile_definitions(spool-tests PRIVATE TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        SPOOL_TEST_RUNNER=1)
    target_link_libraries(spool-tests PRIVATE spool-core Threads::Threads)
    target_sources(spool-tests PRIVATE ${SPOOL_PROVIDER_BUNDLE_SOURCES} sdk/provider-contract-runner.cpp)
    if(TARGET spoolet)
        target_sources(spool-tests PRIVATE tests/automation/LocalControlProtocolTest.cpp
            tests/automation/LocalControlAdmissionTest.cpp src/automation/LocalControlProtocol.cpp
            src/automation/LocalCommandServer.cpp)
    endif()
    add_test(NAME traditional-native COMMAND spool-tests --run-all --results "${CMAKE_CURRENT_BINARY_DIR}/traditional-results.json")
    set_tests_properties(traditional-native PROPERTIES LABELS traditional ENVIRONMENT "${SPOOL_TEST_ENV}" TIMEOUT 0)
    add_test(NAME provider-sdk-tool
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/provider_package_test.py")
    add_test(NAME local-provider-overrides
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/LocalProviderOverridesTest.py")
    if(SPOOL_WEBOS)
        set(SPOOL_FFMPEG_AUDIT_PLATFORM webos)
    elseif(APPLE)
        set(SPOOL_FFMPEG_AUDIT_PLATFORM macos)
    elseif(WIN32)
        set(SPOOL_FFMPEG_AUDIT_PLATFORM windows)
    else()
        set(SPOOL_FFMPEG_AUDIT_PLATFORM linux)
    endif()
    set(SPOOL_FFMPEG_CAPABILITY_HEADER
        "${CMAKE_CURRENT_BINARY_DIR}/generated/FfmpegCapabilities.h")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/ffmpeg-capabilities.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/manifests/ffmpeg-capabilities.json")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/ffmpeg-capabilities.py"
            cpp-header
            --platform "${SPOOL_FFMPEG_AUDIT_PLATFORM}"
            --output "${SPOOL_FFMPEG_CAPABILITY_HEADER}"
        RESULT_VARIABLE SPOOL_FFMPEG_HEADER_RESULT
        ERROR_VARIABLE SPOOL_FFMPEG_HEADER_ERROR
    )
    if(NOT SPOOL_FFMPEG_HEADER_RESULT EQUAL 0)
        message(FATAL_ERROR "FFmpeg capability header generation failed: ${SPOOL_FFMPEG_HEADER_ERROR}")
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
        get_cmake_property(SPOOL_CACHE_VARIABLES CACHE_VARIABLES)
        foreach(cache_variable IN LISTS SPOOL_CACHE_VARIABLES)
            if(cache_variable MATCHES "^(pkgcfg_lib_FFMPEG_AUDIT_|__pkg_config_checked_FFMPEG_AUDIT|FFMPEG_AUDIT_)")
                unset(${cache_variable} CACHE)
            endif()
        endforeach()
        pkg_check_modules(FFMPEG_AUDIT REQUIRED IMPORTED_TARGET libavutil libavcodec libavformat libavfilter)
        target_sources(spool-tests PRIVATE tests/media/FfmpegComplianceTest.cpp)
        target_compile_definitions(spool-tests PRIVATE
            TEST_MEDIA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/media/fixtures")
        target_link_libraries(spool-tests PRIVATE PkgConfig::FFMPEG_AUDIT)
    endif()
    add_test(NAME ffmpeg-capability-generator
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/FfmpegCapabilitiesTest.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/ffmpeg-capabilities.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/manifests/ffmpeg-capabilities.json"
    )
    # Builds ELF fixtures with cc; the AppImage is the only package it audits.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME package-audit
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/PackageAuditTest.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/package-audit.py"
        )
    endif()
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
    add_test(NAME test-driver-ordering
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/tools/TestDriverTest.py")

    set(SPOOL_E2E_SOURCES tests/TestRunner.cpp tests/QmlTestRunner.cpp tests/ExtensionIntegration.h
        tests/ArtworkIntegration.h tests/player/MpvVideoItemTest.cpp tests/player/MpvFullscreenTest.cpp
        tests/e2e/AppJourneyTest.cpp tests/e2e/JellyfinFixture.cpp)
    qt_add_executable(spool-e2e-tests ${SPOOL_E2E_SOURCES})
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native-spool-e2e-path.txt"
        CONTENT "$<TARGET_FILE:spool-e2e-tests>\n")
    target_include_directories(spool-e2e-tests PRIVATE tests src "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_compile_definitions(spool-e2e-tests PRIVATE TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        SPOOL_E2E_APP="$<TARGET_FILE:spool>" SPOOL_E2E_SPOOLET="$<TARGET_FILE:spoolet>")
    if(SPOOL_TEST_RENDER_BACKEND STREQUAL "opengl")
        target_compile_definitions(spool-e2e-tests PRIVATE SPOOL_TEST_OPENGL_ONLY=1)
    endif()
    target_link_libraries(spool-e2e-tests PRIVATE spool-core Qt6::QuickTest)
    add_dependencies(spool-e2e-tests spool spoolet)
    # The journey launches the actual build product, with its shipped runtime
    # fonts at the platform's normal application-root location.
    if(WIN32)
        set(SPOOL_E2E_FONT_DIR "$<TARGET_FILE_DIR:spool>/fonts")
    else()
        set(SPOOL_E2E_FONT_DIR "$<TARGET_FILE_DIR:spool>/../fonts")
    endif()
    add_custom_command(TARGET spool-e2e-tests POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/qml/fonts"
            "${SPOOL_E2E_FONT_DIR}"
        VERBATIM)
        set(SPOOL_QML_TESTS
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
            settings-navigation tests/qml/tst_SettingsNavigation.qml
            remote-control-page tests/qml/tst_RemoteControlPage.qml
            provider-install-dialog tests/qml/tst_ProviderInstallDialog.qml
            provider-shell tests/qml/tst_ProviderShell.qml
            settings-disclosure tests/qml/tst_SettingsDisclosure.qml
            settings-row-identity tests/qml/tst_SettingsRowIdentity.qml
            atomic-view-reveal tests/qml/tst_AtomicViewReveal.qml
            input-latency-warning tests/qml/tst_InputLatencyWarning.qml
            player-seek-bar-hover tests/qml/tst_PlayerSeekBarHover.qml
            list-scroll-bar tests/qml/tst_ListScrollBar.qml
            inline-slider tests/qml/tst_InlineSlider.qml
        )
        list(APPEND SPOOL_QML_TESTS
            subtitle-settings-panel tests/qml/tst_SubtitleSettingsPanel.qml
        )

        # Register component and integration selectors inside the GUI binary.
        list(APPEND SPOOL_QML_TESTS
            extension-integration tests/qml/tst_ExtensionIntegration.qml
            provider-forms tests/qml/tst_ProviderForms.qml
            artwork-integration tests/qml/tst_ArtworkIntegration.qml)
        if(QT6_IS_SHARED_LIBS_BUILD)
            set_property(TARGET spool-e2e-tests PROPERTY QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
        else()
            get_target_property(SPOOL_TEST_SCAN_ARGS spool QT_QML_IMPORT_SCANNER_EXTRA_ARGS)
            set_property(TARGET spool-e2e-tests PROPERTY QT_QML_IMPORT_SCANNER_EXTRA_ARGS ${SPOOL_TEST_SCAN_ARGS})
        endif()
        get_target_property(SPOOL_APP_SOURCES spool SOURCES)
        set(SPOOL_TEST_PLATFORM_SOURCES ${SPOOL_APP_SOURCES})
        list(FILTER SPOOL_TEST_PLATFORM_SOURCES INCLUDE REGEX "/[A-Za-z]+PlatformCapabilities\\.cpp$")
        set(SPOOL_TEST_QML_RESOURCES ${SPOOL_APP_SOURCES})
        list(FILTER SPOOL_TEST_QML_RESOURCES INCLUDE REGEX "/qrc_(qmake_Spool|application_icons|trickplay_preview_shaders|spool_raw_qml_[0-9]+(_extra_qmldirs)?)\\.cpp$")
        # Reuse the application's generated resources rather than declaring a
        # second Spool module (which would also overwrite qrc_qmake_Spool.cpp).
        target_sources(spool-e2e-tests PRIVATE ${SPOOL_TEST_PLATFORM_SOURCES} ${SPOOL_TEST_QML_RESOURCES})
        qt_add_resources(spool-e2e-tests "component_qml_tests"
            PREFIX "/qt/qml/Spool"
            FILES
                tests/qml/tst_MediaRow.qml
                tests/qml/tst_SubtitleSettingsPanel.qml
                tests/qml/tst_RemoteControlPage.qml
                tests/qml/tst_ProviderInstallDialog.qml
                tests/qml/tst_ProviderShell.qml
                tests/qml/tst_ExtensionIntegration.qml
                tests/qml/tst_ProviderForms.qml
                tests/qml/tst_ArtworkIntegration.qml
                tests/qml/tst_PlayerSeekBarHover.qml
        )

        set(qml_registry "${CMAKE_CURRENT_BINARY_DIR}/generated/QmlTestSelectors.cpp")
        file(WRITE "${qml_registry}" "#include \"TestMain.h\"\n#include <QByteArray>\nint spoolRunQml(int, char **, const char *);\nnamespace {\n")
        list(LENGTH SPOOL_QML_TESTS SPOOL_QML_TEST_COUNT)
        math(EXPR SPOOL_QML_TEST_LAST "${SPOOL_QML_TEST_COUNT} - 1")
        foreach(qml_index RANGE 0 ${SPOOL_QML_TEST_LAST} 2)
            math(EXPR qml_source_index "${qml_index} + 1")
            list(GET SPOOL_QML_TESTS ${qml_index} qml_test_name)
            list(GET SPOOL_QML_TESTS ${qml_source_index} qml_test_source)
            if(qml_test_name MATCHES "^(subtitle-settings-panel|remote-control-page|provider-install-dialog|provider-shell|player-seek-bar-hover|media-row|extension-integration|provider-forms|artwork-integration)$")
                set(qml_input ":/qt/qml/Spool/${qml_test_source}")
            else()
                set(qml_input "${CMAKE_CURRENT_SOURCE_DIR}/${qml_test_source}")
            endif()
            set(qml_environment "")
            if(qml_test_name MATCHES "^(extension-integration|provider-shell)$")
                set(qml_environment "qputenv(\"SPOOL_EXTENSION_INTEGRATION\", \"1\");")
            elseif(qml_test_name STREQUAL "artwork-integration")
                set(qml_environment "qputenv(\"SPOOL_ARTWORK_INTEGRATION\", \"1\");")
            endif()
            file(APPEND "${qml_registry}" "int test${qml_index}(int argc, char **argv) { ${qml_environment} return spoolRunQml(argc, argv, \"${qml_input}\"); }\nconst bool registered${qml_index} = SpoolTests::registerTest(\"${qml_test_name}\", &test${qml_index});\n")
        endforeach()
        file(APPEND "${qml_registry}" "}\n")
        target_sources(spool-e2e-tests PRIVATE "${qml_registry}")
        add_test(NAME gui-e2e-native COMMAND spool-e2e-tests --run-all --workers 1 --timeout-ms 600000 --results "${CMAKE_CURRENT_BINARY_DIR}/e2e-results.json")
        set_tests_properties(gui-e2e-native PROPERTIES LABELS e2e ENVIRONMENT "${SPOOL_TEST_ENV}" TIMEOUT 0)
        foreach(test_target IN ITEMS spool-tests spool-e2e-tests)
            set_target_properties(${test_target} PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF
                INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL OFF)
        endforeach()
        set_tests_properties(provider-sdk-tool local-provider-overrides ffmpeg-capability-generator module-seam
            network-shaper render-benchmark-report test-driver-ordering PROPERTIES LABELS traditional)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            set_tests_properties(package-audit PROPERTIES LABELS traditional)
        endif()
    endif()
    if(SPOOL_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        foreach(test_target IN ITEMS spool-tests spool-e2e-tests)
            target_compile_options(${test_target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${test_target} PRIVATE -fsanitize=address,undefined)
        endforeach()
    endif()
endif()

# Static Qt: import plugins that the linker can't auto-discover. BUILD_SHARED_LIBS
# controls this project's libraries, not whether the imported Qt build is static.
get_target_property(SPOOL_QT_CORE_TARGET_TYPE Qt6::Core TYPE)
if(SPOOL_QT_CORE_TARGET_TYPE STREQUAL "STATIC_LIBRARY")
    spool_import_static_platform_plugins(spool)

    # Let qmlimportscanner derive the static QML plugin link set from the
    # application's imports. New imports now fail when their plugin is absent.
    qt_import_qml_plugins(spool)

    # Add linker GC to strip unused code from static libs.
    # macOS ld64 doesn't accept --gc-sections (uses -dead_strip instead, on by
    # default at -O), so only apply this on non-Apple platforms.
    if(NOT APPLE)
        target_link_options(spool PRIVATE
            "-Wl,--gc-sections"
        )
        target_compile_options(spool PRIVATE
            "-ffunction-sections"
            "-fdata-sections"
        )
    endif()
endif()

set_target_properties(spool PROPERTIES
    OUTPUT_NAME "spool"
)
if(APPLE)
    set_property(TARGET spool PROPERTY OUTPUT_NAME "Spool")
endif()
if(UNIX AND NOT APPLE AND NOT ANDROID)
    set_property(TARGET spool PROPERTY INSTALL_RPATH "$ORIGIN/../lib")
endif()
target_compile_definitions(spool PRIVATE
    SPOOL_VERSION="${PROJECT_VERSION}"
    SPOOL_PROVIDER_SOURCES="${SPOOL_PROVIDER_SOURCES}"
    $<$<BOOL:${TOUCHSCREEN}>:TOUCHSCREEN>
)

install(TARGETS spool
    LIBRARY DESTINATION lib
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)

if(NOT ANDROID AND NOT SPOOL_APPLE_MOBILE)
    set(SPOOL_FONT_INSTALL_DIRECTORY "fonts")
    if(APPLE)
        set(SPOOL_FONT_INSTALL_DIRECTORY "Spool.app/Contents/fonts")
    endif()
    install(FILES
        qml/fonts/AtkinsonHyperlegible-Bold.otf
        qml/fonts/AtkinsonHyperlegible-Regular.otf
        qml/fonts/IBMPlexSans-Variable.ttf
        qml/fonts/PTRootUI-Variable.ttf
        qml/fonts/MaterialIcons-Regular.ttf
        DESTINATION "${SPOOL_FONT_INSTALL_DIRECTORY}"
    )
endif()

if(UNIX AND NOT APPLE AND NOT ANDROID AND NOT SPOOL_WEBOS)
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
