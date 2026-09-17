import QtQuick

// SDR UI is composited as usual into a non-sRGB RGBA texture, then decoded
// once to linear BT.709 for the scRGB window. Never put a video item or an
// already-converted HdrUiLayer inside this item. Callers provide window state
// so the same component also works before the backend singletons exist.
Item {
    id: root

    property bool hdrOutput: false
    property real hdrSdrWhiteNits: 203

    // Keep ordinary Qt Quick blending in SDR, then decode only the finished
    // UI layer. The video is a sibling and already contains scRGB pixels.
    layer.enabled: hdrOutput
    layer.format: ShaderEffectSource.RGBA
    layer.effect: ShaderEffect {
        readonly property real whiteScale: root.hdrSdrWhiteNits / 80.0
        fragmentShader: "qrc:/shaders/sdr-ui-to-scrgb.frag.qsb"
    }
}
