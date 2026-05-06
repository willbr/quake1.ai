// doom_wad.zig -- DOOM1.WAD reader (lumps + Doom picture format + DS* sounds).
//
// WAD layout (linuxdoom-1.10/w_wad.c):
//   header: 4 bytes "IWAD"/"PWAD" + i32 numlumps + i32 infotableofs
//   directory at infotableofs: numlumps × { i32 filepos, i32 size, [8]u8 name }
//
// Doom "picture format" (linuxdoom-1.10/r_data.c):
//   header: u16 width, height + i16 leftoffset, topoffset
//   u32 columnofs[width]   (byte offsets from start of patch)
//   columns: list of posts terminated by 0xFF
//     u8 topdelta            (0xFF = end-of-column)
//     u8 length
//     u8 pad
//     u8 pixels[length]
//     u8 pad
//
// DS* sound lumps:
//   u16 format(=3) + u16 sample_rate + u32 sample_count
//   u8 pcm[sample_count]  (8-bit unsigned mono)
//   16 bytes padding

const std = @import("std");
const Io = std.Io;
const File = std.Io.File;
const Dir = std.Io.Dir;
const Allocator = std.mem.Allocator;

pub const PALETTE_LUMP = "PLAYPAL";

pub const Lump = struct {
    name: [8]u8,
    filepos: u32,
    size: u32,

    pub fn nameTrimmed(self: *const Lump) []const u8 {
        const end = std.mem.indexOfScalar(u8, &self.name, 0) orelse self.name.len;
        return self.name[0..end];
    }
};

pub const Wad = struct {
    bytes: []u8,
    lumps: []Lump,

    pub fn deinit(self: *Wad, allocator: Allocator) void {
        allocator.free(self.bytes);
        allocator.free(self.lumps);
        self.* = undefined;
    }

    pub fn open(io: Io, allocator: Allocator, path: []const u8) !Wad {
        const f = try Dir.cwd().openFile(io, path, .{});
        defer f.close(io);
        const stat = try f.stat(io);
        const size: usize = @intCast(stat.size);
        const buf = try allocator.alloc(u8, size);
        errdefer allocator.free(buf);
        if ((try f.readPositionalAll(io, buf, 0)) != size) return error.WadTruncated;

        if (size < 12) return error.WadHeaderTooSmall;
        const ident = buf[0..4];
        if (!std.mem.eql(u8, ident, "IWAD") and !std.mem.eql(u8, ident, "PWAD"))
            return error.WadBadMagic;
        const numlumps_i = std.mem.readInt(i32, buf[4..8], .little);
        const infotableofs_i = std.mem.readInt(i32, buf[8..12], .little);
        if (numlumps_i < 0 or infotableofs_i < 0) return error.WadBadHeader;
        const numlumps: usize = @intCast(numlumps_i);
        const infotableofs: usize = @intCast(infotableofs_i);
        if (infotableofs + numlumps * 16 > size) return error.WadDirTruncated;

        const lumps = try allocator.alloc(Lump, numlumps);
        errdefer allocator.free(lumps);
        var i: usize = 0;
        while (i < numlumps) : (i += 1) {
            const ent = buf[infotableofs + i * 16 ..][0..16];
            lumps[i] = .{
                .filepos = std.mem.readInt(u32, ent[0..4], .little),
                .size    = std.mem.readInt(u32, ent[4..8], .little),
                .name    = ent[8..16].*,
            };
        }
        return .{ .bytes = buf, .lumps = lumps };
    }

    pub fn findLump(self: *const Wad, name: []const u8) ?*const Lump {
        for (self.lumps) |*l| {
            if (std.mem.eql(u8, l.nameTrimmed(), name)) return l;
        }
        return null;
    }

    pub fn lumpData(self: *const Wad, l: *const Lump) []const u8 {
        return self.bytes[l.filepos .. l.filepos + l.size];
    }

    /// Load PLAYPAL palette 0 (256 RGB triples, 6-bit-style but actually 8-bit).
    pub fn loadPlaypal0(self: *const Wad) ![256][3]u8 {
        const l = self.findLump(PALETTE_LUMP) orelse return error.NoPlaypal;
        const data = self.lumpData(l);
        if (data.len < 768) return error.PlaypalTruncated;
        var pal: [256][3]u8 = undefined;
        var i: usize = 0;
        while (i < 256) : (i += 1) {
            pal[i][0] = data[i * 3 + 0];
            pal[i][1] = data[i * 3 + 1];
            pal[i][2] = data[i * 3 + 2];
        }
        return pal;
    }
};

pub const PicSize = struct {
    width: u16,
    height: u16,
    left_offset: i16,
    top_offset: i16,
};

/// Decode a Doom "picture" lump into a paletted bitmap. Output buffer must
/// be at least `width * height` bytes; transparent pixels stay 0xFF.
pub fn decodePicture(lump: []const u8, out: []u8) !PicSize {
    if (lump.len < 8) return error.PicHeaderTooSmall;
    const width  = std.mem.readInt(u16, lump[0..2], .little);
    const height = std.mem.readInt(u16, lump[2..4], .little);
    const left_offset: i16 = @bitCast(std.mem.readInt(u16, lump[4..6], .little));
    const top_offset:  i16 = @bitCast(std.mem.readInt(u16, lump[6..8], .little));
    if (out.len < @as(usize, width) * @as(usize, height)) return error.PicOutBufTooSmall;
    if (lump.len < 8 + @as(usize, width) * 4) return error.PicTruncatedColumnTable;

    @memset(out[0 .. @as(usize, width) * @as(usize, height)], 0xFF);

    var col: usize = 0;
    while (col < width) : (col += 1) {
        const colofs = std.mem.readInt(u32, lump[8 + col * 4 ..][0..4], .little);
        var p: usize = colofs;
        while (true) {
            if (p >= lump.len) return error.PicColTruncated;
            const topdelta = lump[p];
            if (topdelta == 0xFF) break;
            if (p + 2 >= lump.len) return error.PicColTruncated;
            const length = lump[p + 1];
            // p + 2 = pad
            const px_start = p + 3;
            const px_end = px_start + length;
            if (px_end > lump.len) return error.PicColTruncated;
            var k: u8 = 0;
            while (k < length) : (k += 1) {
                const y: usize = @as(usize, topdelta) + k;
                if (y >= height) break;
                out[y * width + col] = lump[px_start + k];
            }
            p = px_end + 1; // skip trailing pad
        }
    }
    return .{ .width = width, .height = height, .left_offset = left_offset, .top_offset = top_offset };
}

pub const SoundBytes = struct {
    rate: u16,
    pcm: []const u8,
};

/// Parse a DSxxx lump. Returns a slice into the lump (no copy) — caller
/// retains the wad in scope.
pub fn parseSound(lump: []const u8) !SoundBytes {
    if (lump.len < 8) return error.SoundHeaderTooSmall;
    const fmt = std.mem.readInt(u16, lump[0..2], .little);
    if (fmt != 3) return error.SoundUnsupportedFormat;
    const rate = std.mem.readInt(u16, lump[2..4], .little);
    const count = std.mem.readInt(u32, lump[4..8], .little);
    if (8 + count > lump.len) return error.SoundTruncated;
    return .{ .rate = rate, .pcm = lump[8 .. 8 + count] };
}

/// Write a paletted bitmap to PPM using Doom's PLAYPAL for visual debug.
pub fn writePicPpm(io: Io, allocator: Allocator, path: []const u8, pixels: []const u8, width: u16, height: u16, palette: [256][3]u8) !void {
    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(allocator);

    try buf.print(allocator, "P6\n{d} {d}\n255\n", .{ width, height });
    for (pixels) |idx| {
        if (idx == 0xFF) {
            try buf.appendSlice(allocator, &[_]u8{ 255, 0, 255 });
        } else {
            try buf.appendSlice(allocator, &palette[idx]);
        }
    }
    const out = try Dir.cwd().createFile(io, path, .{});
    defer out.close(io);
    try out.writePositionalAll(io, buf.items, 0);
}
