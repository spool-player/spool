#version 440
precision highp float;
layout(location = 0) in highp vec2 texCoord;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec4 options;
    vec4 dimensions;
    vec4 cropBounds;
    vec4 chroma;
} ubuf;
layout(binding = 1) uniform sampler2D rgbTexture;
vec3 sourceColor(vec2 pixel)
{
    vec3 color = texture(rgbTexture, pixel * ubuf.dimensions.zw).rgb;
    return mix(color, color.bgr, ubuf.options.y);
}

// Exact normalized windowed sinc(x)*sinc(x/2), radius 2. Opposite-sign
// outer lobes cannot be bilinear-paired. Only the positive middle pair is
// combined: three samples per axis, nine per quad fragment.
vec4 lanczosWeights(float f)
{
    if (f < 0.00001)
        return vec4(0.0, 1.0, 0.0, 0.0);
    if (f > 0.99999)
        return vec4(0.0, 0.0, 1.0, 0.0);
    // The common sin(pi*f) factor cancels during normalization.
    float sh = sin(1.570796326794897 * f);
    float ch = cos(1.570796326794897 * f);
    vec4 d = vec4(f + 1.0, f, 1.0 - f, 2.0 - f);
    vec4 w = vec4(-ch, sh, ch, -sh) / (d * d);
    return w / dot(w, vec4(1.0));
}
void main()
{
    vec2 p = texCoord * ubuf.dimensions.xy - vec2(0.5);
    vec2 base = floor(p) + vec2(0.5);
    vec4 wx = lanczosWeights(fract(p.x));
    vec4 wy = lanczosWeights(fract(p.y));
    vec3 ox = vec3(-1.0, wx.z / (wx.y + wx.z), 2.0);
    vec3 oy = vec3(-1.0, wy.z / (wy.y + wy.z), 2.0);
    vec3 ax = vec3(wx.x, wx.y + wx.z, wx.w);
    vec3 ay = vec3(wy.x, wy.y + wy.z, wy.w);
    vec3 sum = vec3(0.0);
    vec3 low = vec3(1.0), high = vec3(0.0);
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 3; ++x) {
            // Clamp each source tap, not just the quad UV, to the selected
            // tile's pixel centers. Filtering never samples adjacent tiles.
            vec2 pixel = clamp(base + vec2(ox[x], oy[y]), ubuf.cropBounds.xy, ubuf.cropBounds.zw);
            vec3 color = sourceColor(pixel);
            sum += color * (ax[x] * ay[y]);
            low = min(low, color);
            high = max(high, color);
        }
    }
    // Neighborhood envelope limits overshoot from Lanczos's negative lobes.
    vec3 color = clamp(sum, low, high);
    fragColor = vec4(clamp(color, vec3(0.0), vec3(1.0)) * ubuf.qt_Opacity, ubuf.qt_Opacity);
}
