// quake_palette.zig -- Load id1 palette + build 32K-entry RGB→index LUT.
//
// Quake's palette is 256 RGB triples. Source: id1/gfx/palette.lmp.
// In stock id1 it lives inside id1/PAK0.PAK as the lump "gfx/palette.lmp".
// We read it out of the PAK at extract time. If a loose file exists at
// id1/gfx/palette.lmp we prefer that (lets people override).
//
// LUT layout: 32768 entries indexed by ((r>>3)<<10) | ((g>>3)<<5) | (b>>3).
// Each entry is the nearest palette index in 0..254. Index 255 is reserved
// as the writer-side transparent sentinel and is never produced by the LUT.

const std = @import("std");
const Io = std.Io;
const File = std.Io.File;
const Dir = std.Io.Dir;
const Allocator = std.mem.Allocator;

pub const Palette = [256][3]u8;
pub const Lut = [32768]u8;

const PAK_HEADER_SIZE = 12;        // "PACK" + u32 dirofs + u32 dirlen
const PAK_DIRENT_SIZE = 64;        // [56]u8 name + u32 filepos + u32 filelen
const PALETTE_LUMP    = "gfx/palette.lmp";

pub fn loadPalette(io: Io, allocator: Allocator) !Palette {
    // 1. Loose file override.
    if (Dir.cwd().openFile(io, "id1/gfx/palette.lmp", .{})) |f| {
        defer f.close(io);
        var pal: Palette = undefined;
        const n = try f.readPositionalAll(io, std.mem.sliceAsBytes(pal[0..]), 0);
        if (n != 768) return error.PaletteTruncated;
        return pal;
    } else |_| {}

    // 2. Read out of id1/PAK0.PAK.
    const pak = try Dir.cwd().openFile(io, "id1/PAK0.PAK", .{});
    defer pak.close(io);

    var hdr: [PAK_HEADER_SIZE]u8 = undefined;
    if ((try pak.readPositionalAll(io, &hdr, 0)) != PAK_HEADER_SIZE) return error.PakTruncated;
    if (!std.mem.eql(u8, hdr[0..4], "PACK")) return error.PakBadMagic;
    const dirofs = std.mem.readInt(u32, hdr[4..8], .little);
    const dirlen = std.mem.readInt(u32, hdr[8..12], .little);
    if (dirlen % PAK_DIRENT_SIZE != 0) return error.PakBadDirSize;
    const num_entries = dirlen / PAK_DIRENT_SIZE;

    const dir_buf = try allocator.alloc(u8, dirlen);
    defer allocator.free(dir_buf);
    if ((try pak.readPositionalAll(io, dir_buf, dirofs)) != dirlen) return error.PakDirTruncated;

    var i: u32 = 0;
    while (i < num_entries) : (i += 1) {
        const e = dir_buf[i * PAK_DIRENT_SIZE ..][0..PAK_DIRENT_SIZE];
        const name_end = std.mem.indexOfScalar(u8, e[0..56], 0) orelse 56;
        const name = e[0..name_end];
        if (std.mem.eql(u8, name, PALETTE_LUMP)) {
            const filepos = std.mem.readInt(u32, e[56..60], .little);
            const filelen = std.mem.readInt(u32, e[60..64], .little);
            if (filelen != 768) return error.PaletteWrongSize;
            var pal: Palette = undefined;
            if ((try pak.readPositionalAll(io, std.mem.sliceAsBytes(pal[0..]), filepos)) != 768)
                return error.PaletteTruncated;
            return pal;
        }
    }
    return error.PaletteNotFound;
}

/// Build a 32K-entry nearest-color LUT. Index 255 is reserved (transparency)
/// and never selected; the LUT picks the best match in 0..254.
pub fn buildLut(palette: Palette) Lut {
    var lut: Lut = undefined;
    var bin: u32 = 0;
    while (bin < 32768) : (bin += 1) {
        // Bin centre in 8-bit RGB space (5-bit bin → 8-bit by left-shift+mid).
        const r5: u8 = @intCast((bin >> 10) & 31);
        const g5: u8 = @intCast((bin >> 5) & 31);
        const b5: u8 = @intCast(bin & 31);
        const r: i32 = @as(i32, r5) * 8 + 4;
        const g: i32 = @as(i32, g5) * 8 + 4;
        const b: i32 = @as(i32, b5) * 8 + 4;

        var best_idx: u8 = 0;
        var best_d2: i64 = std.math.maxInt(i64);
        var i: u16 = 0;
        while (i < 255) : (i += 1) {  // exclude index 255
            const dr: i64 = r - @as(i32, palette[i][0]);
            const dg: i64 = g - @as(i32, palette[i][1]);
            const db: i64 = b - @as(i32, palette[i][2]);
            const d2 = dr * dr + dg * dg + db * db;
            if (d2 < best_d2) {
                best_d2 = d2;
                best_idx = @intCast(i);
            }
        }
        lut[bin] = best_idx;
    }
    return lut;
}

/// Convenience: look up a single 8-bit RGB triple. Use sparingly; for whole
/// sprites pre-build the LUT once and index it directly.
pub fn nearestIndex(lut: *const Lut, r: u8, g: u8, b: u8) u8 {
    const bin: u32 = (@as(u32, r >> 3) << 10) | (@as(u32, g >> 3) << 5) | (b >> 3);
    return lut[bin];
}
