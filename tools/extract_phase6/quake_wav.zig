// quake_wav.zig -- Write Quake-friendly RIFF/WAVE files.
//
// Format: PCM, 1 channel, 8-bit unsigned, rate from caller (typically 11025).
// Quake's snd_mem.c loads RIFF "fmt " + "data" chunks of this shape directly.
//
// Layout:
//   "RIFF" + u32 file_size_minus_8 + "WAVE"
//   "fmt " + u32 16 + u16 fmt(=1 PCM) + u16 channels(=1) + u32 rate
//          + u32 byte_rate + u16 block_align + u16 bits_per_sample(=8)
//   "data" + u32 data_size + bytes...

const std = @import("std");
const Io = std.Io;
const Dir = std.Io.Dir;
const Allocator = std.mem.Allocator;

pub fn writeWavU8(io: Io, allocator: Allocator, path: []const u8, sample_rate: u32, pcm: []const u8) !void {
    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(allocator);

    const data_size: u32 = @intCast(pcm.len);
    const file_size_minus_8: u32 = 4 + (8 + 16) + (8 + data_size);

    try buf.appendSlice(allocator, "RIFF");
    try writeU32(allocator, &buf, file_size_minus_8);
    try buf.appendSlice(allocator, "WAVE");

    try buf.appendSlice(allocator, "fmt ");
    try writeU32(allocator, &buf, 16);                 // fmt chunk size
    try writeU16(allocator, &buf, 1);                  // PCM
    try writeU16(allocator, &buf, 1);                  // 1 channel
    try writeU32(allocator, &buf, sample_rate);        // sample rate
    try writeU32(allocator, &buf, sample_rate);        // byte rate (1 channel × 1 byte)
    try writeU16(allocator, &buf, 1);                  // block align
    try writeU16(allocator, &buf, 8);                  // bits per sample

    try buf.appendSlice(allocator, "data");
    try writeU32(allocator, &buf, data_size);
    try buf.appendSlice(allocator, pcm);

    if (std.fs.path.dirname(path)) |dir_path| {
        try Dir.cwd().createDirPath(io, dir_path);
    }
    const out = try Dir.cwd().createFile(io, path, .{});
    defer out.close(io);
    try out.writePositionalAll(io, buf.items, 0);
}

fn writeU16(allocator: Allocator, buf: *std.ArrayList(u8), v: u16) !void {
    var b: [2]u8 = undefined;
    std.mem.writeInt(u16, &b, v, .little);
    try buf.appendSlice(allocator, &b);
}

fn writeU32(allocator: Allocator, buf: *std.ArrayList(u8), v: u32) !void {
    var b: [4]u8 = undefined;
    std.mem.writeInt(u32, &b, v, .little);
    try buf.appendSlice(allocator, &b);
}
