# Networking & Protocol

The engine still speaks Quake's original wire protocol — version **15**.
Originals (`net_main.c`, `net_dgrm.c`, `net_loop.c`) compile unchanged; the
platform-specific socket layer is replaced by `net_sdl.c`.

In normal single-player play everything stays in-process via the loopback
driver — no socket is ever opened. The datagram driver is wired up and works
for LAN, but we don't ship a server browser or menu UI for it.

## Drivers

| Driver | File | Purpose |
|---|---|---|
| Loop | `net_loop.c` | In-process server↔client. Default for SP. Two ring buffers, no kernel involvement. |
| Datagram | `net_dgrm.c` | UDP. Uses Winsock on Windows via `net_sdl.c`'s socket primitives. |

Most engine call sites talk to the abstraction in `net_main.c`; the
driver list is fixed at compile time.

## Frame loop

Server side, per `Host_Frame`:

1. `SV_RunClients` — for each connected client, parse one `clc_*` packet from
   the buffer (button bits, movement, string commands).
2. `SV_Physics` — integrate entities (`MOVETYPE_FLY`, `_STEP`, `_TOSS`, …).
3. Game DLL's `start_frame` callback.
4. Per-entity `entity_think` callbacks (where `nextthink <= sv.time`).
5. `SV_SendClientMessages` — for each client, write an `svc_*` message
   containing the world delta since their last ack.

Client side, per `Host_Frame`:

1. `CL_ReadFromServer` — parse incoming `svc_*` messages, update `cl.*`.
2. `CL_SendMove` — pack the player's button state and view angles into a
   `clc_move` packet, send to server.

## Wire format

All multi-byte values are little-endian. Each packet is a sequence of
length-prefixed messages (loop driver) or framed UDP datagrams.

Inside a packet, each message starts with a one-byte command:

- `svc_*` from server to client (see `protocol.h`):
  - `svc_serverinfo` — handshake; map name, precache lists, gametype.
  - `svc_setview` — which entity is the player's "first-person" camera.
  - `svc_updatestat`, `svc_clientdata` — player stats (HP, ammo, weapon).
  - `svc_sound` — start a positional/looping sample.
  - `svc_temp_entity` — one-shot effects (gunshot, explosion, spike).
  - `svc_spawnstatic`, `svc_spawnbaseline` — static / dynamic entity setup.
  - `svc_lightstyle`, `svc_updatename`, `svc_updatecolors` — per-client
    cosmetic state.
  - `svc_print`, `svc_stufftext`, `svc_centerprint` — console / chat / HUD.
  - `svc_intermission`, `svc_finale`, `svc_cutscene` — end-of-level UIs.
  - …plus `svc_killedmonster`, `svc_foundsecret` for HUD counters.
- `clc_*` from client to server:
  - `clc_move` — usercmd (delta-time, view angles, button bits, impulse).
  - `clc_stringcmd` — console commands the client typed (`kill`, `say`).
  - `clc_disconnect`.

If the high bit of the server-side command byte is set, it's an
`svc_update` (entity delta) with the low bits selecting which fields are
included. `protocol.h` lists every `U_*` and `SU_*` bit. This is what
allows entity updates to fit in a few bytes most frames — only changed
fields are transmitted.

## Button bits and the Phase 8 expansion

`clc_move` carries an 8-bit button mask. The original engine uses bits 0–1
for `+attack` and `+jump`. We added two more (`+blink`, `+gust`) in
Phase 8 / M3 — see `memory/project_button_bits_pattern.md` for the
full pattern:

1. Add `entvars_t.button3` / `entvars_t.button4` (`game_types.h`).
2. Add fields to `pr_edict.c`'s `NF_FLOAT` table so saves/loads round-trip.
3. Pack the bits in `cl_input.c::CL_SendMove`.
4. Unpack in `sv_user.c::SV_ReadClientMove`.
5. Bump `GAME_API_VERSION` (the entvars layout changed).
6. Bind the cmds in `default.cfg` (`q` → `+blink`, `f` → `+gust`).

The MCP server's `console_exec` tool can also issue `+blink`/`+gust`, which
fans out through the same console-command path.

## Save games

Save files (`.sav`) are written by `Host_Savegame_f` and live in `id1/`.
Format is plain-text Quake script: a leading version line, comment, then
the global state (light-styles, parm[16]) followed by `ED_Print` of every
edict. Round-trips through `pr_edict.c`'s field tables, so adding a new
edict field requires updating `NF_*` tables to make it persist.

`autosave.sav` is written automatically at level start by the host.

## .cfg files

Console scripts. `default.cfg` (in `pak0.pak`) sets up the stock keybinds;
`config.cfg` is overwritten by the engine on shutdown with the current
binds, cvar values, and aliases. Hand-written `.cfg` files can be exec'd
via the console (`exec foo.cfg`) and behave like a stream of typed
commands.

## Demos (.dem)

Recorded with `record <name> <map> [skill]`. Format:

```
<ascii forced-cdtrack>\n
<repeat>
  int32 message length
  float yaw, pitch, roll   // viewangles at recording time
  byte  message_data[length]   // raw svc_* stream as the client saw it
<end>
```

Playback (`playdemo <name>`) replays the message stream into the client
parser with no server side at all — the recorded svc messages drive the
entire renderer.
