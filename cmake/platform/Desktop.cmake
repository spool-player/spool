function(jellyfin_add_desktop_sources native_target core_target)
    target_sources(${core_target} PRIVATE
        src/platform/desktop/DesktopNativeAppWindow.cpp
        src/platform/desktop/DesktopMpvConfigPolicy.cpp
        src/platform/desktop/DesktopPlaybackSurface.cpp
        src/platform/desktop/DesktopPlaybackRuntime.cpp
    )
    target_sources(${native_target} PRIVATE
        src/platform/desktop/DesktopApplicationServices.cpp
    )
endfunction()
