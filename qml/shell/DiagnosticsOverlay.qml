import QtQuick
import QtQuick.Layouts
import Spool
import "../theme"
import "../primitives"

Item {
    id: root
    property string route: ""
    property string focusedItemId: ""
    // Only read behind the capability: a source without SyncPlay has no such
    // singleton to report on.
    readonly property var syncPlay: ProviderCapabilities.groupPlayback ? Group : null
    readonly property bool syncPlayActive: syncPlay ? syncPlay.enabled : false

    function formatBytes(bytes) {
        const value = Math.max(0, Number(bytes || 0))
        if (value >= 1024 * 1024 * 1024)
            return (value / (1024 * 1024 * 1024)).toFixed(1) + " GiB"
        return Math.round(value / (1024 * 1024)) + " MiB"
    }

    // A platform with no sampler reports zero for everything, and "0.0%" next
    // to stuttering playback reads as a measurement rather than an absence.
    function cpu(value) {
        if (!SystemPerformance.available)
            return "n/a"
        return Number(value || 0).toFixed(1) + "%"
    }

    // System-wide figures are a separate question from this process's own. An
    // Android app may read /proc/self but not /proc/stat, /proc/meminfo or
    // /proc/loadavg, so on a television every one of these is genuinely
    // unknowable while the app's own CPU is not.
    function systemCpu(value) {
        if (!SystemPerformance.systemStatsAvailable)
            return "n/a"
        return Number(value || 0).toFixed(1) + "%"
    }

    function signedMs(value) {
        const number = Number(value || 0)
        return (number > 0 ? "+" : "") + number.toFixed(2) + " ms"
    }

    NumberAnimation on opacity {
        running: root.visible
        from: 0
        to: 1
        duration: 80
    }

    Rectangle {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: Metrics.scaled(18)
        width: Metrics.scaled(420)
        height: diagColumn.implicitHeight + Metrics.scaled(24)
        color: "#E6161616"
        border.width: 1
        border.color: Theme.borderStrong
        radius: Theme.radiusMedium
        ColumnLayout {
            id: diagColumn
            anchors.fill: parent
            anchors.margins: Metrics.scaled(12)
            spacing: Metrics.scaled(4)
            SecondaryText {
                text: "Diagnostics"
                color: Theme.textPrimary
                font.weight: Font.DemiBold
            }
            SecondaryText {
                text: "CPU  system " + root.systemCpu(SystemPerformance.systemCpuPercent) + "  app " + root.cpu(
                          SystemPerformance.processCpuPercent)
            }
            SecondaryText {
                text: "mpv  total " + root.cpu(SystemPerformance.mpvCpuPercent) + "  video decode " + root.cpu(
                          SystemPerformance.videoDecodeCpuPercent)
            }
            SecondaryText {
                text: "audio  decode " + root.cpu(SystemPerformance.audioDecodeCpuPercent) + "  output " + root.cpu(
                          SystemPerformance.audioOutputCpuPercent)
            }
            SecondaryText {
                text: "Dropped frames  decoder " + Player.decoderDroppedFrames + "  output "
                      + Player.outputDroppedFrames + "  late " + Player.delayedFrames
            }
            // The number that catches a renderer falling behind. Video synced
            // to audio does not drop frames when it cannot keep up; it runs
            // slow, and only the rate shows it.
            SecondaryText {
                text: "Frame rate  " + Player.outputFps.toFixed(2) + " of " + Player.containerFps.toFixed(2) + " fps"
            }
            SecondaryText {
                visible: SystemPerformance.systemStatsAvailable
                text: "Load  " + SystemPerformance.loadOne.toFixed(2) + "  " + SystemPerformance.loadFive.toFixed(2)
                      + "  " + SystemPerformance.loadFifteen.toFixed(2)
            }
            SecondaryText {
                text: "App memory  " + root.formatBytes(SystemPerformance.processRssBytes) + " RSS  " + root.formatBytes(
                          SystemPerformance.processAnonymousBytes) + " anon"
            }
            SecondaryText {
                text: "System memory  " + root.formatBytes(SystemPerformance.systemUsedBytes) + " / " + root.formatBytes(
                          SystemPerformance.systemTotalBytes) + "  free " + root.formatBytes(
                          SystemPerformance.systemAvailableBytes)
            }
            SecondaryText {
                text: "Input  " + InputLatency.lastLatencyMs.toFixed(2) + " ms  worst " + InputLatency.worstLatencyMs.toFixed(
                          2) + " ms  budget " + InputLatency.frameBudgetMs.toFixed(2) + " ms"
            }
            SecondaryText {
                text: "Frames  late " + InputLatency.lateCount + "  missed " + InputLatency.missedFrameCount
                      + "  samples " + InputLatency.sampleCount
            }
            SecondaryText {
                visible: root.syncPlayActive
                Layout.maximumWidth: Metrics.scaled(396)
                text: !root.syncPlayActive ? "" : "Group  " + (root.syncPlay.groupState || "Unknown") + (
                                                 root.syncPlay.groupStateReason ? " / "
                                                                                  + root.syncPlay.groupStateReason :
                                                                                  "") + (root.syncPlay.waitingForPlayback
                                                                                         ? "  ·  waiting to play" : "")
                elide: Text.ElideRight
            }
            SecondaryText {
                visible: root.syncPlayActive
                Layout.maximumWidth: Metrics.scaled(396)
                text: !root.syncPlayActive ? "" : "Time sync  " + "server  offset " + root.signedMs(
                                                 root.syncPlay.clockOffsetMs) + "  ping " + Number(root.syncPlay.pingMs
                                                                                                   || 0).toFixed(2)
                                             + " ms"
                elide: Text.ElideRight
            }
            SecondaryText {
                visible: root.syncPlayActive
                Layout.maximumWidth: Metrics.scaled(396)
                text: !root.syncPlayActive ? "" : "Playback drift  " + (root.syncPlay.playbackDiffValid ? root.signedMs(
                                                                                                              root.syncPlay.playbackDiffMs) :
                                                                                                          "—") + "  method "
                                             + root.syncPlay.syncMethod + "  (ignore <100 ms; seek ≥400 ms)"
                elide: Text.ElideRight
            }
            SecondaryText {
                Layout.maximumWidth: Metrics.scaled(396)
                text: "Stage  " + InputLatency.lastStage + (InputLatency.lastRouteSample.length > 0 ? "  ·  "
                                                                                                      + InputLatency.lastRouteSample :
                                                                                                      "")
                elide: Text.ElideRight
            }
            SecondaryText {
                Layout.maximumWidth: Metrics.scaled(396)
                text: "UI  " + root.width + "x" + root.height + "  " + Metrics.scale.toFixed(2) + "  " + Metrics.lane(
                          root.width) + "  " + root.route + (root.focusedItemId.length > 0 ? "  ·  "
                                                                                             + root.focusedItemId : "")
                elide: Text.ElideRight
            }
        }
    }
}
