# Video recording (dev screen capture)

_Extracted from CLAUDE.md (reference detail; CLAUDE.md keeps a summary + pointer here)._

### Video recording (dev screen capture)

`sdlquake/platform/video_record.c` — a development-only MPEG-1 screen recorder
(Mac/Windows desktop) over the vendored single-file `sdlquake/vendor/jo_mpeg/`
writer. `Video_Record_CaptureFrame` runs each `VID_Update` (after
`gpu_render_frame`, while `vid.buffer` still holds the frame): a wallclock CFR
sampler keyed off `Sys_FloatTime()` palette-expands the 8-bit framebuffer to
RGBX (via `d_8to24table`, same as the screenshot path) at the engine's current
`vid_scale` resolution. Output goes to gitignored `videos/`.

- Console: **`recordvideo [name] [dur]`** / **`stopvideo`**; cvars
  **`record_fps`** (default 60, snapped to jo_mpeg's legal 24/25/30/50/60) and
  **`record_maxdim`** (default 960; long-edge cap on the *encoded* frame, 0 =
  full res). `dur` mirrors the `profile` command — `30s`, `2m`, or `level`
  (record until the level ends: map change / intermission / disconnect, with the
  same pending-latch so `recordvideo level` from the command line defers until a
  map loads). Command names avoid Quake's existing demo `record`/`stop`. No
  audio (yet).
- **Encode downscale (`record_maxdim`)**: correct-speed CFR needs the encoder to
  actually sustain `record_fps`, and at the 4× framebuffer (1280×800) it tops
  out at ~55fps — so 60fps full-res played ~1.1× fast. The source is box-
  downscaled by the smallest integer factor whose long edge fits `record_maxdim`
  (1280×800 → 640×400 at the 960 default; vid_scale 3's 960×600 stays native),
  cutting per-frame encode cost ~4× so 60fps fits with headroom. The downscale
  is in the producer's expand pass (`vr_expand_downscale`).
- **Encoding is parallel** — the MPEG-1 DCT is far slower than a frame
  (inline it tanked the game to ~1fps; a single worker only sustained ~15fps at
  1280×800, dropping half the frames so a CFR stream played ~2× too fast). Since
  each jo_mpeg frame is an independent intra-coded sequence, frames encode
  concurrently: the **render thread (producer)** expands a frame into a free
  slot of a 12-slot ring and queues it (it *blocks* rather than drops when the
  window is full, so no frame is lost and playback speed stays correct); **N =
  clamp(cores−2, 2, 6) encode workers** turn slots into MPEG byte buffers; **one
  writer thread** concatenates them to the file in capture order. Sync is one
  `SDL_Mutex` + four `SDL_Condition`s, drained on stop so the tail isn't lost.
  jo_mpeg was given an in-memory entry point (`jo_encode_mpeg_frame` → malloc'd
  bytes; `jo_write_mpeg(FILE*)` kept as a wrapper, bitstream unchanged).
- Convert/transcode outside the engine with ffmpeg (e.g. `ffmpeg -i clip.mpg
  -c:v libx264 -crf 20 -pix_fmt yuv420p clip.mp4`).

