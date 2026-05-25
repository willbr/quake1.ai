// palette.vert -- full-screen triangle vertex shader.
//
// Three-vertex triangle covering the clip-space quad (-1,-1)..(1,1); the
// hardware rasterizes only the screen-covering region. No vertex buffer
// required — gl_VertexIndex drives the positions.
//
// SDL_GPU follows Vulkan clip-space conventions (Y down).
#version 450

layout(location = 0) out vec2 v_uv;

void main() {
    vec2 pos = vec2(
        (gl_VertexIndex == 1) ?  3.0 : -1.0,
        (gl_VertexIndex == 2) ?  3.0 : -1.0
    );
    gl_Position = vec4(pos, 0.0, 1.0);
    v_uv        = pos * 0.5 + 0.5;
}
