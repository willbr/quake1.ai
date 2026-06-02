#!/usr/bin/env python3
# Generates id1/actors/dummy.iqm : a 5-part rigid cube "actor" with a baked
# "look" animation (head joint yaws +/-30 deg). IQM v2. Quake axes: +X fwd, +Y left, +Z up.
import struct, os, math

OUT_IQM = os.path.join(os.path.dirname(__file__), "..", "id1", "actors", "dummy.iqm")

# --- joints: (name, parent, parent-relative translate) ; identity rot, unit scale ---
JOINTS = [
    ("base",  -1, (0, 0, 0)),
    ("chest",  0, (0, 0, 24)),
    ("head",   1, (0, 0, 28)),
    ("eye.L",  2, (8, 4, 4)),
    ("eye.R",  2, (8, -4, 4)),
    ("ponytail_01", 2, (-12, 0, 6)),   # idx 5: attach at the back of the head
    ("ponytail_02", 5, (-13, 0, 0)),   # idx 6: tail sticks straight back (sags under gravity)
    ("ponytail_03", 6, (-13, 0, 0)),   # idx 7
    ("ponytail_04", 7, (-13, 0, 0)),   # idx 8
    ("ponytail_05", 8, (-13, 0, 0)),   # idx 9
]
HEAD = 2

def joint_world(i):
    name, parent, t = JOINTS[i]
    if parent < 0:
        return t
    pw = joint_world(parent)
    return (pw[0] + t[0], pw[1] + t[1], pw[2] + t[2])

# --- parts: (joint_index, half_extents, center_offset_from_joint, material) ---
PARTS = [
    (0, (16, 16, 12), (0, 0, 12), "p_base"),
    (1, (12, 14, 12), (0, 0, 0),  "p_chest"),
    (2, (10, 10, 10), (0, 0, 0),  "p_head"),
    (3, (2, 2, 2),    (0, 0, 0),  "p_eye"),
    (4, (2, 2, 2),    (0, 0, 0),  "p_eye"),
    (5, (4, 4, 8),    (0, 0, 0),  "p_pony"),
    (6, (4, 4, 8),    (0, 0, 0),  "p_pony"),
    (7, (4, 4, 8),    (0, 0, 0),  "p_pony"),
    (8, (4, 4, 8),    (0, 0, 0),  "p_pony"),
    (9, (4, 4, 8),    (0, 0, 0),  "p_pony"),
]

CORNERS = [(-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1),
           (-1, -1, 1),  (1, -1, 1),  (1, 1, 1),  (-1, 1, 1)]
TRIS = [(0, 3, 2), (0, 2, 1), (4, 5, 6), (4, 6, 7),
        (0, 1, 5), (0, 5, 4), (2, 3, 7), (2, 7, 6),
        (1, 2, 6), (1, 6, 5), (0, 4, 7), (0, 7, 3)]
CORNER_UV = [(0, 0), (1, 0), (1, 1), (0, 1), (0, 0), (1, 0), (1, 1), (0, 1)]

positions = []; texcoords = []; blendidx = []; blendwt = []; triangles = []
for (ji, half, off, mat) in PARTS:
    jw = joint_world(ji)
    base_vtx = len(positions)
    for k, (cx, cy, cz) in enumerate(CORNERS):
        positions.append((jw[0] + off[0] + cx * half[0],
                          jw[1] + off[1] + cy * half[1],
                          jw[2] + off[2] + cz * half[2]))
        texcoords.append(CORNER_UV[k])
        blendidx.append((ji, 0, 0, 0))
        blendwt.append((255, 0, 0, 0))
    for (a, b, c) in TRIS:
        triangles.append((base_vtx + a, base_vtx + b, base_vtx + c))

numverts = len(positions); numtris = len(triangles)

# ---- animation: "look" clip, head yaws about +Z ----
NFRAMES = 16
FPS     = 10.0
AMP     = 30.0   # degrees
# per-frame head quaternion (x,y,z,w) about +Z
head_q = []
for f in range(NFRAMES):
    a = math.radians(AMP * math.sin(2.0 * math.pi * f / NFRAMES))
    head_q.append((0.0, 0.0, math.sin(a / 2.0), math.cos(a / 2.0)))
qz_vals = [q[2] for q in head_q]; qw_vals = [q[3] for q in head_q]
qz_min, qz_max = min(qz_vals), max(qz_vals)
qw_min, qw_max = min(qw_vals), max(qw_vals)
qz_scale = (qz_max - qz_min) / 65535.0 if qz_max > qz_min else 0.0
qw_scale = (qw_max - qw_min) / 65535.0 if qw_max > qw_min else 0.0

# poses: one per joint (parallel). mask bits: 0,1,2=Txyz 3,4,5,6=Qxyzw 7,8,9=Sxyz
poses = []   # (parent, mask, offset[10], scale[10])
for j, (name, parent, t) in enumerate(JOINTS):
    offset = [t[0], t[1], t[2], 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0]
    scale  = [0.0] * 10
    mask   = 0
    if j == HEAD:
        mask = (1 << 5) | (1 << 6)         # animate Qz, Qw only
        offset[5] = qz_min; scale[5] = qz_scale
        offset[6] = qw_min; scale[6] = qw_scale
    poses.append((parent, mask, offset, scale))

CHANNEL_ORDER = list(range(10))
num_framechannels = sum(bin(p[1]).count("1") for p in poses)   # = 2 (head Qz,Qw)

def enc(val, off, scl):
    if scl == 0.0:
        return 0
    v = int(round((val - off) / scl))
    return max(0, min(65535, v))

frame_words = []   # uint16 stream, frame-major then pose-major then channel order
for f in range(NFRAMES):
    for j, (parent, mask, offset, scale) in enumerate(poses):
        for c in CHANNEL_ORDER:
            if not (mask & (1 << c)):
                continue
            if j == HEAD and c == 5:
                frame_words.append(enc(head_q[f][2], offset[5], scale[5]))
            elif j == HEAD and c == 6:
                frame_words.append(enc(head_q[f][3], offset[6], scale[6]))
            else:
                frame_words.append(0)
assert len(frame_words) == NFRAMES * num_framechannels

# ---- string table ----
strings = b"\0"
def add_str(s):
    global strings
    o = len(strings); strings += s.encode("ascii") + b"\0"; return o
joint_name_ofs = [add_str(n) for (n, _, _) in JOINTS]
mesh_name_ofs  = [add_str("mesh%d" % i) for i in range(len(PARTS))]
mesh_mat_ofs   = [add_str(m) for (_, _, _, m) in PARTS]
anim_name_ofs  = add_str("look")

def align(n, a=8):
    return (n + (a - 1)) & ~(a - 1)

HDR_SIZE = 124
off = HDR_SIZE
ofs_text = off; off += len(strings); off = align(off)
ofs_meshes = off; off += 24 * len(PARTS); off = align(off)
NUM_VA = 4
ofs_vas = off; off += 20 * NUM_VA; off = align(off)
ofs_pos = off; off += numverts * 12
ofs_uv  = off; off += numverts * 8
ofs_bi  = off; off += numverts * 4
ofs_bw  = off; off += numverts * 4
off = align(off)
ofs_tris = off; off += numtris * 12; off = align(off)
ofs_joints = off; off += 48 * len(JOINTS); off = align(off)   # iqmjoint = 48 bytes
ofs_poses = off; off += 88 * len(poses); off = align(off)      # iqmpose = 88 bytes
ofs_anims = off; off += 20 * 1; off = align(off)               # iqmanim = 20 bytes
ofs_frames = off; off += NFRAMES * num_framechannels * 2; off = align(off)
filesize = off

hdr  = struct.pack("<16s", b"INTERQUAKEMODEL\0")
hdr += struct.pack("<III", 2, filesize, 0)
hdr += struct.pack("<II", len(strings), ofs_text)
hdr += struct.pack("<II", len(PARTS), ofs_meshes)
hdr += struct.pack("<III", NUM_VA, numverts, ofs_vas)
hdr += struct.pack("<III", numtris, ofs_tris, 0)
hdr += struct.pack("<II", len(JOINTS), ofs_joints)
hdr += struct.pack("<II", len(poses), ofs_poses)               # poses
hdr += struct.pack("<II", 1, ofs_anims)                        # anims
hdr += struct.pack("<IIII", NFRAMES, num_framechannels, ofs_frames, 0)  # frames..ofs_bounds=0
hdr += struct.pack("<II", 0, 0)   # comment
hdr += struct.pack("<II", 0, 0)   # extensions
assert len(hdr) == HDR_SIZE, len(hdr)

out = bytearray(filesize)
out[0:HDR_SIZE] = hdr
out[ofs_text:ofs_text + len(strings)] = strings

p = ofs_meshes; fv = 0; ft = 0
for i in range(len(PARTS)):
    out[p:p + 24] = struct.pack("<IIIIII", mesh_name_ofs[i], mesh_mat_ofs[i], fv, 8, ft, 12)
    p += 24; fv += 8; ft += 12

IQM_POSITION, IQM_TEXCOORD, IQM_BLENDINDEXES, IQM_BLENDWEIGHTS = 0, 1, 4, 5
IQM_UBYTE, IQM_FLOAT = 1, 7
p = ofs_vas
out[p:p+20] = struct.pack("<IIIII", IQM_POSITION,     0, IQM_FLOAT, 3, ofs_pos); p += 20
out[p:p+20] = struct.pack("<IIIII", IQM_TEXCOORD,     0, IQM_FLOAT, 2, ofs_uv);  p += 20
out[p:p+20] = struct.pack("<IIIII", IQM_BLENDINDEXES, 0, IQM_UBYTE, 4, ofs_bi);  p += 20
out[p:p+20] = struct.pack("<IIIII", IQM_BLENDWEIGHTS, 0, IQM_UBYTE, 4, ofs_bw);  p += 20

p = ofs_pos
for (x, y, z) in positions: out[p:p+12] = struct.pack("<fff", x, y, z); p += 12
p = ofs_uv
for (s, t) in texcoords: out[p:p+8] = struct.pack("<ff", s, t); p += 8
p = ofs_bi
for b in blendidx: out[p:p+4] = struct.pack("<BBBB", *b); p += 4
p = ofs_bw
for w in blendwt: out[p:p+4] = struct.pack("<BBBB", *w); p += 4
p = ofs_tris
for (a, b, c) in triangles: out[p:p+12] = struct.pack("<III", a, b, c); p += 12
p = ofs_joints
for i, (name, parent, t) in enumerate(JOINTS):
    out[p:p+48] = struct.pack("<iiffffffffff", joint_name_ofs[i], parent,
        t[0], t[1], t[2], 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0); p += 48
p = ofs_poses
for (parent, mask, offset, scale) in poses:
    out[p:p+88] = struct.pack("<iI20f", parent, mask, *offset, *scale); p += 88
p = ofs_anims
out[p:p+20] = struct.pack("<IIIfI", anim_name_ofs, 0, NFRAMES, FPS, 1)  # flags: IQM_LOOP
p += 20
p = ofs_frames
for w in frame_words: out[p:p+2] = struct.pack("<H", w); p += 2

os.makedirs(os.path.dirname(OUT_IQM), exist_ok=True)
with open(OUT_IQM, "wb") as f: f.write(out)
assert len(out) == filesize, (len(out), filesize)
print("wrote", os.path.normpath(OUT_IQM), filesize, "bytes;",
      numverts, "verts", numtris, "tris", len(JOINTS), "joints,",
      NFRAMES, "frames", num_framechannels, "framechannels")
