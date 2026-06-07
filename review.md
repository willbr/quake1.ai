# Codebase review — `quake1.ai`

Whole-codebase review of the SDL3 port + Zig build + MCP server + hot-reload
game DLL + ImGui editor + Phase 8 sim systems + vendored qbsp/light/vis +
engine-fork patches. Findings below are ones I read the code to confirm; a
few are flagged as "verify" where I leaned on a subagent's summary.

## Top of mind — the five fixes that retire the most risk

Roughly 150 lines of change between them; addresses the highest-severity
real problems.

1. **Gate `do_load` behind "not in a live SV session"**, or scan-and-null
   `v.think`/`v.touch`/`v.use`/`v.blocked` on reload. Fixes finding #1
   (dangling function pointers in 217 sites after hot-reload).
2. **Add a size-settled wait before `SDL_CopyFile`** in hot-reload. Fixes
   finding #2 (partial-DLL race when the linker is still writing).
3. **Replace `WriteBSPFile("light_inproc.bsp")` with the existing qbsp
   membuf path.** Fixes finding #3 (CWD pollution + disk fsync per
   relight).
4. **Add `numbrushes`/`numfaces`/`entities`/`num_entities` to
   `qbsp_namespace.h`.** Fixes finding #8 (latent link-time collision).
5. **Bump `PROTOCOL_VERSION` from 15 to 16.** Fixes finding #6 (5 new TE
   codes added without a version stamp).

---

## Critical

### 1. `game.dll` hot-reload dangles 217+ function pointers stored in live edicts

**RESOLVED 2026-06-07** on two fronts. (a) Live hot-reload: reloads now defer
while `svb_active()` (hotreload.c:1201) — the DLL is only swapped at the main
menu / between maps, so no live edict ever holds a pointer into an unloaded
module. (Note: Q2's offset-rebase would *not* be safe here — a recompile
reorders functions, so per-edict offsets shift between builds; deferring is the
correct fix.) (b) Savegames: the dangling-across-serialization case is fixed by
the relocation-token mechanism described in #11. See the **Savegames** section
in CLAUDE.md.

Verified:
- 217 assignments of the form `e->v.think = …`, `e->v.touch = …`,
  `e->v.use = …`, `e->v.blocked = …` in `sdlquake/game/` (grep count).
- These are raw C function pointers into the loaded `game.dll`'s `.text`.
- `hotreload.c::do_load` (L1045–1086) unloads the old DLL, copies+loads a
  fresh one (almost certainly mapped at a different base on Windows because
  of /DYNAMICBASE), sets `g_game_api` to the new vtable, and calls
  `init()`.
- **There is no scan of `sv.edicts` to rebind `v.think`/`v.touch`/`v.use`/
  `v.blocked` after reload.** Grep finds zero matches for "rebind",
  "fixup.*think", or "HotReload.*edict". The only `v.think` write in the
  engine is `pr_exec.c:660`, which is the old QuakeC path that doesn't run
  in NATIVE_GAME mode.
- After any reload during live play, the next `SV_Physics` walk that fires
  an edict whose `v.think` was set by the old DLL calls into unmapped or
  recycled memory.

Why it hasn't been a daily crash: reloads tend to happen during quiet
moments; on macOS the dylib may map at the same address often; live play
during a reload may be rare in practice.

Possible fixes, cheapest first:
- Reject reloads while `sv.active` is true; allow only at main menu / map
  load. (Smallest behaviour change.)
- On reload, walk `sv.edicts` and null out `v.think`/`v.touch`/`v.use`/
  `v.blocked` (accepts one missed tick per edict).
- Indirection registry: game exports symbol names via
  `g_game_api->resolve_think("SUB_regen")`; edicts store name indices, not
  pointers. (Most invasive.)

### 2. `do_load` partial-copy race — `engine/hotreload.c:1050`

`SDL_CopyFile(GAME_DLL_SRC, GAME_DLL_LOAD)` fires the instant the mtime
poll ticks. There is no "size has stopped changing" wait. If the linker is
still writing when mtime publishes, a torn binary gets copied and loaded.

Fix: poll `SDL_GetPathInfo(SRC).size` until stable for ~100 ms before
calling `SDL_CopyFile`.

### 3. `vendor/light/light_lib.c:206` — unconditionally writes `light_inproc.bsp` to CWD on every relight

`WriteBSPFile("light_inproc.bsp")` is unconditional and verified. Relights
happen interactively in the editor (every brush move can trigger one).
This writes several MB to disk per relight and may overwrite an unrelated
user file named `light_inproc.bsp`.

Fix: route through `qbsp_membuf` (the in-memory BSP buffer the rest of the
lib uses).

### 4. `vendor/vis/flow.c` — unbounded recursion

`RecursiveLeafFlow` is recursive (self-call L263, L329) and so is
`SimpleFlood` (self-call L401). The original tool ran as a subprocess
where a stack overflow only killed the tool; in-process, it kills the
engine. Risk is map-dependent.

Fix: convert to an explicit stack, or set a recursion-depth limit and
abort the bake with an error message instead of overflowing.

### 5. `vendor/qbsp/map.c:33` — `strcpy(miptex[i], name)` into 16-byte slot, no bounds check

Inherited Carmack code. `miptex_t` is `char[16]`. The editor's texture
cache can pass arbitrary-length names through here. Long texture name →
silent corruption, then crash.

Fix: `Q_strlcpy(miptex[i], name, sizeof(miptex[i]))`, or validate at the
editor boundary.

---

## High

### 6. `engine_src/protocol.h` — 5 new `TE_*` codes added without bumping `PROTOCOL_VERSION` (still 15)

Verified: codes 16–20 added (TE_WATERSPLASH, TE_BLOODSPRAY, TE_SPARKBURST,
TE_SHELLEJECT, TE_DEBUGBLOOD). `cl_tent.c` handles them so same-binary
play works. An old binary parsing a packet from a new binary hits
`Sys_Error("CL_ParseTEnt: bad type")` and exits. Mostly irrelevant if no
mixed-binary scenarios exist, but `PROTOCOL_VERSION` is the contract.

Fix: bump to 16.

### 7. `platform/snd_sdl.c` — cross-thread state without atomics

`dma_play_pos` is declared `static volatile int` (L22) and read/written
between the SDL audio callback thread (L60–61) and the main thread
(L122). `volatile` is not a memory barrier.

- On x86 the 32-bit aligned read/write is naturally atomic but ordering
  between the callback's buffer reads and its samplepos write isn't
  enforced.
- On ARM (Apple Silicon — the macOS target) the producer's writes to
  `shm->buffer` and reads of `shm->samplepos` need release/acquire pairing
  for the consumer to see consistent state.
- Practical symptom: rare unreproducible audio glitches.

Fix: `SDL_AtomicSetInt`/`GetInt` on `samplepos`, with
`SDL_MemoryBarrierAcquire`/`Release` around the buffer access.

### 8. `vendor/qbsp/qbsp_namespace.h` — generic globals unprefixed

`numbrushes`, `numfaces`, `nummiptex`, `entities`, `num_entities`,
`numtexinfo` are NOT renamed. The header is deliberately minimal
("add new entries here when a future link-time collision shows up"). The
light/vis namespace headers are aggressive; qbsp is the unprefixed base.
No collision today, but `entities` and `num_entities` are
catastrophically generic globals to leave un-prefixed.

Fix: add `qbsp_` prefixes now. One-time, harmless, removes a foot-gun.

### 9. `vendor/qbsp/qbsp_lib.c` — `qbsp_reset_state` is a hand-curated reset list

The reset enumerates a manually-maintained set of globals. Add a new
global to any qbsp source file and forget to add it here → the second
compile in a session uses stale state. Silent failure mode.

Fix: declare resettable globals via a header macro expanded both in the
source (`int FOO;`) and in the reset function (`FOO = 0;`). Single source
of truth.

---

## Medium

### 10. `platform/sys_crash.c:249` — POSIX handler calls `snprintf` in the signal handler

Verified. `snprintf` is not POSIX async-signal-safe.
`backtrace_symbols_fd` is (or "best effort" on macOS). The realistic
failure: a crash inside the malloc heap re-enters allocator state from
snprintf, the handler hangs without printing — exactly when you need the
trace.

Fix: hand-format the header with `write()` of pre-built strings + a
locale-free integer-to-decimal helper. The Windows path (`crash_filter`)
is in-process unhandled-exception filter, not a signal handler, so it's
fine.

### 11. Savegames silently break when entvars layout shifts

**RESOLVED 2026-06-07.** The native-mode `ED_Write`/`ED_WriteGlobals` stubs are
now real (`pr_edict.c`): a field table (`s_nfields[]`) drives field-by-field
serialization, `entvars_t` callbacks survive via `func_to_token`/`token_to_func`
relocation (GAME_API_VERSION 38), edict refs serialize as numbers, and
`SAVEGAME_VERSION` is bumped 6→7 so the loader rejects the old stub format.
Verified: save→load round-trips on e1m1 in-process *and* cold-start (proving the
relocation survives an ASLR rebase). Was the Q2 `g_save.c` salvage
(`docs/quake2-ideas-to-steal.md` #1). Remaining gap: a save still does not
survive a `game.dll` *recompile* (offsets move) — documented, acceptable.

Original report — verified: `Host_Savegame_f` exists and `ED_Write` walks fields. Phase 8
added `button3`, `button4`, `decal_on_bounce` to `entvars_t`. Loading a
pre-Phase-8 save into a current binary: missing fields default to zero on
the new side (usually OK). The reverse (load a current save into an older
binary) corrupts. There is no savegame version stamp.

Engine-fork patches also added fields to `dlight_t`
(`client.h:80` — `vec3_t color`) and `msurface_t` (`model.h:107–145`
changed `dlightbits` and `cachespots`). These are recomputed on map load,
not saved, so the in-memory layout shift is not a savegame issue per se —
but it would break demo recording cross-binary if anyone relies on it.

Fix: add a `SAVEGAME_VERSION` byte and reject mismatched versions.

### 12. `editor/light_bake_thread.c::Editor_LightBake_Shutdown` leaks worker thread + frees its scratch under it

Verified. Lines 158–168. Comment admits this. If shutdown fires while a
bake is in flight, the worker continues writing to freed `s_snap.*`
buffers. Only matters on app exit, but presents as "the editor crashes
when I quit during a bake."

Fix: join with timeout before freeing, or set a `s_shutdown` flag the
worker checks before its final writes.

### 13. First-bake lightmap read/write race in `light_bake_thread.c::apply_snapshot`

Before the first successful bake completes, `mod->lightdata` still points
to the buffer that LIGHT mutates during the bake. The renderer reads
`surf->samples` (which derives from `mod->lightdata`) while the worker
writes it. One-frame visual artifact maximum, not a crash. Subsequent
bakes are safe because Apply repoints `mod->lightdata` to
`s_live_lightdata`.

Fix: on first bake, gate rendering of newly-baked surfaces until apply
completes (or accept the one-frame artifact and document it).

### 14. `vendor/light/light_lib.c` worker thread — `Error()` calls `exit()`, kills engine

The lib uses `setjmp`/`longjmp` (`qbsp_err_jmp`) to catch errors back into
a return value, but per a subagent's read the relight thread runs with
`qbsp_err_jmp = NULL`. An `Error()` call mid-bake kills the process.

Fix: thread-local `setjmp` frame on the relight thread, or convert
`Error()` to a thread-aware return path.

Status: needs direct verification of the `qbsp_err_jmp` setup on the
worker thread.

### 15. Edict pointer dangling across frames in gameplay code (pattern, multiple sites)

The gameplay port stores `owner`/`enemy`/`oldenemy`/`inflictor` as raw
`edict_t*` across frames. The engine recycles edict slots aggressively;
after a respawn or kill, a stored pointer may point at a different entity.
A subagent flagged ~10 specific sites in `combat.c`, `weapons.c`,
`items.c`, `ai.c`, and `sim/sim_ai.c`. The pattern is real; the specific
sites need confirmation case-by-case.

Architectural fix: introduce `ent_handle_t = { num, serial }` and a
`ent_resolve(handle)` helper; migrate cross-frame stores. The editor
already has the concept of a `spawn_serial` to copy.

### 16. Vendored tools — duplicate `bspfile.h` / `cmdlib.h` / `mathlib.h` per tool

Each of qbsp/light/vis ships its own copy of these headers. The build
only compiles qbsp's `.c` versions and forces light/vis TUs to link
against them via `-include qbsp_namespace.h`. Sound but brittle: if the
three header copies drift (someone patches `vendor/light/bspfile.h`
thinking it's the only copy), light's TU sees one struct layout and the
qbsp `.c` it links against sees another. Silent ABI mismatch.

Fix: delete the duplicate headers and force everyone to include qbsp's,
or add a build-time `cmp` check that the three are byte-identical.

---

## Low / informational

### 17. MCP `inspect_entity` includes `think_ptr` as `%p` — `mcp/mcp_server.c:1053`

Verified. Not a security bug given the localhost-bound listener; drop for
hygiene.

### 18. `PROTOCOL_VERSION` comment block in `protocol.h` doesn't say "bump on changes"

Documentation gap, not a bug. After fixing #6, add a comment.

### 19. MCP HTTP server lacks a `recv` timeout / slowloris-resistant accept loop

`mcp_server.c` HTTP transport is synchronous and single-threaded with no
`SO_RCVTIMEO`. Trivial slowloris on the dev port. Mitigated by the
localhost-only bind; still worth a one-line `setsockopt`.

### 20. `MEMORY.md` entry says hot-reload checks every ~1 s — correct, but worth knowing the polling cost is amortized

`RELOAD_CHECK_INTERVAL = 60` frames at 60 fps = 1 s. Each poll is a single
`SDL_GetPathInfo` stat, so the cost is negligible. Not a problem; just
documenting that the throttling is correct.

---

## Retractions — pass-1 findings that were wrong

These were reported by the first round of subagent reviews. Direct
reading of the code refutes them.

| Pass-1 claim | Reality |
|---|---|
| `sim_wind.c` callers of `idx_or_neg()` don't check −1 → `s_cells[-1]` | **Wrong.** Every site (L303, L354, L380, L425, L458, L486, L507) has `if (i < 0) continue;` immediately after. |
| `light_bake_thread.c` "no completion gate; both threads race the snapshot buffer" | **Wrong.** `s_in_progress`/`s_result_ready` are `SDL_AtomicInt`; worker owns heap buffers it allocates per-spawn; Trigger early-returns if already in flight. |
| MCP `inspect_entity` "leaks ASLR via `%p`" framed as a security bug | **Overstated.** The leak is real but the listener is localhost-only and the client is your own Claude Code. Aesthetic, not exploitable. |
| Hot-reload polling counter "doesn't reset on reload, wraps and polls immediately" | **Wrong.** Counter resets to 0 every `RELOAD_CHECK_INTERVAL` ticks unconditionally (`hotreload.c:1110`). |
| `hotreload.c:1041` "missing null check on `g_game_api` before shutdown" | **Wrong.** L1041: `if (g_game_api) { g_game_api->shutdown(); ... }`. |
| `combat.c` GIB ring "may skip slots when wrapping" | Unconfirmed; re-read suggests the loop terminates correctly. Flagged but not verified. |
| `d_scan.c` Bayer dither OOB | **Wrong.** Index range is bounded by construction. |
| MCP "header injection via `mcp_http_port`" | **Wrong.** `mcp_http_port` is a process-startup int, not attacker-controlled. |

---

## Architectural observations

### Hot-reload trust model

The current design assumes the DLL's function-pointer surface area is
small (the `engine_api_t`/`game_api_t` vtables). In reality the gameplay
code spreads function pointers across every live edict (217 sites). Either
narrow the surface (string keys + per-frame resolve) or fence reload to
safe points (between maps).

### In-process tool refactor is ~80% done

qbsp/light/vis are well-namespaced for types but only partially for
globals (#8). The reset story is hand-maintained (#9). Error paths still
`Error()`/`exit()` on at least one thread (#14). The remaining 20% is:
make `Error()` thread-safe and convertible to a return value, prefix the
remaining qbsp globals, and auto-generate the reset list. Two days of
work for a much more durable design.

### Savegame versioning needs to exist

#11 above. Right now any field add to `entvars_t` silently breaks
load-game compat in one direction. A 1-byte version stamp is cheap
insurance.

### Linux is genuinely untested

README says "untested but should work." Several platform-layer issues
(audio ordering #7, signal-handler async-safety #10) are worse on Linux
than on Apple Silicon. If Linux is ever a real target, this needs a
deliberate pass — start with running under TSan to surface #7.

### Engine fork divergence is manageable but unbounded

The diff against `ref/Quake-master/` covers ~40 modified files and ~10
new-only files. Patches I noticed:
- `client.h:80` — `vec3_t color` added to `dlight_t`.
- `model.h:107–145` — `msurface_t.dlightbits` changed from `int` to
  multi-word struct; `cachespots[MIPLEVELS]` expanded to
  `cachespots[MIPLEVELS][NUM_DITHER_BUCKETS]`; new `last_miplevel`,
  `last_bucket`, `rgb_samples`, `stain` fields.
- `model.h:372–383` — `live_rgblightdata` + size on `model_t`.
- `d_local.h:47` — `stain_gen`, `dither_gen` added to `surfcache_t`.
- New files: `r_decals.c`, `r_fog.c`, `r_livelight.c`, `r_lut.c`,
  `r_surf_rgb.c`, `r_drawflat.c`, `line_editor.c`, `cl_dlight_colors.h`.

The shifts are in-memory layout changes only, recomputed on map load.
Worth maintaining a `docs/engine-patches.md` index so a future reader
can find the seams.

---

## What I did NOT review

- Vendored `imgui-1.92.8`, `SDL3-3.4.8`, `stb` — third-party, untouched.
- Asset-extraction pipeline in `tools/extract_phase6/` — only skimmed via
  the build review.
- `.mcp.json` — file presence not confirmed.
- Whole-file walk of `engine_src/` — only the patches were diffed.

---

# What to remove — cruft, dead code, and old options

Found by enumerating what `build.zig` actually compiles versus what's on
disk, then sanity-checking each candidate. Roughly grouped from "slam
dunk, just delete it" to "judgment call."

## Tier 1 — slam dunks (1.6 MB + 5 dirs, zero behavioural risk) **[DONE]**

All Tier 1 dirs (`kit/` 600KB, `dxsdk/` 380KB, `scitech/` 556KB,
`gas2masm/` 72KB, `data/` 208KB) deleted in prior sessions. `docs/`
removed 2026-05-26 along with a parallel pile of pre-port build
artefacts that had been sitting next to the source: `*.bat` (DOS/Win
build scripts), `*.dsp`/`.dsw`/`.mdp`/`.ncb`/`.opt`/`.plg` (Visual
Studio 6 workspace files), `*.spec.sh` (Linux RPM specs), `*.aps`/`.rc`
(Win32 resource files), `*.ico`/`.gif` (icons/banner), `cwsdpmi.exe`
(DOS DPMI host), `Makefile.linuxi386`/`Makefile.Solaris`,
`progdefs.q1`/`progdefs.q2` (QC build artefacts; NATIVE_GAME path is
gone), and the leftover `3dfx.txt`/`glqnotes.txt`/`wqreadme.txt`/
`README.Solaris` readmes. ~40 files. Build clean afterwards.

## Tier 2 — 61 unbuilt `.c` files in `engine_src/` (probably ~1 MB source) **[DONE]**

All 71 `.c` files now in `engine_src/` are referenced by `build.zig` —
the GL renderer, DOS/Sun/Linux platform layers, i386 `.s` files, and
unused net/CD transports have all been deleted in prior sessions.

By category:

| Category | Files | Notes |
|---|---|---|
| OpenGL renderer | `gl_draw.c`, `gl_mesh.c`, `gl_model.c`, `gl_refrag.c`, `gl_rlight.c`, `gl_rmain.c`, `gl_rmisc.c`, `gl_rsurf.c`, `gl_screen.c`, `gl_test.c`, `gl_vidlinux.c`, `gl_vidlinuxglx.c`, `gl_vidnt.c`, `gl_warp.c`, `gl_warp_sin.h`, `gl_model.h` | Software renderer is the chosen path; the entire GL pipeline is dead. 14 files. |
| Win32 platform | `sys_win.c`, `sys_wind.c`, `vid_win.c`, `in_win.c`, `snd_win.c`, `net_win.c`, `net_wins.c`, `net_wipx.c`, `conproc.c`, `mplib.c`, `mplpc.c` | Replaced by SDL3 in `sdlquake/platform/`. |
| DOS platform | `sys_dos.c`, `vid_dos.c`, `vid_vga.c`, `vid_ext.c`, `vid_svgalib.c`, `in_dos.c`, `snd_dos.c`, `net_dos.c`, `dos_v2.c`, `vregset.c`, `dosisms.h` | DOS! |
| Unix variants | `sys_linux.c`, `sys_sun.c`, `in_sun.c`, `snd_linux.c`, `snd_next.c`, `snd_sun.c`, `vid_sunx.c`, `vid_sunxil.c`, `vid_x.c`, `net_bsd.c`, `net_udp.c` | Replaced by SDL3. |
| Null stubs | `sys_null.c`, `snd_null.c`, `vid_null.c`, `in_null.c`, `net_none.c` | Not used. |
| Net transports | `net_bw.c`, `net_ipx.c`, `net_mp.c`, `net_ser.c`, `net_comx.c`, `net_wso.c` | Only `net_dgrm`/`loop`/`main`/`vcr` are built. |
| CD audio | `cd_audio.c`, `cd_linux.c`, `cd_win.c` | Only `cd_null.c` is built. |
| Misc | `snd_gus.c` (Gravis Ultrasound, 1992 hardware) | |
| i386 assembly | `d_copy.s`, `d_draw.s`, `d_draw16.s`, `d_parta.s`, `d_polysa.s`, `d_scana.s`, `d_spr8.s`, `d_varsa.s`, `dosasm.s`, `math.s` | Not built on x64 or arm64. |

**Cost of keeping**: every code search across `engine_src/` returns hits in
files that don't run; every "where is this used" answer has to be
filtered through "is this file actually compiled?"; new readers waste
time on `gl_rsurf.c` and assume it matters.

**Cost of removing**: gives up the option to flip back to OpenGL or to a
non-SDL platform layer without re-vendoring from `ref/Quake-master/`. But
the diff baseline is already there in `ref/Quake-master/`, so this is a
git-history-level loss, not a "we can't get it back" loss.

**Recommendation**: delete. If a future need arises, copy back from
`ref/Quake-master/WinQuake/`.

## Tier 3 — `NATIVE_GAME=0` (QuakeC VM) path **[DONE]**

Build flag defaults `native_game = true`. The fallback path compiles in
`pr_exec.c` + `pr_cmds.c` (the QuakeC VM interpreter and builtins) and
keeps 67 `#ifdef NATIVE_GAME` branches alive throughout the engine. 144
QC-VM symbol references (`G_FLOAT`, `pr_globals`, `PR_ExecuteProgram`,
etc.) are scattered across the engine.

The gameplay code has been entirely hand-ported to C in `sdlquake/game/`,
and the project's whole Phase 8 design (sim systems, hot-reload) only
works with the native DLL. Going back to QC would mean recovering a
`progs.dat`, which would not have any of the Phase 5–8 work.

**Recommendation**: delete the option. Remove `native_game` from
`build.zig`, drop `pr_exec.c` + `pr_cmds.c` from the build (and likely
from disk), trim the 67 conditional branches, and slim `pr_edict.c` to
just the edict-storage pieces the native path actually uses.

**Caveat**: don't delete this if `progs.dat` interpretation is something
you specifically want to keep as a fallback or as a reference implementation.

## Tier 4 — DirectSound dead branches in shipping code **[DONE]**

`pDSBuf` and the `IDirectSoundBuffer` stub are gone from `snd_mix.c`,
`snd_dma.c`, `platform/snd_sdl.c`, and `platform/winquake.h` (now
52 lines, down from 104).

## Tier 5 — `ref/` directory (≈ 250 MB total)

| Subdir | Size | Used? |
|---|---|---|
| `Quake-master/` | 12 MB | **Yes** — diff baseline for `engine_src/` patches. Keep. |
| `Quake-Tools-master/` | 5.3 MB | **Yes** — diff baseline for vendored qbsp/light/vis. Keep. |
| `quake_map_source-master/` | **136 MB** | Reference id1 `.map` sources. Useful for the editor; massive. Consider moving outside the repo or git-LFS'ing. |
| `TrenchBroom-master/` | 40 MB | Reference for editor design. Was it ever actually grepped? If not, delete. |
_Removed 2026-06-07: `fteqw-master/` (30 MB) — modern QuakeWorld engine, mostly
GL/Vulkan/D3D and so irrelevant to a software-only fork. Salvaged the genuine
deltas to `docs/fteqw-ideas-to-steal.md` first: headline is FTE's scriptable
particle grammar (`client/r_part.c` + `partcfgs/*.cfg`); plus skeletal animation
blending/events. Its software renderer is a 32-bit RGBA compatibility fallback
with nothing to steal._

_Removed 2026-06-07: `Quake-2-master/` (6.5 MB) + `Quake-2-Tools-master/`
(1.5 MB) — Q2 is a sibling engine, ~90% structurally identical to our Q1 base,
so its reference value was thin. Salvaged the genuine deltas to
`docs/quake2-ideas-to-steal.md` first: the headline being Q2's reload-safe
function-pointer serialization (`game/g_save.c` segment-relative offsets), which
is the durable fix for open findings #1 (hot-reload dangling pointers) and #11
(savegame versioning); plus detail brushes (`CONTENTS_DETAIL`), qrad3 radiosity
bounce, and PHS for the in-process compile/relight/AI-sound chain._

_Removed 2026-06-07: `quake106/` (8.7 MB) — NOT a source release; it was the
Quake Shareware v1.06 DOS binary distribution (a `deice.exe` extractor + a 9 MB
LHa self-extracting `resource.1` archive of PAK assets + the DOS game binary).
Useless as a diff/reference baseline (no readable source, won't run on modern
hosts), and its only content — the shareware assets — is already committed loose
under `id1/`. Not superseded by `Quake-master/`; they're different kinds of thing
(source vs. packed installer)._

_Removed 2026-06-07: `wolf3d-data/`, `wolf3d-master/`, `DOOM-master/`, and
`doom-data/` — both the Wolf3D and Doom gun rosters were dropped, so their
shareware data, source trees (diff baselines), and the `tools/extract_phase6/`
extractor were deleted. Mechanics worth salvaging live in
`docs/wolf3d-ideas-to-steal.md` and `docs/doom-ideas-to-steal.md`._

**Recommendation**: audit your actual grep/Read history for these dirs.
The ones you never touch are 70+ MB of dead weight that bloats git
clones, slows down recursive greps, and pollutes search results.
`quake_map_source-master/` at 136 MB is by far the worst offender —
strongly consider moving it outside the repo or symlinking.

## Tier 6 — older docs and superseded plans

- `docs/engine/port-audit.md` — likely a historical doc from the
  start-of-port audit. If the audit's findings have all been addressed,
  archive or delete.
- `docs/superpowers/plans/` — Phase 8 plans dated 2026-05-04 through
  2026-05-14. The M1–M6 plans for completed milestones are now
  documentation of what was done, not a forward plan. Consider moving
  completed ones to `docs/history/` or deleting.
- `ideas.md` (345 lines) at repo root — user content; can't judge.

## Tier 7 — code architecture cleanups (not deletes per se)

These are smaller, less-obvious things that have become more complex than
they need to be after Phase 8.

### `platform/winquake.h` stub is 80% dead

After Tier 4 (delete the DirectSound stubs) and assuming Tier 1's
`dxsdk/` removal, `winquake.h` shrinks to ~20 lines of "typedef DWORD,
HWND etc. to void*". That's still useful for any inherited engine code
that mentions Win32 types in non-functional ways (debug strings, etc.).
Consider whether even those types are still referenced — if not, delete
the whole stub.

### Multiplayer netcode (`net_dgrm.c`, `net_main.c`, `net_vcr.c`)

`net_loop.c` is the in-process loopback for single-player. The other
three are for multiplayer / demo recording. If multiplayer is not a goal,
those are ~3000 lines of net dispatcher that runs every frame for no
benefit. Cost of removal: lose multiplayer + demo recording forever.
Cost of keeping: complexity surface that nothing exercises and no test
covers.

### `r_paths.c` (250 lines, engine glue)

This file is small but I noticed it wasn't deeply justified anywhere.
Worth a one-time read to confirm it's still doing something the engine
proper doesn't.

### Phase 6 weapon files

`game/weapons_phase6.c` (854 lines) and `game/player_phase6.c` (466
lines) are tagged "phase6" — Phase 6 is done. If they're now the
canonical weapon code, rename to drop the phase tag; if they're parallel
to `weapons.c`, that's confusing and should be unified.

### `game/items_push.c` (112 lines)

Small, possibly extracted for Phase 8 push/Gust interaction. Worth
checking if it duplicates anything in `items.c`.

---

## Summary of recommended removals

| What | Size | Status |
|---|---|---|
| `engine_src/kit/` | 600 KB | **[DONE]** |
| `engine_src/dxsdk/` | 380 KB | **[DONE]** |
| `engine_src/scitech/` | 556 KB | **[DONE]** |
| `engine_src/gas2masm/` | 72 KB | **[DONE]** |
| 61 unbuilt `.c`/`.s` files in `engine_src/` | ~1 MB | **[DONE]** |
| DirectSound branches in `snd_mix.c` + `winquake.h` stubs | ~150 LOC | **[DONE]** |
| `NATIVE_GAME=0` + `pr_exec.c` + `pr_cmds.c` + `pr_comp.h` + `progdefs.h` | ~5000 LOC | **[DONE]** |
| `engine_src/data/` (QC sources) | 208 KB | **[DONE]** |
| `engine_src/docs/` + pre-port build artefacts (.bat/.dsp/.spec.sh/.ico/cwsdpmi.exe/Makefile.*/progdefs.q?/etc.) | ~150 KB, 40 files | **[DONE]** 2026-05-26 |
| Unused `ref/` subdirs | up to ~220 MB | open — needs grep-history audit per dir |
| Old `docs/superpowers/plans/` for done milestones | small | open — move to `docs/history/` |

All zero-risk cleanup wins from Tiers 1–4 are done. Tier 5 (`ref/`
subdir audit) and Tier 6 (archive completed plans) are the remaining
open items, both judgment calls.
