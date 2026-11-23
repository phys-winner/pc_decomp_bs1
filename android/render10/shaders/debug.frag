#version 450
// Diagnostics (BIOSHOCK_R10_DEBUG bit 64): replaces every pixel shader, so a draw that rasterizes
// shows magenta whatever its own shader computes.
layout(location = 0) out vec4 color;

void main()
{
    color = vec4(1.0, 0.0, 1.0, 1.0);
}
