// quake_spr.zig -- Write Quake SP1 sprite (.spr) files.
//
// File layout (see Quake-master/WinQuake/spritegn.h):
//   dsprite_t {
//     i32 ident       = "IDSP" little-endian
//     i32 version     = 1
//     i32 type        = SPR_VP_PARALLEL (2)
//     f32 boundingradius
//     i32 width       (max across frames)
//     i32 height      (max across frames)
//     i32 numframes
//     f32 beamlength  = 0
//     i32 synctype    = 0 (ST_SYNC)
//   }
//   for each frame:
//     i32 frametype = 0 (SPR_SINGLE)
//     dspriteframe_t { i32 origin[2], i32 width, i32 height }
//     width * height paletted bytes
//
// Origin convention (from Mod_LoadSpriteFrame): up=origin[1], down=origin[1]-h,
// left=origin[0], right=origin[0]+w. We write origin_x=-w/2 and origin_y=h
// so the sprite spans (-w/2, h) → (w/2, 0) in sprite-local space — convenient
// for our screen-space anchor (Phase B routes mod_sprite viewmodels through
// a dedicated bottom-center blit).

const std = @import("std");
const Io = std.Io;
const Dir = std.Io.Dir;
const Allocator = std.mem.Allocator;

pub const SPR_VP_PARALLEL: i32 = 2;
pub const ST_SYNC: i32 = 0;
pub const SPR_SINGLE: i32 = 0;
const IDSP_MAGIC: i32 = 0x50534449; // 'I' | 'D'<<8 | 'S'<<16 | 'P'<<24

pub const Frame = struct {
    width: u32,
    height: u32,
    pixels: []const u8,    // width * height paletted bytes (Quake palette indices)
    origin_x: i32,
    origin_y: i32,
};

pub fn writeSprite(io: Io, allocator: Allocator, path: []const u8, frames: []const Frame) !void {
    if (frames.len == 0) return error.NoFrames;

    var max_w: u32 = 0;
    var max_h: u32 = 0;
    for (frames) |fr| {
        if (fr.pixels.len != fr.width * fr.height) return error.FrameSizeMismatch;
        if (fr.width > max_w) max_w = fr.width;
        if (fr.height > max_h) max_h = fr.height;
    }

    var buf: std.ArrayList(u8) = .empty;
    defer buf.deinit(allocator);

    // Bounding radius = sqrt((w/2)^2 + (h/2)^2) of the largest frame.
    const half_w_f: f32 = @as(f32, @floatFromInt(max_w)) * 0.5;
    const half_h_f: f32 = @as(f32, @floatFromInt(max_h)) * 0.5;
    const bounding_radius: f32 = @sqrt(half_w_f * half_w_f + half_h_f * half_h_f);

    // dsprite_t header
    try writeI32(allocator, &buf, IDSP_MAGIC);
    try writeI32(allocator, &buf, 1);                          // version
    try writeI32(allocator, &buf, SPR_VP_PARALLEL);            // type
    try writeF32(allocator, &buf, bounding_radius);            // boundingradius
    try writeI32(allocator, &buf, @intCast(max_w));            // width
    try writeI32(allocator, &buf, @intCast(max_h));            // height
    try writeI32(allocator, &buf, @intCast(frames.len));       // numframes
    try writeF32(allocator, &buf, 0);                          // beamlength
    try writeI32(allocator, &buf, ST_SYNC);                    // synctype

    for (frames) |fr| {
        try writeI32(allocator, &buf, SPR_SINGLE);             // frametype
        try writeI32(allocator, &buf, fr.origin_x);
        try writeI32(allocator, &buf, fr.origin_y);
        try writeI32(allocator, &buf, @intCast(fr.width));
        try writeI32(allocator, &buf, @intCast(fr.height));
        try buf.appendSlice(allocator, fr.pixels);
    }

    // Make sure the parent directory exists.
    if (std.fs.path.dirname(path)) |dir_path| {
        try Dir.cwd().createDirPath(io, dir_path);
    }

    const out = try Dir.cwd().createFile(io, path, .{});
    defer out.close(io);
    try out.writePositionalAll(io, buf.items, 0);
}

fn writeI32(allocator: Allocator, buf: *std.ArrayList(u8), v: i32) !void {
    var bytes: [4]u8 = undefined;
    std.mem.writeInt(i32, &bytes, v, .little);
    try buf.appendSlice(allocator, &bytes);
}

fn writeF32(allocator: Allocator, buf: *std.ArrayList(u8), v: f32) !void {
    const u: u32 = @bitCast(v);
    var bytes: [4]u8 = undefined;
    std.mem.writeInt(u32, &bytes, u, .little);
    try buf.appendSlice(allocator, &bytes);
}

// ---------------------------------------------------------------------------
// Round-trip parser for testing only.
// ---------------------------------------------------------------------------
pub const ReadHeader = struct {
    width: i32,
    height: i32,
    numframes: i32,
};

pub fn readHeader(data: []const u8) !ReadHeader {
    if (data.len < 36) return error.SprTruncated;
    const ident = std.mem.readInt(i32, data[0..4], .little);
    if (ident != IDSP_MAGIC) return error.SprBadMagic;
    const version = std.mem.readInt(i32, data[4..8], .little);
    if (version != 1) return error.SprBadVersion;
    return .{
        .width     = std.mem.readInt(i32, data[16..20], .little),
        .height    = std.mem.readInt(i32, data[20..24], .little),
        .numframes = std.mem.readInt(i32, data[24..28], .little),
    };
}
