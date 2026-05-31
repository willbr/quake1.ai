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
    // height (>=1). supersample: ss factor (1..4) — average each ss×ss block
    // of expanded texels down to one render-res texel (true SSAA). pad1: keep
    // std140 layout at 16 bytes.
    float scanline_intensity;
    float scanline_size;
    float supersample;
    float pad1;
} u_params;

void main() {
    // The framebuffer is the *super* buffer (render_size * ss). Supersample by
    // averaging each ss×ss block of palette-EXPANDED RGB texels down to one
    // render-resolution texel — true SSAA. The expansion must happen BEFORE the
    // average: palette indices are non-linear, so averaging raw indices is
    // meaningless (and the R8_UINT texture can't be hardware-filtered anyway).
    // With ss=1 the block is 1×1, i.e. a single point fetch — byte-identical to
    // the pre-SSAA path.
    int   ss      = clamp(int(u_params.supersample + 0.5), 1, 4);
    ivec2 fb_size = textureSize(u_framebuffer, 0);     // super dims (render*ss)
    ivec2 rsize   = fb_size / ss;                       // render dims (exact)
    ivec2 rpx     = clamp(ivec2(v_uv * vec2(rsize)), ivec2(0), rsize - 1);
    ivec2 base    = rpx * ss;                           // top-left super texel

    vec4 acc = vec4(0.0);
    for (int j = 0; j < 4; ++j) {
        if (j >= ss) break;
        for (int i = 0; i < 4; ++i) {
            if (i >= ss) break;
            ivec2 p    = base + ivec2(i, j);
            uint  idx  = texelFetch(u_framebuffer, p, 0).r;
            uint  slot = texelFetch(u_palette_id,  p, 0).r;
            acc += texelFetch(u_palette, ivec2(int(idx), int(slot)), 0);
        }
    }
    vec4 color = acc / float(ss * ss);

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
