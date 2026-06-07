// rect_overlay.vert -- full-screen triangle for the crop-screenshot overlay.
//
// Same fullscreen-triangle trick as palette.vert; kept as its own file so each
// pipeline binds shaders compiled with matching set/binding layouts and the
// build script can emit them as independent symbols. v_uv.y is negated for the
// same SDL_GPU Y-up-NDC / Y-down-texcoord reason documented in palette.vert.glsl
// (was upside-down on Vulkan when it relied on the Metal-only --flip-vert-y).
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
