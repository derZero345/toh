#version 450

// Solid shapes sample a 1x1 white texture, text samples the glyph atlas
// (an R8 image swizzled to (1,1,1,r)), images sample their RGBA texture.
layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 inUV;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = inColor * texture(tex, inUV);
}
