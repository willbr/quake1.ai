// palette.frag -- 8-bit indexed → 32-bit RGBA expansion on the GPU.
//
// Reads two R8 textures (the Quake framebuffer of palette indices and the
// per-pixel palette-slot map for Doom/Wolf3D weapon overlays) plus a
// 256×N_PALETTES RGBA8 LUT, and produces an RGBA8 output. Replaces the
// CPU-side `palette_expand` loop in vid_sdl.c.
//
// Binding sets follow SDL_GPU conventions: fragment sampler bindings live
// in set 2 with sequential binding indices in declaration order.
#version 450

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 frag_color;

layout(set = 2, binding = 0) uniform usampler2D u_framebuffer; // R8 indices
layout(set = 2, binding = 1) uniform usampler2D u_palette_id;  // R8 palette slot
layout(set = 2, binding = 2) uniform sampler2D  u_palette;     // RGBA8 256×N

void main() {
    ivec2 fb_size = textureSize(u_framebuffer, 0);
    ivec2 px      = ivec2(v_uv * vec2(fb_size));
    uint  idx     = texelFetch(u_framebuffer, px, 0).r;
    uint  slot    = texelFetch(u_palette_id,  px, 0).r;
    frag_color    = texelFetch(u_palette, ivec2(int(idx), int(slot)), 0);
}
