// manifest.zig -- Declarative table of Phase 6 extraction targets + driver.
//
// 10 weapons × N frames each = 10 .spr files.
// 9 sounds (3 Wolf + 6 Doom) = 9 .wav files.
// Wolf knife has no digital sound in shareware (Adlib-only) — silent for now.

const std = @import("std");
const Io = std.Io;
const Allocator = std.mem.Allocator;

const palette_mod = @import("quake_palette.zig");
const wolf_vswap  = @import("wolf_vswap.zig");
const wolf_digi   = @import("wolf_digi.zig");
const doom_wad    = @import("doom_wad.zig");
const quake_spr   = @import("quake_spr.zig");
const quake_wav   = @import("quake_wav.zig");

// ---------------------------------------------------------------------------
// Sprite manifest entries.
// ---------------------------------------------------------------------------
const WolfSpriteSet = struct {
    out_path: []const u8,
    base_idx: u16,    // first sprite chunk in VSWAP
    frame_count: u8,  // sequential
};

const DoomSpriteSet = struct {
    out_path: []const u8,
    lumps: []const []const u8,  // ordered frames
};

const wolf_sprite_sets = [_]WolfSpriteSet{
    .{ .out_path = "id1/progs/v_wolfknife.spr",     .base_idx = 522, .frame_count = 5 },
    .{ .out_path = "id1/progs/v_wolfpistol.spr",    .base_idx = 527, .frame_count = 5 },
    .{ .out_path = "id1/progs/v_wolfmg.spr",        .base_idx = 532, .frame_count = 5 },
    .{ .out_path = "id1/progs/v_wolfchaingun.spr",  .base_idx = 537, .frame_count = 5 },
};

// Frame counts confirmed by enumerating DOOM1.WAD via -doom_info.
const doom_pung_lumps  = [_][]const u8{ "PUNGA0", "PUNGB0", "PUNGC0", "PUNGD0" };
const doom_pisg_lumps  = [_][]const u8{ "PISGA0", "PISGB0", "PISGC0", "PISGD0", "PISGE0" };
const doom_shtg_lumps  = [_][]const u8{ "SHTGA0", "SHTGB0", "SHTGC0", "SHTGD0" };
const doom_chgg_lumps  = [_][]const u8{ "CHGGA0", "CHGGB0" };
const doom_misg_lumps  = [_][]const u8{ "MISGA0", "MISGB0" };
const doom_sawg_lumps  = [_][]const u8{ "SAWGA0", "SAWGB0", "SAWGC0", "SAWGD0" };

const doom_sprite_sets = [_]DoomSpriteSet{
    .{ .out_path = "id1/progs/v_doomfist.spr",     .lumps = &doom_pung_lumps },
    .{ .out_path = "id1/progs/v_doompistol.spr",   .lumps = &doom_pisg_lumps },
    .{ .out_path = "id1/progs/v_doomshotgun.spr",  .lumps = &doom_shtg_lumps },
    .{ .out_path = "id1/progs/v_doomchaingun.spr", .lumps = &doom_chgg_lumps },
    .{ .out_path = "id1/progs/v_doomrocket.spr",   .lumps = &doom_misg_lumps },
    .{ .out_path = "id1/progs/v_doomchainsaw.spr", .lumps = &doom_sawg_lumps },
};

// ---------------------------------------------------------------------------
// Sound manifest entries.
// ---------------------------------------------------------------------------
const WolfSoundSet = struct {
    out_path: []const u8,
    digi_idx: u16,  // index into DigiList (per WL1 wolfdigimap)
};

const DoomSoundSet = struct {
    out_path: []const u8,
    lump:     []const u8,
};

const wolf_sound_sets = [_]WolfSoundSet{
    .{ .out_path = "id1/sound/phase6/wolf_pistol.wav",   .digi_idx = 5 },
    .{ .out_path = "id1/sound/phase6/wolf_mg.wav",       .digi_idx = 4 },
    .{ .out_path = "id1/sound/phase6/wolf_chaingun.wav", .digi_idx = 6 },
};

const doom_sound_sets = [_]DoomSoundSet{
    .{ .out_path = "id1/sound/phase6/doom_pistol.wav",   .lump = "DSPISTOL"  },
    .{ .out_path = "id1/sound/phase6/doom_shotgn.wav",   .lump = "DSSHOTGN"  },
    .{ .out_path = "id1/sound/phase6/doom_rlaunch.wav",  .lump = "DSRLAUNC"  },
    .{ .out_path = "id1/sound/phase6/doom_punch.wav",    .lump = "DSPUNCH"   },
    .{ .out_path = "id1/sound/phase6/doom_sawhit.wav",   .lump = "DSSAWHIT"  },
    .{ .out_path = "id1/sound/phase6/doom_sawful.wav",   .lump = "DSSAWFUL"  },
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Build a 256-entry remap from a source 8-bit palette to Quake palette
/// indices (0..254), via Euclidean-distance nearest match. Source index 0xFF
/// stays 0xFF (reserved transparency).
fn buildRemap8(src: [256][3]u8, dst: palette_mod.Palette) [256]u8 {
    var rmap: [256]u8 = undefined;
    var i: usize = 0;
    while (i < 256) : (i += 1) {
        if (i == 0xFF) { rmap[i] = 0xFF; continue; }
        const r: i32 = @as(i32, src[i][0]);
        const g: i32 = @as(i32, src[i][1]);
        const b: i32 = @as(i32, src[i][2]);
        var best_idx: u8 = 0;
        var best_d2: i64 = std.math.maxInt(i64);
        var k: u16 = 0;
        while (k < 255) : (k += 1) {
            const dr: i64 = r - @as(i32, dst[k][0]);
            const dg: i64 = g - @as(i32, dst[k][1]);
            const db: i64 = b - @as(i32, dst[k][2]);
            const d2 = dr * dr + dg * dg + db * db;
            if (d2 < best_d2) { best_d2 = d2; best_idx = @intCast(k); }
        }
        rmap[i] = best_idx;
    }
    return rmap;
}

fn applyRemap(rmap: [256]u8, pixels: []u8) void {
    for (pixels) |*p| p.* = rmap[p.*];
}

fn convertWolfPalette() [256][3]u8 {
    var out: [256][3]u8 = undefined;
    var i: usize = 0;
    while (i < 256) : (i += 1) {
        out[i][0] = wolf_vswap.channel6to8(wolf_vswap.wolf_palette_6bit[i * 3 + 0]);
        out[i][1] = wolf_vswap.channel6to8(wolf_vswap.wolf_palette_6bit[i * 3 + 1]);
        out[i][2] = wolf_vswap.channel6to8(wolf_vswap.wolf_palette_6bit[i * 3 + 2]);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------

pub fn extractAll(io: Io, allocator: Allocator) !void {
    const quake_pal = try palette_mod.loadPalette(io, allocator);

    // ----- Wolf sources -----
    var v = try wolf_vswap.VSwap.open(io, allocator, "wolf3d-data/VSWAP.WL1");
    defer v.deinit(allocator);

    const wolf_pal_8bit = convertWolfPalette();
    const wolf_remap = buildRemap8(wolf_pal_8bit, quake_pal);

    // Sprite output buffers (held one frame at a time). 64x64 fits all Wolf sprites.
    for (wolf_sprite_sets) |set| {
        var frames_storage: [16]quake_spr.Frame = undefined;
        var pixel_storage: [16][wolf_vswap.SPRITE_DIM * wolf_vswap.SPRITE_DIM]u8 = undefined;

        var fi: u8 = 0;
        while (fi < set.frame_count) : (fi += 1) {
            try v.decodeSprite(set.base_idx + fi, &pixel_storage[fi]);
            applyRemap(wolf_remap, &pixel_storage[fi]);
            frames_storage[fi] = .{
                .width    = wolf_vswap.SPRITE_DIM,
                .height   = wolf_vswap.SPRITE_DIM,
                .pixels   = &pixel_storage[fi],
                .origin_x = -@divTrunc(@as(i32, wolf_vswap.SPRITE_DIM), 2),
                .origin_y = wolf_vswap.SPRITE_DIM,
            };
        }
        try quake_spr.writeSprite(io, allocator, set.out_path, frames_storage[0..set.frame_count]);
        std.debug.print("  wrote {s} ({d} frames)\n", .{ set.out_path, set.frame_count });
    }

    for (wolf_sound_sets) |set| {
        const raw = try wolf_digi.extractDigiSound(allocator, &v, set.digi_idx);
        defer allocator.free(raw);
        const resampled = try wolf_digi.resampleU8(allocator, raw, wolf_digi.SourceRate, 11025);
        defer allocator.free(resampled);
        try quake_wav.writeWavU8(io, allocator, set.out_path, 11025, resampled);
        std.debug.print("  wrote {s} ({d} samples @ 11025 Hz)\n", .{ set.out_path, resampled.len });
    }

    // ----- Doom sources -----
    var w = try doom_wad.Wad.open(io, allocator, "doom-data/DOOM1.WAD");
    defer w.deinit(allocator);
    const doom_pal = try w.loadPlaypal0();
    const doom_remap = buildRemap8(doom_pal, quake_pal);

    for (doom_sprite_sets) |set| {
        // Doom sprites vary in size — allocate per frame.
        var frames = try allocator.alloc(quake_spr.Frame, set.lumps.len);
        defer allocator.free(frames);
        var pixel_bufs = try allocator.alloc([]u8, set.lumps.len);
        // Mark slots empty so partial-init cleanup is safe.
        for (pixel_bufs) |*pb| pb.* = &.{};
        defer {
            for (pixel_bufs) |p| if (p.len > 0) allocator.free(p);
            allocator.free(pixel_bufs);
        }

        var i: usize = 0;
        while (i < set.lumps.len) : (i += 1) {
            const l = w.findLump(set.lumps[i]) orelse {
                std.debug.print("  WARNING: Doom lump '{s}' missing in WAD\n", .{set.lumps[i]});
                return error.LumpMissing;
            };
            const data = w.lumpData(l);
            // Peek dimensions to size the output buffer.
            if (data.len < 4) return error.PicHeaderTooSmall;
            const wpx = std.mem.readInt(u16, data[0..2], .little);
            const hpx = std.mem.readInt(u16, data[2..4], .little);
            const out = try allocator.alloc(u8, @as(usize, wpx) * @as(usize, hpx));
            pixel_bufs[i] = out;
            const sz = try doom_wad.decodePicture(data, out);
            applyRemap(doom_remap, out);
            frames[i] = .{
                .width    = sz.width,
                .height   = sz.height,
                .pixels   = out,
                .origin_x = -@divTrunc(@as(i32, sz.width), 2),
                .origin_y = sz.height,
            };
        }
        try quake_spr.writeSprite(io, allocator, set.out_path, frames);
        std.debug.print("  wrote {s} ({d} frames)\n", .{ set.out_path, set.lumps.len });
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
            const resampled = try wolf_digi.resampleU8(allocator, sb.pcm, sb.rate, 11025);
            defer allocator.free(resampled);
            try quake_wav.writeWavU8(io, allocator, set.out_path, 11025, resampled);
        }
        std.debug.print("  wrote {s} ({d} samples @ {d}->11025 Hz)\n", .{ set.out_path, sb.pcm.len, sb.rate });
    }
}
