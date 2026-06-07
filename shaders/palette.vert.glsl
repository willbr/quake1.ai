// palette.vert -- full-screen triangle vertex shader.
//
// Three-vertex triangle covering the clip-space quad (-1,-1)..(1,1); the
// hardware rasterizes only the screen-covering region. No vertex buffer
// required — gl_VertexIndex drives the positions.
//
// SDL_GPU normalized device coords are Y-UP (lower-left = (-1,-1)) but texture
// coords are Y-DOWN (top-left = (0,0)) — see SDL_gpu.h. The framebuffer texture
// stores row 0 at the image top, so the screen TOP (clip y=+1) must map to
// v_uv.y=0. Hence v_uv.y is the *negated* half-mapping of pos.y. (Earlier this
// shader emitted `pos*0.5+0.5` and leaned on a Metal-only `spirv-cross
// --flip-vert-y`, which left the Vulkan path upside-down; the flip is now baked
// into the source so both backends render upright with no per-backend flip.)
#version 450

layout(location = 0) out vec2 v_uv;

void main() {
    vec2 pos = vec2(
        (gl_VertexIndex == 1) ?  3.0 : -1.0,
        (gl_VertexIndex == 2) ?  3.0 : -1.0
    );
    gl_Position = vec4(pos, 0.0, 1.0);
    v_uv        = vec2(pos.x * 0.5 + 0.5, 0.5 - pos.y * 0.5);
}
