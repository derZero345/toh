#version 450

// Everything is drawn in logical window coordinates (origin top-left, y down).
layout(push_constant) uniform PushConstants {
    vec2 screen;
} pc;

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;

layout(location = 0) out vec2 outUV;
layout(location = 1) out vec4 outColor;

void main() {
    outUV    = inUV;
    outColor = inColor;
    gl_Position = vec4(inPos / pc.screen * 2.0 - 1.0, 0.0, 1.0);
}
