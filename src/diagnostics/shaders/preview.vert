#version 440
precision highp float;
layout(location = 0) in vec4 aVertex;
layout(location = 1) in highp vec2 aTexCoord;
layout(location = 0) out highp vec2 texCoord;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec4 options;
    vec4 yBounds;
    vec4 chromaBounds;
    vec4 chromaScale;
} ubuf;
out gl_PerVertex { vec4 gl_Position; };
void main()
{
    // Qt supplies the backend-correct projection (including framebuffer Y flip).
    gl_Position = ubuf.qt_Matrix * aVertex;
    texCoord = aTexCoord;
}
