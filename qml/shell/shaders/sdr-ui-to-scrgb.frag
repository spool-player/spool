#version 440

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float whiteScale;
};
layout(binding = 1) uniform sampler2D source;

float srgbToLinear(float value)
{
    return value <= 0.04045 ? value / 12.92
                           : pow((value + 0.055) / 1.055, 2.4);
}

void main()
{
    vec4 sampleColor = texture(source, qt_TexCoord0);
    if (sampleColor.a <= 0.0) {
        fragColor = vec4(0.0);
        return;
    }

    // Qt's layer texture contains premultiplied sRGB. Decode straight color,
    // not coverage: decoding premultiplied channels darkens translucent edges.
    vec3 srgb = sampleColor.rgb / sampleColor.a;
    vec3 linear = vec3(srgbToLinear(srgb.r), srgbToLinear(srgb.g), srgbToLinear(srgb.b));
    // sRGB and scRGB share BT.709 primaries. Scale RGB only, then premultiply
    // again for Qt's source-over blend into the floating-point window target.
    fragColor = vec4(linear * whiteScale * sampleColor.a, sampleColor.a) * qt_Opacity;
}
