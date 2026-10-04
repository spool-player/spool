#version 440
precision highp float;
layout(location = 0) in highp vec2 texCoord;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec4 options;
    vec4 yBounds;
    vec4 chromaBounds;
    vec4 chromaScale;
} ubuf;
layout(binding = 1) uniform sampler2D rgbTexture;
void main()
{
    vec3 rgb = texture(rgbTexture, clamp(texCoord, ubuf.yBounds.xy, ubuf.yBounds.zw)).rgb;
    // A BGRA byte buffer can be uploaded unchanged to RGBA8 if BGRA8 is absent.
    // Swizzle in this same draw, not by allocating/converting the full atlas.
    rgb = mix(rgb, rgb.bgr, ubuf.options.y);
    fragColor = vec4(rgb * ubuf.qt_Opacity, ubuf.qt_Opacity);
}
