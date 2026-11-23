#version 450
/* Present pass of the Vulkan backend: the back buffer as a letterboxed quad, rotated for the
 * swap chain's pre-transform (Android's pre-rotation). */
layout(location = 0) out vec2 uv;
layout(push_constant) uniform Present {
    vec4 rotation;      /* 2x2 matrix, rows (x, y) and (z, w) */
    vec4 placement;     /* scale x, scale y, offset x, offset y in clip space */
} pc;
void main() {
    vec2 p = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    uv = p;
    vec2 c = (p * 2.0 - 1.0) * pc.placement.xy + pc.placement.zw;
    gl_Position = vec4(c.x * pc.rotation.x + c.y * pc.rotation.y, c.x * pc.rotation.z + c.y * pc.rotation.w, 0.0, 1.0);
}
