// wolf_digi.zig -- Wolf3D digital sound extractor.
//
// Surprise: digital PCM sounds are NOT in AUDIOT.WL1 — that file only holds
// PC-speaker + Adlib (FM synth) data. Digital sounds live at the *end* of
// VSWAP.WL1, paged in 4096-byte chunks. The very last chunk in VSWAP holds
// `DigiList`, an array of (u16 page_offset, u16 byte_count) pairs indexed
// by digital-sound number.
//
// See wolf3d-master/WOLFSRC/ID_SD.C `SDL_SetupDigi` for the original loader.
//
// Wolf3D shareware (WL1) digital-sound map (from WL_MAIN.C `wolfdigimap`):
//   ATKMACHINEGUNSND → 4
//   ATKPISTOLSND     → 5
//   ATKGATLINGSND    → 6
// Knife (ATKKNIFESND) has no digital variant in shareware — it's Adlib-only.
//
// Sample rate: 7000 Hz, 8-bit unsigned PCM, mono.

const std = @import("std");
const Allocator = std.mem.Allocator;
const wolf_vswap = @import("wolf_vswap.zig");

pub const PMPageSize: usize = 4096;
pub const SourceRate: u32 = 7000;

/// Concatenate the bytes of digital sound `digi_idx` (the index in DigiList,
/// not the LASTSOUND enum) into a freshly-allocated buffer.
pub fn extractDigiSound(allocator: Allocator, v: *const wolf_vswap.VSwap, digi_idx: u16) ![]u8 {
    // Last chunk of VSWAP is the DigiList page.
    const last_chunk_idx: u16 = v.chunk_count - 1;
    const list = v.chunk(last_chunk_idx) orelse return error.DigiListMissing;
    if (list.len < @as(usize, digi_idx + 1) * 4) return error.DigiIndexOOB;

    const page_off = std.mem.readInt(u16, list[@as(usize, digi_idx) * 4 ..][0..2], .little);
    const byte_count = std.mem.readInt(u16, list[@as(usize, digi_idx) * 4 + 2 ..][0..2], .little);
    if (byte_count == 0) return error.DigiSoundEmpty;

    var out = try allocator.alloc(u8, byte_count);
    errdefer allocator.free(out);

    var written: usize = 0;
    var ck: u16 = v.sound_start + page_off;
    while (written < byte_count) {
        if (ck >= last_chunk_idx) return error.DigiRanOff;
        const data = v.chunk(ck) orelse return error.DigiChunkMissing;
        const remain = byte_count - written;
        const n = @min(data.len, remain);
        @memcpy(out[written .. written + n], data[0..n]);
        written += n;
        ck += 1;
    }
    return out;
}

/// Linear-resample 8-bit unsigned PCM from `src_rate` to `dst_rate`.
pub fn resampleU8(allocator: Allocator, in: []const u8, src_rate: u32, dst_rate: u32) ![]u8 {
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
