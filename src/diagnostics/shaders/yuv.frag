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
layout(binding = 1) uniform sampler2D yTexture;
layout(binding = 2) uniform sampler2D uTexture;
layout(binding = 3) uniform sampler2D vTexture;
float planeValue(vec4 sampleValue)
{
    // RED_OR_ALPHA8 remains an actual one-byte texture on GLES2 devices.
    return mix(sampleValue.r, sampleValue.a, ubuf.options.x);
}
void main()
{
    vec2 l = clamp(texCoord, ubuf.yBounds.xy, ubuf.yBounds.zw);
    float y = planeValue(texture(yTexture, l));
    // JPEG 4:2:0 chroma centers lie halfway between paired luma centers.
    // Scaling only matters for odd visible dimensions; padded MCU rows never
    // enter the textures. Each plane uses hardware bilinear interpolation.
    vec2 c = clamp(l * ubuf.chromaScale.xy,
                   ubuf.chromaBounds.xy, ubuf.chromaBounds.zw);
    float cb = planeValue(texture(uTexture, c)) - (128.0 / 255.0);
    float cr = planeValue(texture(vTexture, c)) - (128.0 / 255.0);
    vec3 rgb = vec3(y + 1.402 * cr,
                    y - 0.344136286 * cb - 0.714136286 * cr,
                    y + 1.772 * cb);
    rgb = clamp(rgb, vec3(0.0), vec3(1.0));
    fragColor = vec4(rgb * ubuf.qt_Opacity, ubuf.qt_Opacity);
}
