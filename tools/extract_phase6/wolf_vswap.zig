// wolf_vswap.zig -- VSWAP.WL1 reader for Wolf3D shareware data.
//
// VSWAP layout (see wolf3d-master/WOLFSRC/ID_PM.C):
//   u16 chunkCount
//   u16 spriteStart       (first sprite chunk index)
//   u16 soundStart        (first sound chunk index)
//   u32 offsets[chunkCount]
//   u16 lengths[chunkCount]
//   chunk data
//
// Sprite chunks are between [spriteStart, soundStart). Each is "compiled
// column" format:
//   u16 leftpix
//   u16 rightpix
//   u16 colofs[rightpix - leftpix + 1]   // BYTE offsets into chunk
//   For each column, walk a list of post triplets:
//     u16 endY*2     (0 terminates)
//     u16 srcofs     (byte index into chunk; pixel for row y is at chunk[srcofs + y])
//     u16 startY*2
//   Plus the actual paletted pixel byte array somewhere in the chunk.
//
// Sprite is 64x64 paletted; transparent pixels are those not covered by any
// post — we initialize the output to 0xFF (transparent sentinel).

const std = @import("std");
const Io = std.Io;
const File = std.Io.File;
const Dir = std.Io.Dir;
const Allocator = std.mem.Allocator;

pub const SPRITE_DIM = 64;

/// 6-bit-per-channel Wolf3D VGA palette (extracted from GAMEPAL.OBJ at offset 119).
pub const wolf_palette_6bit: *const [768]u8 = @embedFile("wolf_palette.bin");

/// Convert 6-bit (0..63) to 8-bit (0..255) by bit replication.
pub fn channel6to8(v6: u8) u8 {
    return (v6 << 2) | (v6 >> 4);
}

pub const VSwap = struct {
    bytes: []u8,
    chunk_count: u16,
    sprite_start: u16,
    sound_start: u16,

    // Byte offsets into self.bytes for the offsets + lengths tables.
    // Reads go through chunkOffset/chunkLength because the tables are not
    // 4-byte aligned in the file (header is 6 bytes).
    offsets_byte_off: usize,
    lengths_byte_off: usize,

    pub fn deinit(self: *VSwap, allocator: Allocator) void {
        allocator.free(self.bytes);
        self.* = undefined;
    }

    pub fn open(io: Io, allocator: Allocator, path: []const u8) !VSwap {
        const f = try Dir.cwd().openFile(io, path, .{});
        defer f.close(io);

        const stat = try f.stat(io);
        const size: usize = @intCast(stat.size);
        const buf = try allocator.alloc(u8, size);
        errdefer allocator.free(buf);
        if ((try f.readPositionalAll(io, buf, 0)) != size) return error.VSwapTruncated;

        if (size < 6) return error.VSwapHeaderTooSmall;
        const chunk_count  = std.mem.readInt(u16, buf[0..2], .little);
        const sprite_start = std.mem.readInt(u16, buf[2..4], .little);
        const sound_start  = std.mem.readInt(u16, buf[4..6], .little);

        const offsets_off: usize = 6;
        const lengths_off: usize = offsets_off + @as(usize, chunk_count) * 4;
        const data_off:    usize = lengths_off + @as(usize, chunk_count) * 2;
        if (data_off > size) return error.VSwapTruncatedTables;

        return .{
            .bytes            = buf,
            .chunk_count      = chunk_count,
            .sprite_start     = sprite_start,
            .sound_start      = sound_start,
            .offsets_byte_off = offsets_off,
            .lengths_byte_off = lengths_off,
        };
    }

    pub fn chunkOffset(self: *const VSwap, idx: u16) u32 {
        const p: usize = self.offsets_byte_off + @as(usize, idx) * 4;
        return std.mem.readInt(u32, self.bytes[p..][0..4], .little);
    }

    pub fn chunkLength(self: *const VSwap, idx: u16) u16 {
        const p: usize = self.lengths_byte_off + @as(usize, idx) * 2;
        return std.mem.readInt(u16, self.bytes[p..][0..2], .little);
    }

    pub fn chunk(self: *const VSwap, idx: u16) ?[]const u8 {
        if (idx >= self.chunk_count) return null;
        const ofs: usize = @intCast(self.chunkOffset(idx));
        const len: usize = @intCast(self.chunkLength(idx));
        if (ofs == 0 or len == 0) return null;
        if (ofs + len > self.bytes.len) return null;
        return self.bytes[ofs .. ofs + len];
    }

    /// Decode a sprite chunk into a 64×64 paletted bitmap. Pixels are written
    /// row-major (y * 64 + x). Transparent pixels stay as 0xFF.
    pub fn decodeSprite(self: *const VSwap, idx: u16, out: *[SPRITE_DIM * SPRITE_DIM]u8) !void {
        const data = self.chunk(idx) orelse return error.SpriteEmpty;
        @memset(out, 0xFF);

        if (data.len < 4) return error.SpriteHeaderTooSmall;
        const leftpix  = std.mem.readInt(u16, data[0..2], .little);
        const rightpix = std.mem.readInt(u16, data[2..4], .little);
        if (leftpix > rightpix or rightpix >= SPRITE_DIM) return error.SpriteBadColumns;

        const num_cols: u16 = rightpix - leftpix + 1;
        const colofs_byte_start: usize = 4;
        const colofs_byte_end:   usize = colofs_byte_start + @as(usize, num_cols) * 2;
        if (colofs_byte_end > data.len) return error.SpriteTruncated;

        var col: u16 = 0;
        while (col < num_cols) : (col += 1) {
            const x: u16 = leftpix + col;
            const co_off = colofs_byte_start + @as(usize, col) * 2;
            const post_ofs: u16 = std.mem.readInt(u16, data[co_off..][0..2], .little);
            if (post_ofs == 0) continue;
            var p: usize = post_ofs;

            while (true) {
                if (p + 2 > data.len) return error.SpriteBadPost;
                const end_y2: u16 = std.mem.readInt(u16, data[p..][0..2], .little);
                if (end_y2 == 0) break;
                if (p + 6 > data.len) return error.SpriteBadPost;
                const src_ofs: i16 = @bitCast(std.mem.readInt(u16, data[p + 2 ..][0..2], .little));
                const start_y2: u16 = std.mem.readInt(u16, data[p + 4 ..][0..2], .little);
                p += 6;

                const start_y = start_y2 / 2;
                const end_y = end_y2 / 2;
                if (end_y > SPRITE_DIM or start_y > end_y) return error.SpriteBadPostRange;

                // pixel for row y: data[(i32)src_ofs + y]
                var y: u16 = start_y;
                while (y < end_y) : (y += 1) {
                    const px_idx: i32 = @as(i32, src_ofs) + @as(i32, y);
                    if (px_idx < 0 or @as(usize, @intCast(px_idx)) >= data.len) return error.SpritePixelOOB;
                    out[@as(usize, y) * SPRITE_DIM + x] = data[@intCast(px_idx)];
                }
            }
        }
    }
};

/// Write a 64×64 paletted bitmap as a P6 PPM (RGB, 8-bit) using the Wolf
/// VGA palette, for visual verification.
pub fn writeSpritePpm(io: Io, allocator: Allocator, path: []const u8, pixels: *const [SPRITE_DIM * SPRITE_DIM]u8) !void {
    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(allocator);

    try buf.print(allocator, "P6\n{d} {d}\n255\n", .{ SPRITE_DIM, SPRITE_DIM });
    var i: usize = 0;
    while (i < SPRITE_DIM * SPRITE_DIM) : (i += 1) {
        const idx = pixels[i];
        if (idx == 0xFF) {
            // Transparent → magenta.
            try buf.appendSlice(allocator, &[_]u8{ 255, 0, 255 });
        } else {
            const r6 = wolf_palette_6bit[@as(usize, idx) * 3 + 0];
            const g6 = wolf_palette_6bit[@as(usize, idx) * 3 + 1];
            const b6 = wolf_palette_6bit[@as(usize, idx) * 3 + 2];
            try buf.appendSlice(allocator, &[_]u8{
                channel6to8(r6),
                channel6to8(g6),
                channel6to8(b6),
            });
        }
    }

    const out = try Dir.cwd().createFile(io, path, .{});
    defer out.close(io);
    try out.writePositionalAll(io, buf.items, 0);
}
