// palette.frag -- 8-bit indexed → 32-bit RGBA expansion on the GPU,
// with an optional CRT-style scanline dim aligned to swapchain pixel rows.
//
// Reads two R8 textures (the Quake framebuffer of palette indices and the
// per-pixel palette-slot map for Doom/Wolf3D weapon overlays) plus a
// 256×N_PALETTES RGBA8 LUT, and produces an RGBA8 output. Replaces the
// CPU-side `palette_expand` loop in vid_sdl.c.
//
// Binding sets follow SDL_GPU conventions: fragment sampler bindings live
// in set 2 with sequential binding indices in declaration order; fragment
// uniform buffers live in set 3.
#version 450

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 frag_color;

layout(set = 2, binding = 0) uniform usampler2D u_framebuffer; // R8 indices
layout(set = 2, binding = 1) uniform usampler2D u_palette_id;  // R8 palette slot
layout(set = 2, binding = 2) uniform sampler2D  u_palette;     // RGBA8 256×N

layout(set = 3, binding = 0) uniform Params {
    // intensity: 0 disables the overlay; >0 darkens the "dark" band by that
    // fraction (1 = pure black between bands). size: physical-pixel band
    // height (>=1). pad0/pad1: keep std140 layout at 16 bytes.
    float scanline_intensity;
    float scanline_size;
    float pad0;
    float pad1;
} u_params;

void main() {
    ivec2 fb_size = textureSize(u_framebuffer, 0);
    ivec2 px      = ivec2(v_uv * vec2(fb_size));
    uint  idx     = texelFetch(u_framebuffer, px, 0).r;
    uint  slot    = texelFetch(u_palette_id,  px, 0).r;
    vec4  color   = texelFetch(u_palette, ivec2(int(idx), int(slot)), 0);

    if (u_params.scanline_intensity > 0.0) {
        // gl_FragCoord is in swapchain pixels regardless of viewport offset,
        // so bands stay locked to the physical pixel grid — same behaviour as
        // the prior CPU overlay.
        int row  = int(gl_FragCoord.y);
        int size = int(max(u_params.scanline_size, 1.0));
        int band = (row / size) & 1;     // 0 = dark band, 1 = bright band
        if (band == 0) {
            color.rgb *= (1.0 - u_params.scanline_intensity);
        }
    }
    frag_color = color;
}
