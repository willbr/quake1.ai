// extract.zig -- Phase 6 asset extractor.
//
// Reads:   ref/wolf3d-data/VSWAP.WL1 + AUDIOHED.WL1 + AUDIOT.WL1
//          ref/doom-data/DOOM1.WAD
// Writes:  id1/progs/v_*.spr   (Quake SP1 viewmodel sprites)
//          id1/sound/phase6/*.wav (11025 Hz mono u8 PCM)
//
// Run via: zig build extract
//   Optional flags:
//     -test_palette   Print Quake palette + a few LUT lookups, exit.

const std = @import("std");
const Io = std.Io;
const Dir = std.Io.Dir;
const palette_mod = @import("quake_palette.zig");
const wolf_vswap  = @import("wolf_vswap.zig");
const wolf_digi   = @import("wolf_digi.zig");
const doom_wad    = @import("doom_wad.zig");
const quake_spr   = @import("quake_spr.zig");
const quake_wav   = @import("quake_wav.zig");
const manifest    = @import("manifest.zig");

pub fn main(init: std.process.Init) !void {
    const arena = init.arena.allocator();
    const io = init.io;
    const args = try init.minimal.args.toSlice(arena);

    var i: usize = 1;
    while (i < args.len) : (i += 1) {
        const arg = args[i];
        if (std.mem.eql(u8, arg, "-test_palette")) {
            try testPalette(io, arena);
            return;
        } else if (std.mem.eql(u8, arg, "-dump_sprite")) {
            i += 1;
            if (i >= args.len) {
                std.debug.print("-dump_sprite: missing chunk index\n", .{});
                return error.BadArg;
            }
            const idx = try std.fmt.parseInt(u16, args[i], 10);
            try dumpSprite(io, arena, idx);
            return;
        } else if (std.mem.eql(u8, arg, "-vswap_info")) {
            try vswapInfo(io, arena);
            return;
        } else if (std.mem.eql(u8, arg, "-test_wav")) {
            try testWav(io, arena);
            return;
        } else if (std.mem.eql(u8, arg, "-test_spr")) {
            try testSpr(io, arena);
            return;
        } else if (std.mem.eql(u8, arg, "-doom_info")) {
            try doomInfo(io, arena);
            return;
        } else if (std.mem.eql(u8, arg, "-dump_doom_sprite")) {
            i += 1;
            if (i >= args.len) return error.BadArg;
            try dumpDoomSprite(io, arena, args[i]);
            return;
        } else if (std.mem.eql(u8, arg, "-dump_doom_sound")) {
            i += 1;
            if (i >= args.len) return error.BadArg;
            try dumpDoomSound(io, arena, args[i]);
            return;
        } else if (std.mem.eql(u8, arg, "-dump_digi")) {
            i += 1;
            if (i >= args.len) {
                std.debug.print("-dump_digi: missing digi index\n", .{});
                return error.BadArg;
            }
            const idx = try std.fmt.parseInt(u16, args[i], 10);
            try dumpDigi(io, arena, idx);
            return;
        } else {
            std.debug.print("extract: unknown arg '{s}'\n", .{arg});
            return error.BadArg;
        }
    }

    try manifest.extractAll(io, arena);
}

fn testPalette(io: Io, allocator: std.mem.Allocator) !void {
    const pal = try palette_mod.loadPalette(io, allocator);
    std.debug.print("palette[0]   = {d} {d} {d}\n", .{ pal[0][0], pal[0][1], pal[0][2] });
    std.debug.print("palette[15]  = {d} {d} {d}\n", .{ pal[15][0], pal[15][1], pal[15][2] });
    std.debug.print("palette[255] = {d} {d} {d}\n", .{ pal[255][0], pal[255][1], pal[255][2] });

    const lut = palette_mod.buildLut(pal);

    // Black, mid-grey, white, primaries round-trip checks.
    const tests = [_][3]u8{
        .{ 0, 0, 0 },
        .{ 128, 128, 128 },
        .{ 255, 255, 255 },
        .{ 255, 0, 0 },
        .{ 0, 255, 0 },
        .{ 0, 0, 255 },
    };
    for (tests) |t| {
        const idx = palette_mod.nearestIndex(&lut, t[0], t[1], t[2]);
        const m = pal[idx];
        std.debug.print(
            "nearest({d:>3},{d:>3},{d:>3}) -> idx {d:>3} = {d:>3} {d:>3} {d:>3}\n",
            .{ t[0], t[1], t[2], idx, m[0], m[1], m[2] },
        );
    }

    // Confirm 255 never produced.
    var bin: u32 = 0;
    while (bin < 32768) : (bin += 1) {
        if (lut[bin] == 255) {
            std.debug.print("LUT BUG: index 255 produced at bin {d}\n", .{bin});
            return error.LutLeakedReservedIndex;
        }
    }
    std.debug.print("LUT ok: never produces reserved index 255\n", .{});
}

fn vswapInfo(io: Io, allocator: std.mem.Allocator) !void {
    var v = try wolf_vswap.VSwap.open(io, allocator, "ref/wolf3d-data/VSWAP.WL1");
    defer v.deinit(allocator);
    std.debug.print(
        "VSWAP: {d} chunks, sprite_start={d}, sound_start={d}\n",
        .{ v.chunk_count, v.sprite_start, v.sound_start },
    );
    std.debug.print("First 5 sprite chunks (idx, ofs, len):\n", .{});
    var i: u16 = v.sprite_start;
    var n: u16 = 0;
    while (i < v.sound_start and n < 5) : ({ i += 1; n += 1; }) {
        std.debug.print("  {d}: ofs={d} len={d}\n", .{ i, v.chunkOffset(i), v.chunkLength(i) });
    }
}

fn dumpSprite(io: Io, allocator: std.mem.Allocator, idx: u16) !void {
    var v = try wolf_vswap.VSwap.open(io, allocator, "ref/wolf3d-data/VSWAP.WL1");
    defer v.deinit(allocator);

    var pixels: [wolf_vswap.SPRITE_DIM * wolf_vswap.SPRITE_DIM]u8 = undefined;
    try v.decodeSprite(idx, &pixels);

    const out_path = try std.fmt.allocPrint(allocator, "wolf_sprite_{d}.ppm", .{idx});
    try wolf_vswap.writeSpritePpm(io, allocator, out_path, &pixels);
    std.debug.print("wrote {s}\n", .{out_path});
}

fn testWav(io: Io, allocator: std.mem.Allocator) !void {
    const pcm = [_]u8{ 128, 200, 50, 128, 128, 200, 50, 128 };
    const path = "tools/extract_phase6/test.wav";
    try quake_wav.writeWavU8(io, allocator, path, 11025, &pcm);

    const f = try std.Io.Dir.cwd().openFile(io, path, .{});
    defer f.close(io);
    const stat = try f.stat(io);
    const data = try allocator.alloc(u8, @intCast(stat.size));
    defer allocator.free(data);
    _ = try f.readPositionalAll(io, data, 0);

    if (data.len != 44 + pcm.len) return error.WavWrongSize;
    if (!std.mem.eql(u8, data[0..4], "RIFF")) return error.WavBadRiff;
    if (!std.mem.eql(u8, data[8..12], "WAVE")) return error.WavBadWave;
    if (!std.mem.eql(u8, data[12..16], "fmt ")) return error.WavBadFmt;
    if (!std.mem.eql(u8, data[36..40], "data")) return error.WavBadData;
    const channels = std.mem.readInt(u16, data[22..24], .little);
    const rate = std.mem.readInt(u32, data[24..28], .little);
    const bps = std.mem.readInt(u16, data[34..36], .little);
    if (channels != 1 or rate != 11025 or bps != 8) return error.WavWrongFormat;
    if (!std.mem.eql(u8, data[44..], &pcm)) return error.WavWrongPcm;

    try std.Io.Dir.cwd().deleteFile(io, path);
    std.debug.print("test_wav: ok ({d} bytes total)\n", .{data.len});
}

fn testSpr(io: Io, allocator: std.mem.Allocator) !void {
    // Synthetic 32x16 + 16x8 two-frame sprite, round-trip parse it.
    var px1: [32 * 16]u8 = undefined;
    var k: usize = 0; while (k < px1.len) : (k += 1) px1[k] = @intCast(k & 0xFF);
    var px2: [16 * 8]u8 = undefined;
    k = 0; while (k < px2.len) : (k += 1) px2[k] = @intCast((k * 7) & 0xFF);
    const frames = [_]quake_spr.Frame{
        .{ .width = 32, .height = 16, .pixels = &px1, .origin_x = -16, .origin_y = 16 },
        .{ .width = 16, .height = 8,  .pixels = &px2, .origin_x = -8,  .origin_y = 8 },
    };
    const path = "tools/extract_phase6/test.spr";
    try quake_spr.writeSprite(io, allocator, path, &frames);

    const f = try std.Io.Dir.cwd().openFile(io, path, .{});
    defer f.close(io);
    const stat = try f.stat(io);
    const data = try allocator.alloc(u8, @intCast(stat.size));
    defer allocator.free(data);
    _ = try f.readPositionalAll(io, data, 0);

    const hdr = try quake_spr.readHeader(data);
    std.debug.print(
        "wrote+parsed {s}: {d}x{d}, {d} frames\n",
        .{ path, hdr.width, hdr.height, hdr.numframes },
    );
    if (hdr.width != 32 or hdr.height != 16 or hdr.numframes != 2) return error.RoundTripFailed;

    try std.Io.Dir.cwd().deleteFile(io, path);
    std.debug.print("test_spr: ok\n", .{});
}

fn doomInfo(io: Io, allocator: std.mem.Allocator) !void {
    var w = try doom_wad.Wad.open(io, allocator, "ref/doom-data/DOOM1.WAD");
    defer w.deinit(allocator);
    std.debug.print("DOOM1.WAD: {d} lumps\n", .{w.lumps.len});
    // Scan all 4-letter sprite prefixes for player weapons.
    const prefixes = [_][]const u8{ "PUNG", "PISG", "SHTG", "CHGG", "MISG", "SAWG", "PISF", "SHTF", "CHGF", "MISF" };
    for (prefixes) |pre| {
        var line: [80]u8 = undefined;
        var idx: usize = 0;
        for (w.lumps) |*l| {
            const n = l.nameTrimmed();
            if (n.len < 5) continue;
            if (std.mem.startsWith(u8, n, pre)) {
                if (idx + n.len + 1 < line.len) {
                    @memcpy(line[idx .. idx + n.len], n);
                    line[idx + n.len] = ' ';
                    idx += n.len + 1;
                }
            }
        }
        std.debug.print("  {s}: {s}\n", .{ pre, line[0..idx] });
    }
}

fn dumpDoomSprite(io: Io, allocator: std.mem.Allocator, name: []const u8) !void {
    var w = try doom_wad.Wad.open(io, allocator, "ref/doom-data/DOOM1.WAD");
    defer w.deinit(allocator);

    const l = w.findLump(name) orelse return error.LumpNotFound;
    const data = w.lumpData(l);
    const pal = try w.loadPlaypal0();

    var pixels: [320 * 240]u8 = undefined;
    const sz = try doom_wad.decodePicture(data, &pixels);
    const out_path = try std.fmt.allocPrint(allocator, "doom_{s}.ppm", .{name});
    try doom_wad.writePicPpm(io, allocator, out_path, pixels[0 .. @as(usize, sz.width) * @as(usize, sz.height)], sz.width, sz.height, pal);
    std.debug.print("wrote {s} ({d}x{d}, leftofs={d}, topofs={d})\n", .{ out_path, sz.width, sz.height, sz.left_offset, sz.top_offset });
}

fn dumpDoomSound(io: Io, allocator: std.mem.Allocator, name: []const u8) !void {
    var w = try doom_wad.Wad.open(io, allocator, "ref/doom-data/DOOM1.WAD");
    defer w.deinit(allocator);

    const l = w.findLump(name) orelse return error.LumpNotFound;
    const sb = try doom_wad.parseSound(w.lumpData(l));
    const out_path = try std.fmt.allocPrint(allocator, "doom_{s}.raw", .{name});
    const f = try Dir.cwd().createFile(io, out_path, .{});
    defer f.close(io);
    try f.writePositionalAll(io, sb.pcm, 0);
    std.debug.print("wrote {s} ({d} samples @ {d} Hz)\n", .{ out_path, sb.pcm.len, sb.rate });
}

fn dumpDigi(io: Io, allocator: std.mem.Allocator, digi_idx: u16) !void {
    var v = try wolf_vswap.VSwap.open(io, allocator, "ref/wolf3d-data/VSWAP.WL1");
    defer v.deinit(allocator);

    const raw = try wolf_digi.extractDigiSound(allocator, &v, digi_idx);
    const resampled = try wolf_digi.resampleU8(allocator, raw, wolf_digi.SourceRate, 11025);

    const out_path = try std.fmt.allocPrint(allocator, "wolf_digi_{d}.raw", .{digi_idx});
    const f = try Dir.cwd().createFile(io, out_path, .{});
    defer f.close(io);
    try f.writePositionalAll(io, resampled, 0);
    std.debug.print(
        "wrote {s} ({d} bytes raw -> {d} bytes @ 11025 Hz)\n",
        .{ out_path, raw.len, resampled.len },
    );
}
