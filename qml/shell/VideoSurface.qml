import QtQuick
import JellyfinWebOS
import "../primitives"

FocusScope {
    id: root

    property bool active: false
    property bool mediaInfoVisible: false
    property bool diagnosticsVisible: false
    readonly property bool directionRelease: true
    readonly property bool titlebarVisible: !active || playerOverlay.controlsVisible
    onTitlebarVisibleChanged: {
        if (Qt.platform.os === "osx")
            NativeWindow.setTitlebarVisible(titlebarVisible)
    }

    // KeyRouter defers activation to key release for any target that merely
    // owns a longPress member, so this is advertised only while the queue panel
    // wants it. Everywhere else OK-to-pause stays a press-time action.
    property var longPress: playerOverlay.queuePanelVisible ? root.queuePanelLongPress : undefined

    signal playbackBackRequested(var item)

    visible: active
    enabled: active
    focus: active

    function focusInput() {
        InputKeys.focus(inputShield)
    }

    function back() {
        return playerOverlay.back()
    }

    function routeKey(key, phase, repeat) {
        return playerOverlay.routeKey(key, phase, repeat)
    }

    function unhandledKey(key, phase, repeat, modifiers, text) {
        return playerOverlay.unhandledKey(key, phase, repeat, modifiers, text)
    }

    function activate() {
        playerOverlay.activate()
    }

    function queuePanelLongPress() {
        return playerOverlay.queuePanelLongPress()
    }

    function finishOpeningGesture() {
        playerOverlay.queuePanelFinishGesture()
    }

    function openPlaybackSettings() {
        playerOverlay.openMenu("debug")
        focusInput()
    }

    function toggleOsd() {
        if (playerOverlay.controlsVisible)
            playerOverlay.hideControls()
        else
            playerOverlay.showControls("timeline")
        focusInput()
    }

    function showRemoteSeekPreview(seconds, active) {
        playerOverlay.showRemoteSeekPreview(seconds, active)
    }

    // Clear letterbox bars for embedded video and the audio stage. Native
    // video (Starfish or direct MediaCodec) sits beneath the Qt window, so
    // even a negative QML z would cover it: that path must stay transparent.
    Rectangle {
        anchors.fill: parent
        color: "black"
        visible: Player.embeddedVideoOutput || Player.mediaKind === "audio"
        z: -1
    }

    // Registered dynamically by main.cpp, so no static .qmltypes entry exists.
    MpvVideoItem {
        anchors.fill: parent
        visible: root.active && Player.embeddedVideoOutput
        opacity: 1.0
        z: 0
    }
    // qmllint enable import unresolved-type

    // The renderer remains a sibling: only SDR chrome, artwork, subtitles
    // handed over as an image, and diagnostics enter this conversion layer.
    // Their existing relative z values still move mpv stats above the OSD.
    HdrUiLayer {
        anchors.fill: parent
        hdrOutput: NativeWindow.hdrOutput
        hdrSdrWhiteNits: NativeWindow.hdrSdrWhiteNits
        z: 1

        // What mpv drew, on the outputs that hand it over rather than compositing
        // it into the picture themselves. Subtitles belong under the player's
        // chrome, the same as they do everywhere else -- but this image carries
        // mpv's stats page too, and a diagnostic that the controls can cover is
        // not a diagnostic. So while the stats page is up, the whole of it comes
        // to the front, above every part of the player.
        Image {
            x: NativeWindow.overlayX
            y: NativeWindow.overlayY
            width: NativeWindow.overlayWidth
            height: NativeWindow.overlayHeight
            visible: root.active && width > 0 && height > 0
            source: visible ? "image://mpv-overlay/live?rev=" + NativeWindow.overlayRevision : ""
            cache: false
            fillMode: Image.Stretch
            z: Player.debugOsdVisible ? 6 : 1
        }

        Loader {
            anchors.fill: parent
            active: root.active && Player.mediaKind === "audio"
            z: 0
            sourceComponent: PlayerMusicStage {
                overlay: playerOverlay
            }
        }

        PlayerOverlayPage {
            id: playerOverlay
            anchors.fill: parent
            visible: root.active
            onPlaybackBackRequested: item => root.playbackBackRequested(item)
            z: 2
        }

        FocusScope {
            id: inputShield
            anchors.fill: parent
            visible: root.active
            enabled: visible && !playerOverlay.subtitleSettingsVisible && !playerOverlay.queuePanelVisible
            focus: enabled
            z: 3

            onVisibleChanged: if (enabled)
                                  InputKeys.focus(inputShield)
            onEnabledChanged: if (enabled)
                                  InputKeys.focus(inputShield)
            onActiveFocusChanged: if (enabled && !activeFocus)
                                      Qt.callLater(() => InputKeys.focus(inputShield))
        }

        // Audio playback has no video output for mpv to draw its stats page into,
        // so the UI draws it — over the now playing stage, which is what it is
        // there to cover.
        Loader {
            anchors.fill: parent
            active: root.active && Player.mediaKind === "audio" && Player.debugOsdVisible
            z: 5
            sourceComponent: PlayerStatsPanel {}
        }

        // Performance stats bring the app's own numbers up with mpv's, for this
        // file only: the flag lives with the playback session, so it never outlasts
        // the file the way the settings switch does.
        Loader {
            anchors.fill: parent
            active: root.active && (root.diagnosticsVisible || Player.debugOsdVisible)
            z: 4
            sourceComponent: DiagnosticsOverlay {
                route: "player"
            }
        }
    }
}
