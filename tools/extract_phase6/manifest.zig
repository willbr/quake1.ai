// manifest.zig -- Declarative table of Phase 6 extraction targets + driver.
//
// 6 Doom weapons × N frames each = 6 .spr files.
// 7 Doom sounds = 7 .wav files.

const std = @import("std");
const Io = std.Io;
const Dir = std.Io.Dir;
const Allocator = std.mem.Allocator;

const palette_png = @import("quake_palette_png.zig");
const doom_wad    = @import("doom_wad.zig");
const quake_spr   = @import("quake_spr.zig");
const quake_wav   = @import("quake_wav.zig");

// ---------------------------------------------------------------------------
// Sprite manifest entries.
// ---------------------------------------------------------------------------

// A Doom viewmodel frame is either a bare gun lump, or a gun lump with a
// flash lump composited on top (positioned via Doom's per-sprite leftoffset
// / topoffset so the flash sits over the muzzle).
const DoomFrameSpec = struct {
    lump:  []const u8,
    flash: ?[]const u8 = null,
};

const DoomSpriteSet = struct {
    out_path: []const u8,
    frames: []const DoomFrameSpec,
};

// Frame manifests confirmed by enumerating DOOM1.WAD via -doom_info.
// Flash overlays follow Doom's p_pspr.c firing actions:
//   - A_FirePistol   sets ps_flash = S_PISTOLFLASH (PISFA0, 7 tics, fullbright).
//   - A_FireShotgun  sets ps_flash = S_SGUNFLASH1 (SHTFA0) → S_SGUNFLASH2 (SHTFB0).
//   - A_FireCGun     alternates CHGFA0/CHGFB0 each shot (we use CHGFA0 on first frame).
//   - A_FireMissile  sets ps_flash = S_MISSILEFLASH1..4 (MISFA..MISFD).
//   - Punch/Saw      no flash.
// We composite the flash onto the frame whose Doom state visually overlaps it.
const doom_pung_frames  = [_]DoomFrameSpec{
    .{ .lump = "PUNGA0" },
    .{ .lump = "PUNGB0" },
    .{ .lump = "PUNGC0" },
    .{ .lump = "PUNGD0" },
};
const doom_pisg_frames  = [_]DoomFrameSpec{
    .{ .lump = "PISGA0" },                       // 0: idle / pre-fire hold
    .{ .lump = "PISGB0", .flash = "PISFA0" },    // 1: fire (composited PISFA0 flash)
    .{ .lump = "PISGC0" },                       // 2: smoke
    .{ .lump = "PISGB0" },                       // 3: recoil settle (no flash)
};
const doom_shtg_frames  = [_]DoomFrameSpec{
    .{ .lump = "SHTGA0" },                       // 0: idle / pre-fire    -- S_SGUN1, S_SGUN8 (no flash) — also weaponframe=0 idle
    .{ .lump = "SHTGA0", .flash = "SHTFA0" },    // 1: fire (with flash)  -- S_SGUN2 fires here
    .{ .lump = "SHTGB0" },                       // 2: pump open          -- S_SGUN3, S_SGUN7
    .{ .lump = "SHTGC0" },                       // 3: pump back          -- S_SGUN4, S_SGUN6
    .{ .lump = "SHTGD0" },                       // 4: pump full back     -- S_SGUN5
};
const doom_chgg_frames  = [_]DoomFrameSpec{
    .{ .lump = "CHGGA0" },                       // 0: idle (no flash) — weaponframe=0 idle pose
    .{ .lump = "CHGGA0", .flash = "CHGFA0" },    // 1: fire pose A with flash A -- S_CHAIN1
    .{ .lump = "CHGGB0", .flash = "CHGFB0" },    // 2: fire pose B with flash B -- S_CHAIN2
};
const doom_misg_frames  = [_]DoomFrameSpec{
    .{ .lump = "MISGA0" },                         // 0: idle / lowered pose
    .{ .lump = "MISGB0", .flash = "MISFA0" },      // 1: fire (flash composited on launcher pose)
    .{ .lump = "MISGB0" },                         // 2: settle (no flash)
};
const doom_sawg_frames  = [_]DoomFrameSpec{
    .{ .lump = "SAWGA0" },
    .{ .lump = "SAWGB0" },
    .{ .lump = "SAWGC0" },
    .{ .lump = "SAWGD0" },
};

const doom_sprite_sets = [_]DoomSpriteSet{
    .{ .out_path = "id1/progs/v_doomfist.spr",     .frames = &doom_pung_frames },
    .{ .out_path = "id1/progs/v_doompistol.spr",   .frames = &doom_pisg_frames },
    .{ .out_path = "id1/progs/v_doomshotgun.spr",  .frames = &doom_shtg_frames },
    .{ .out_path = "id1/progs/v_doomchaingun.spr", .frames = &doom_chgg_frames },
    .{ .out_path = "id1/progs/v_doomrocket.spr",   .frames = &doom_misg_frames },
    .{ .out_path = "id1/progs/v_doomchainsaw.spr", .frames = &doom_sawg_frames },
};

// ---------------------------------------------------------------------------
// Sound manifest entries.
// ---------------------------------------------------------------------------
const DoomSoundSet = struct {
    out_path: []const u8,
    lump:     []const u8,
};

const doom_sound_sets = [_]DoomSoundSet{
    .{ .out_path = "id1/sound/phase6/doom_pistol.wav",   .lump = "DSPISTOL"  },
    .{ .out_path = "id1/sound/phase6/doom_shotgn.wav",   .lump = "DSSHOTGN"  },
    .{ .out_path = "id1/sound/phase6/doom_rlaunch.wav",  .lump = "DSRLAUNC"  },
    .{ .out_path = "id1/sound/phase6/doom_punch.wav",    .lump = "DSPUNCH"   },
    .{ .out_path = "id1/sound/phase6/doom_sawhit.wav",   .lump = "DSSAWHIT"  },
    .{ .out_path = "id1/sound/phase6/doom_sawful.wav",   .lump = "DSSAWFUL"  },
    .{ .out_path = "id1/sound/phase6/doom_sawidl.wav",   .lump = "DSSAWIDL"  },
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Linear-resample 8-bit unsigned PCM from `src_rate` to `dst_rate`. Used by
/// the Doom sound path for any lump that isn't already 11025 Hz.
fn resampleU8(allocator: Allocator, in: []const u8, src_rate: u32, dst_rate: u32) ![]u8 {
    if (src_rate == dst_rate) return allocator.dupe(u8, in);
    const out_len: usize = (in.len * dst_rate) / src_rate;
    var out = try allocator.alloc(u8, out_len);
    errdefer allocator.free(out);

    var i: usize = 0;
    while (i < out_len) : (i += 1) {
        const src_pos_q16: u64 = (@as(u64, i) * @as(u64, src_rate) * 65536) / @as(u64, dst_rate);
        const src_idx: usize = @intCast(src_pos_q16 >> 16);
        const frac: u32 = @intCast(src_pos_q16 & 0xFFFF);
        if (src_idx + 1 >= in.len) {
            out[i] = in[in.len - 1];
        } else {
            const a: u32 = in[src_idx];
            const b: u32 = in[src_idx + 1];
            const interp = (a * (65536 - frac) + b * frac) >> 16;
            out[i] = @intCast(interp & 0xFF);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Doom sprite decode + flash composite helpers
// ---------------------------------------------------------------------------

const DoomPic = struct {
    width:       u16,
    height:      u16,
    left_offset: i16,
    top_offset:  i16,
    pixels:      []u8, // heap-allocated, caller frees
};

fn decodeDoomLump(allocator: Allocator, wad: *doom_wad.Wad, name: []const u8) !DoomPic {
    const l = wad.findLump(name) orelse {
        std.debug.print("  WARNING: Doom lump '{s}' missing in WAD\n", .{name});
        return error.LumpMissing;
    };
    const data = wad.lumpData(l);
    if (data.len < 4) return error.PicHeaderTooSmall;
    const wpx = std.mem.readInt(u16, data[0..2], .little);
    const hpx = std.mem.readInt(u16, data[2..4], .little);
    const out = try allocator.alloc(u8, @as(usize, wpx) * @as(usize, hpx));
    errdefer allocator.free(out);
    const sz = try doom_wad.decodePicture(data, out);
    return .{
        .width = sz.width, .height = sz.height,
        .left_offset = sz.left_offset, .top_offset = sz.top_offset,
        .pixels = out,
    };
}

const Composed = struct {
    width:  u16,
    height: u16,
    // Effective Doom-style offsets for the composited canvas. Computed so that
    // the gun's anchor in screen space stays at exactly where it would have
    // been if rendered alone (the flash is just decoration above the gun).
    left_offset: i32,
    top_offset:  i32,
    pixels: []u8,
};

/// Composite a Doom flash sprite over a gun sprite, aligning their per-frame
/// anchors (Doom's leftoffset / topoffset). Doom positions every weapon sprite
/// so its anchor lands at the same screen point; matching them keeps the flash
/// over the muzzle. The output canvas keeps the gun horizontally centered (so
/// bottom-center anchoring on the screen stays stable across frames) and grows
/// upward / sideways as needed to contain the flash.
///
/// All pixels equal to 0xFF are transparent in both input and output.
fn compositeFlash(allocator: Allocator, gun: DoomPic, flash: DoomPic) !Composed {
    // Gun anchor at (gun.left_offset, gun.top_offset) within gun pixels;
    // same anchor for flash. Place gun at canvas (gx, gy) and flash at
    // (fx, fy) so anchors coincide. Pick axes so the gun is bottom-centered.
    const dx: i32 = @as(i32, gun.left_offset) - @as(i32, flash.left_offset);
    const dy: i32 = @as(i32, gun.top_offset)  - @as(i32, flash.top_offset);

    // Horizontal: keep gun centered. We expand the canvas symmetrically by
    // whichever side the flash overflows further.
    const gw: i32 = @intCast(gun.width);
    const gh: i32 = @intCast(gun.height);
    const fw: i32 = @intCast(flash.width);
    const fh: i32 = @intCast(flash.height);

    // If we placed gun at gx=0, the flash would be at fx = dx, occupying
    // dx..dx+fw. Compute pad needed on each side of the gun:
    const pad_left:  i32 = @max(0, -dx);
    const pad_right: i32 = @max(0, (dx + fw) - gw);
    const pad_each:  i32 = @max(pad_left, pad_right);
    const canvas_w:  i32 = gw + 2 * pad_each;
    const gx: i32 = pad_each;
    const fx: i32 = gx + dx;

    // Vertical: gun stays at the bottom; canvas grows upward if flash sits
    // above the gun (typical case — dy < 0 for muzzle flashes).
    // If flash extends below gun (dy + fh > gh), pad bottom too — but the
    // gun's bottom-edge will no longer be the canvas-bottom, so we drop the
    // overflow instead (rare for muzzle flashes).
    const flash_top_above: i32 = @max(0, -dy);
    const canvas_h: i32 = gh + flash_top_above;
    const gy: i32 = flash_top_above;
    const fy: i32 = gy + dy;

    // Defensive: anything that would still spill outside the canvas, we skip
    // per-pixel rather than expand further.
    const total: usize = @intCast(canvas_w * canvas_h);
    const canvas = try allocator.alloc(u8, total);
    @memset(canvas, 0xFF);

    blitPixels(canvas, canvas_w, canvas_h, gun.pixels, gw, gh, gx, gy);
    blitPixels(canvas, canvas_w, canvas_h, flash.pixels, fw, fh, fx, fy);

    // The gun was placed at (gx, gy) within the canvas. To keep the gun's
    // screen-space anchor identical to bare-gun rendering, shift the
    // canvas-effective offsets by gx/gy: gun_anchor_in_canvas =
    // (gx + gun.left_offset, gy + gun.top_offset).
    return .{
        .width       = @intCast(canvas_w),
        .height      = @intCast(canvas_h),
        .left_offset = @as(i32, gun.left_offset) + gx,
        .top_offset  = @as(i32, gun.top_offset)  + gy,
        .pixels      = canvas,
    };
}

fn blitPixels(
    dst: []u8, dst_w: i32, dst_h: i32,
    src: []const u8, src_w: i32, src_h: i32,
    dx: i32, dy: i32,
) void {
    var y: i32 = 0;
    while (y < src_h) : (y += 1) {
        const cy = dy + y;
        if (cy < 0 or cy >= dst_h) continue;
        var x: i32 = 0;
        while (x < src_w) : (x += 1) {
            const cx = dx + x;
            if (cx < 0 or cx >= dst_w) continue;
            const px = src[@intCast(y * src_w + x)];
            if (px == 0xFF) continue;
            dst[@intCast(cy * dst_w + cx)] = px;
        }
    }
}

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------

/// Stat every output file the extractor would produce. Returns true iff
/// they all already exist on disk — extractor inputs (DOOM1.WAD, pak0.pak)
/// are committed reference data that doesn't change in practice, so existence
/// alone is a sufficient freshness check. `rm` any output to force
/// re-extraction.
fn allOutputsExist(io: Io) bool {
    for (doom_sprite_sets) |s| if (!pathExists(io, s.out_path)) return false;
    for (doom_sound_sets)  |s| if (!pathExists(io, s.out_path)) return false;
    if (!pathExists(io, "id1/gfx/palette_doom.lmp")) return false;
    if (!pathExists(io, "id1/gfx/palette.lmp")) return false;
    return true;
}

fn pathExists(io: Io, path: []const u8) bool {
    if (Dir.cwd().openFile(io, path, .{})) |f| {
        f.close(io);
        return true;
    } else |_| return false;
}

pub fn extractAll(io: Io, allocator: Allocator) !void {
    // Regenerate id1/gfx/palette.lmp from tools/palette.png if the .lmp is
    // gone. The PNG is the human-readable source of truth; the engine still
    // reads the binary .lmp at runtime. This runs before allOutputsExist so a
    // missing palette.lmp doesn't shortcut the rest of the extractor too.
    try palette_png.regenerateLmpIfMissing(io, allocator);

    if (allOutputsExist(io)) return;
    std.debug.print("extracting Phase 6 assets...\n", .{});

    // ----- Doom sources -----
    var w = try doom_wad.Wad.open(io, allocator, "ref/doom-data/DOOM1.WAD");
    defer w.deinit(allocator);
    const doom_pal = try w.loadPlaypal0();

    // Phase 6 palette switching: write Doom's PLAYPAL palette 0 as a
    // 768-byte LMP that the engine loads at startup into vid_lut[1].
    {
        const out_path = "id1/gfx/palette_doom.lmp";
        if (std.fs.path.dirname(out_path)) |dir_path|
            try Dir.cwd().createDirPath(io, dir_path);
        var bytes: [768]u8 = undefined;
        var i: usize = 0;
        while (i < 256) : (i += 1) {
            bytes[i * 3 + 0] = doom_pal[i][0];
            bytes[i * 3 + 1] = doom_pal[i][1];
            bytes[i * 3 + 2] = doom_pal[i][2];
        }
        const f = try Dir.cwd().createFile(io, out_path, .{});
        defer f.close(io);
        try f.writePositionalAll(io, &bytes, 0);
        std.debug.print("  wrote {s} (768 bytes)\n", .{out_path});
    }

    // Doom-authentic anchoring: emit each Doom sprite at its native size and
    // pass through the WAD's leftoffset / topoffset values via SPR's
    // origin_x / origin_y. R_DrawViewModelSprite then anchors the sprite
    // using Doom's WEAPONTOP+topoffset math so the gun's bottom extends past
    // vrect bottom (clipped, like Doom's sbar overdraws). No downscale.

    for (doom_sprite_sets) |set| {
        // Doom sprites vary in size — allocate per frame.
        var frames = try allocator.alloc(quake_spr.Frame, set.frames.len);
        defer allocator.free(frames);
        var pixel_bufs = try allocator.alloc([]u8, set.frames.len);
        // Mark slots empty so partial-init cleanup is safe.
        for (pixel_bufs) |*pb| pb.* = &.{};
        defer {
            for (pixel_bufs) |p| if (p.len > 0) allocator.free(p);
            allocator.free(pixel_bufs);
        }

        var i: usize = 0;
        while (i < set.frames.len) : (i += 1) {
            const spec = set.frames[i];
            const gun = try decodeDoomLump(allocator, &w, spec.lump);

            // Build either bare gun or gun + composited flash. Pass through
            // Doom-style offsets so the renderer can use WEAPONTOP+topoffset.
            var raw_w: u32 = 0;
            var raw_h: u32 = 0;
            var raw_pixels: []u8 = &.{};
            var raw_left_off: i32 = 0;
            var raw_top_off: i32 = 0;
            if (spec.flash) |flash_name| {
                if (w.findLump(flash_name)) |_| {
                    const flash = try decodeDoomLump(allocator, &w, flash_name);
                    const composed = try compositeFlash(allocator, gun, flash);
                    allocator.free(gun.pixels);
                    allocator.free(flash.pixels);
                    raw_w = composed.width;
                    raw_h = composed.height;
                    raw_pixels = composed.pixels;
                    raw_left_off = composed.left_offset;
                    raw_top_off = composed.top_offset;
                } else {
                    std.debug.print("  WARNING: Doom flash lump '{s}' missing — skipping flash on {s}\n",
                        .{ flash_name, spec.lump });
                    raw_w = gun.width;
                    raw_h = gun.height;
                    raw_pixels = gun.pixels;
                    raw_left_off = gun.left_offset;
                    raw_top_off = gun.top_offset;
                }
            } else {
                raw_w = gun.width;
                raw_h = gun.height;
                raw_pixels = gun.pixels;
                raw_left_off = gun.left_offset;
                raw_top_off = gun.top_offset;
            }

            pixel_bufs[i] = raw_pixels;
            frames[i] = .{
                .width    = raw_w,
                .height   = raw_h,
                .pixels   = raw_pixels,
                .origin_x = raw_left_off,
                .origin_y = raw_top_off,
            };
        }
        try quake_spr.writeSprite(io, allocator, set.out_path, frames);
        std.debug.print("  wrote {s} ({d} frames)\n", .{ set.out_path, set.frames.len });
    }

    for (doom_sound_sets) |set| {
        const l = w.findLump(set.lump) orelse {
            std.debug.print("  WARNING: Doom sound '{s}' missing\n", .{set.lump});
            return error.LumpMissing;
        };
        const sb = try doom_wad.parseSound(w.lumpData(l));
        // Doom DS* sounds are already 11025 Hz typically; resample only if needed.
        if (sb.rate == 11025) {
            try quake_wav.writeWavU8(io, allocator, set.out_path, 11025, sb.pcm);
        } else {
            const resampled = try resampleU8(allocator, sb.pcm, sb.rate, 11025);
            defer allocator.free(resampled);
            try quake_wav.writeWavU8(io, allocator, set.out_path, 11025, resampled);
        }
        std.debug.print("  wrote {s} ({d} samples @ {d}->11025 Hz)\n", .{ set.out_path, sb.pcm.len, sb.rate });
    }
}
