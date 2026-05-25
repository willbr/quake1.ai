// rect_overlay.frag -- crop-screenshot selection overlay.
//
// One fullscreen pass that, given a rect (in viewport-local swapchain pixels):
//   - draws a white 1-logical-pixel border around the rect,
//   - leaves the interior untouched (discard so the palette draw shows through),
//   - dims everything outside the rect via standard alpha blending.
//
// Bound with src=SRC_ALPHA, dst=ONE_MINUS_SRC_ALPHA so:
//   outside: out=(0,0,0,a) -> dst' = dst*(1-a)  (a≈0.5 => 50% dim)
//   border:  out=(1,1,1,1) -> dst' = white
#version 450

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 frag_color;

layout(set = 3, binding = 0) uniform RectParams {
    // rect: (x0, y0, x1_exclusive, y1_exclusive) in viewport-local pixels.
    // viewport: (origin_x, origin_y, w, h) in swapchain pixels.
    vec4  rect;
    vec4  viewport;
    float border_width;   // swapchain pixels (≈ int_scale)
    float dim_alpha;      // 0..1; 0 means no dim
    float pad0;
    float pad1;
} u;

void main() {
    vec2  vp = gl_FragCoord.xy - u.viewport.xy;
    float x0 = u.rect.x, y0 = u.rect.y;
    float x1 = u.rect.z, y1 = u.rect.w;
    float bw = max(u.border_width, 1.0);

    bool inside = (vp.x >= x0 && vp.x < x1 && vp.y >= y0 && vp.y < y1);
    bool border = inside && (vp.x < x0 + bw || vp.x >= x1 - bw ||
                             vp.y < y0 + bw || vp.y >= y1 - bw);

    if (border) {
        frag_color = vec4(1.0, 1.0, 1.0, 1.0);
    } else if (inside) {
        discard;
    } else {
        frag_color = vec4(0.0, 0.0, 0.0, u.dim_alpha);
    }
}
