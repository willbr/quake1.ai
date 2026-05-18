// quake_palette_png.zig -- Decode tools/palette.png (16x16 RGB) and write
// id1/gfx/palette.lmp (256x RGB bytes). Provides the build-time fallback that
// lets palette.png act as the source of truth: delete the .lmp and the next
// build regenerates it. The engine itself still reads the binary .lmp at
// runtime via host.c's COM_LoadHunkFile call; this file isn't compiled into
// the engine.

const std = @import("std");
const Io = std.Io;
const Dir = std.Io.Dir;
const Allocator = std.mem.Allocator;

const PNG_SIG = [_]u8{ 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
const PNG_PATH = "tools/palette.png";
const LMP_PATH = "id1/gfx/palette.lmp";

const EXPECT_W: u32 = 16;
const EXPECT_H: u32 = 16;
const EXPECT_BPP: u32 = 3; // RGB8 → 3 bytes per pixel

pub fn regenerateLmpIfMissing(io: Io, allocator: Allocator) !void {
    if (Dir.cwd().openFile(io, LMP_PATH, .{})) |f| {
        f.close(io);
        return; // already present
    } else |_| {}

    // PNG is required at this point; if it's also missing we have nothing to
    // fall back on. Caller's pak0.pak loader will surface the missing-palette
    // error a layer further out.
    const png_file = Dir.cwd().openFile(io, PNG_PATH, .{}) catch |err| {
        std.debug.print("palette regen: {s} missing ({s})\n", .{ PNG_PATH, @errorName(err) });
        return err;
    };
    defer png_file.close(io);
    const stat = try png_file.stat(io);
    const png_bytes = try allocator.alloc(u8, @intCast(stat.size));
    defer allocator.free(png_bytes);
    if ((try png_file.readPositionalAll(io, png_bytes, 0)) != png_bytes.len)
        return error.PngTruncated;

    const rgb = try decodeRgbPng(allocator, png_bytes);
    defer allocator.free(rgb);
    if (rgb.len != 768) return error.PalettePngWrongSize;

    if (std.fs.path.dirname(LMP_PATH)) |dir_path|
        try Dir.cwd().createDirPath(io, dir_path);
    const out = try Dir.cwd().createFile(io, LMP_PATH, .{});
    defer out.close(io);
    try out.writePositionalAll(io, rgb, 0);
    std.debug.print("palette regen: wrote {s} from {s}\n", .{ LMP_PATH, PNG_PATH });
}

/// Decode a 16x16 RGB8 PNG into a flat 768-byte RGB buffer. Handles all five
/// PNG row filters so any PNG editor that round-trips palette.png will still
/// work. Allocates the result with `allocator`; caller frees.
pub fn decodeRgbPng(allocator: Allocator, png: []const u8) ![]u8 {
    if (png.len < 8 or !std.mem.eql(u8, png[0..8], &PNG_SIG)) return error.NotAPng;

    var w: u32 = 0;
    var h: u32 = 0;
    var bit_depth: u8 = 0;
    var color_type: u8 = 0;
    var have_ihdr = false;
    var idat: std.ArrayList(u8) = .empty;
    defer idat.deinit(allocator);

    var i: usize = 8;
    while (i + 12 <= png.len) {
        const clen: u32 = std.mem.readInt(u32, png[i..][0..4], .big);
        const tag = png[i + 4 ..][0..4];
        const end = i + 8 + clen;
        if (end + 4 > png.len) return error.PngChunkTruncated;
        const payload = png[i + 8 .. end];

        if (std.mem.eql(u8, tag, "IHDR")) {
            if (clen != 13) return error.PngBadIhdr;
            w = std.mem.readInt(u32, payload[0..4], .big);
            h = std.mem.readInt(u32, payload[4..8], .big);
            bit_depth  = payload[8];
            color_type = payload[9];
            // payload[10] compression = 0
            // payload[11] filter      = 0
            // payload[12] interlace   = 0 (we don't support Adam7)
            if (payload[12] != 0) return error.PngInterlaceUnsupported;
            have_ihdr = true;
        } else if (std.mem.eql(u8, tag, "IDAT")) {
            try idat.appendSlice(allocator, payload);
        } else if (std.mem.eql(u8, tag, "IEND")) {
            break;
        }
        // (CRC at end+4..end+8 is not validated -- zlib already detects bit
        //  rot via its Adler-32, and our PNG is committed-in-tree data.)
        i = end + 4;
    }

    if (!have_ihdr) return error.PngNoIhdr;
    if (bit_depth != 8) return error.PngUnsupportedDepth;
    if (color_type != 2) return error.PngExpectedRgb;
    if (w != EXPECT_W or h != EXPECT_H) return error.PngWrongDimensions;

    // zlib-decompress concatenated IDAT into a fixed-size buffer.
    const row_bytes: usize = w * EXPECT_BPP;
    const filtered_len: usize = h * (row_bytes + 1);
    const filtered = try allocator.alloc(u8, filtered_len);
    defer allocator.free(filtered);

    var src_reader: std.Io.Reader = .fixed(idat.items);
    var dst_writer: std.Io.Writer = .fixed(filtered);
    var decompress: std.compress.flate.Decompress = .init(&src_reader, .zlib, &.{});
    const n = decompress.reader.streamRemaining(&dst_writer) catch return error.PngZlibCorrupt;
    if (n != filtered_len) return error.PngIdatLengthMismatch;

    // Un-filter.
    const out = try allocator.alloc(u8, h * row_bytes);
    errdefer allocator.free(out);
    var prev_row: []const u8 = &.{};
    var y: usize = 0;
    while (y < h) : (y += 1) {
        const filt: u8 = filtered[y * (row_bytes + 1)];
        const row_in = filtered[y * (row_bytes + 1) + 1 ..][0..row_bytes];
        const row_out = out[y * row_bytes ..][0..row_bytes];
        try unfilterRow(filt, row_in, row_out, prev_row, EXPECT_BPP);
        prev_row = row_out;
    }
    return out;
}

fn unfilterRow(
    filt: u8,
    row_in: []const u8,
    row_out: []u8,
    prev_row: []const u8,
    bpp: usize,
) !void {
    switch (filt) {
        0 => @memcpy(row_out, row_in), // None
        1 => { // Sub: out[x] = in[x] + out[x-bpp]
            var x: usize = 0;
            while (x < row_in.len) : (x += 1) {
                const left: u8 = if (x >= bpp) row_out[x - bpp] else 0;
                row_out[x] = row_in[x] +% left;
            }
        },
        2 => { // Up: out[x] = in[x] + prev[x]
            var x: usize = 0;
            while (x < row_in.len) : (x += 1) {
                const up: u8 = if (prev_row.len == row_in.len) prev_row[x] else 0;
                row_out[x] = row_in[x] +% up;
            }
        },
        3 => { // Average: out[x] = in[x] + floor((left + up) / 2)
            var x: usize = 0;
            while (x < row_in.len) : (x += 1) {
                const left: u16 = if (x >= bpp) row_out[x - bpp] else 0;
                const up:   u16 = if (prev_row.len == row_in.len) prev_row[x] else 0;
                row_out[x] = row_in[x] +% @as(u8, @intCast((left + up) / 2));
            }
        },
        4 => { // Paeth: out[x] = in[x] + Paeth(left, up, upleft)
            var x: usize = 0;
            while (x < row_in.len) : (x += 1) {
                const a: i32 = if (x >= bpp) row_out[x - bpp] else 0;
                const b: i32 = if (prev_row.len == row_in.len) prev_row[x] else 0;
                const c: i32 = if (x >= bpp and prev_row.len == row_in.len) prev_row[x - bpp] else 0;
                row_out[x] = row_in[x] +% @as(u8, @intCast(paethPredictor(a, b, c)));
            }
        },
        else => return error.PngBadFilterType,
    }
}

fn paethPredictor(a: i32, b: i32, c: i32) i32 {
    const p = a + b - c;
    const pa = @abs(p - a);
    const pb = @abs(p - b);
    const pc = @abs(p - c);
    if (pa <= pb and pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}
