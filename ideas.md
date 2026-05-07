# Ideas

## Rendering & visuals

* decals — bullet holes, blood, dirt & grime, scratches
* coloured lights
* fog
* texture filtering
* model interpolation
* more particle effects
* dynamic player shadow (Mario 64 blob / stencil) for grounding in dark levels
* vertex lighting & smooth shading (N64-style soft gradients on walls and characters)
* simple real-time shadows projected from monsters and props
* vibrant palette + atmospheric distance fog (Star Fox 64, Zelda)
* billboarded particles & sprites — explosions, blood, dust, lens flares
* texture animation & environment mapping for water, lava, metal
* low-poly charm enhanced with normal maps or baked lighting
* dynamic lighting effects — coloured lights, flickering torches, muzzle flashes
* review the software renderer — what cool things could we add?

## Debug rendering

* flat-shaded mode (no texturing, solid face colours)
* no-lighting / fullbright mode
* wireframe overlay (edges only, or edges over solid)
* BSP leaf / node boundaries
* PVS visualisation for current leaf
* overdraw heatmap (how many times each pixel was written)
* texture-checker mode (lightmap-only, mip-level colour, UV grid)
* surface normal visualisation
* per-face poly count / cost coloring

## Debug gizmos (entities & AI)

* monster patrol paths, current goal, line-of-sight cones
* pathfinding waypoints and current edge
* trigger volumes and their targets, drawn as labelled boxes
* entity bounding boxes, hull boxes, hitboxes
* velocity vectors and predicted trajectories (rockets, grenades, gibs)
* sound emitter ranges as wireframe spheres
* trace lines for the last N hitscan shots
* link arrows for `target` / `targetname` relationships
* spawnflags / classname labels floating over each edict
* health / AI-state HUD per visible monster (idle / hunt / attack)

## Engine & platform

* uncapped framerate with fixed physics
* increased engine limits
* BSP2 support
* what SDL3 features should I use?

## Environmental fx

* wind that blows smoke, plants, rain
* permanent damage to a level (broken crates stay broken, corpses stay)
* make the world more interactive

## UI & HUD

* mouse support for the default menu
* rebuilt main menu in imgui (dev overlay already done)
* custom HUD

## Editor & live workflow

* live `.map` editing
* live entity editing
* brush boolean ops — subtract, intersect, hollow
* live texture painting (paint lightmap or diffuse onto a face, hot-reload)
* undo / redo stack with branch points
* diff view — highlight what changed since last compile
* screenshot regression testing (golden frames per map)
* light baking on a worker thread, progressive refinement
* prefab library, drag-and-drop entity templates

## Game logic / scripting

* random / procedural maps
* procedural everything — models, textures, music, story

## AI / LLM-native gameplay

* LLM-driven monster taunts that react to *how* you fight (camping, rocket-jumping, low health)
* talk-to-monsters mode — Undertale mercy route, Claude voices each shambler
* AI dungeon master that rewrites encounters mid-level based on player skill
* voice-controlled console (whisper → Claude → cvar / cmd)
* Claude proposes balance patches from your last session's telemetry, you accept / reject
* AI-generated flavor text for items, signs, and corpses
* procedural side-quests injected mid-playthrough ("find the rune before the next slipgate")
* AI director like Left 4 Dead

## MCP server extensions

* `record_demo` / `replay_demo` / `scrub_to_tick`
* `rewind` — time-travel debugger via periodic state snapshots
* `screenshot` tool returning PNG bytes so Claude can *see* the game
* `set_player_pos` / `spawn_entity` / `damage_entity` for scripted test scenarios
* A/B harness — run two `game.dll` builds side-by-side, compare metrics
* expose the imgui console log as an MCP resource (live tail)

## Modding ecosystem

* multi-DLL plugin loader (load N `game.dll`s, each owning a subsystem)
* mod browser fetching from a URL or git repo
* in-game Lua sandbox for quick scripting without a rebuild
* per-map cvar profiles (auto-applied on map load)
* asset hot-reload for textures / sounds / models, not just code

## Renderer experiments (beyond the software look)

* optional GPU path that emulates the 8-bit palette + dither exactly
* CRT / scanline / chromatic-aberration post FX
* VR mode (SDL3 GPU API; OpenXR layer)
* path-traced reference renderer for screenshot comparisons
* HDR bloom that respects the palette
* stereoscopic 3D / anaglyph

## Audio

* reverb zones tied to BSP leafs
* HRTF / binaural positional audio
* adaptive music (intensity tracks, combat state)
* procedural ambient soundscape per map theme
* TTS-voiced monsters (cached per line)

## Multiplayer / networking

* modern netcode — rollback + delta compression
* WASM web client (SDL3 builds for emscripten)
* spectator free-cam
* cooperative campaign
* speedrun leaderboards stored in SQLite

## Persistence & save

* changes to a level persist across saves
* named save slots, autosave tunables
* replay buffer (last 30s on demand, like ShadowPlay)
* cloud sync via plain HTTP
* SQLite for everything — what else could we replace with SQLite?
* log to SQLite

## Quality of life

* command palette (Ctrl+K) in the imgui console
* bookmark teleporters — drop a pin, jump back to it
* benchmarking tool with FPS-over-time graph
* modern keybinding UI in imgui
* per-weapon sensitivity

## Accessibility

* sound-effect subtitles
* colour-blind palettes for the HUD / blood
* aim assist toggle
* motion-sickness options (vignette, FOV scaling, head-bob off)
* difficulty modifiers exposed as cvars

## Speedrun / competitive

* input recording + ghost overlay
* bunny-hop / rocket-jump trainer mode
* movement physics tunables exposed in cvars
* segmented timer with split history

## Physics & destruction

* physics overhaul
* verlet ragdolls for monster deaths
* destructible BSP chunks
* cloth (banners, robes)
* buoyancy + swim dynamics
* rope / grappling hook

## New player skills & weapons

* blink (Dishonored teleport)
* throwable objects, including a throwable camera
* Prey gloo cannon to build platforms
* deployable turrets
* permanent weapon buffs
* crossbow
* axe — throw it, God of War style
* something interesting on mouse2

## Stealth gameplay

* smoke bombs
* stealth objectives, alert states

## Camera & perspective

* third-person camera (Last of Us, Assassin's Creed)
* limited vertical rendering like Doom 1 / Wolf 3D (for crossover maps)

## Genre experiments — change the game

* Souls-like
* XCOM
* Metroidvania
* Minecraft
* RTS (AoE2)
* Max Payne bullet time
* cat vs mouse — more monster-vs-monster fights

## Crossover — Wolf 3D × Doom × Quake

* mix guns, HUD, enemies, maps across the three
* progression map: start in Wolf 3D, transition into Doom, end in Quake
* limited vertical rendering for the Wolf / Doom segments
* port Wolf and Doom physics for those segments

* enemies
    * Wolf3D: SS, brown guard, dog, mutant, officer, Hans Grosse, Mecha-Hitler
    * Doom: imp (fireball), demon / spectre, cacodemon (flying), lost soul (charging), baron of hell, revenant (homing missiles), arch-vile (resurrects others)
    * port the AI patterns too — Wolf's grid-locked patrol nodes, Doom's see → chase → attack → pain FSM

* pickups & items
    * Wolf3D: treasure (cross / cup / chest / crown for score), dog food, food tray, first aid kit, gold / silver keys, extra life
    * Doom: med kit / stimpack, armor bonus, health bonus, soul sphere, megasphere, berserk pack, invulnerability, partial invisibility, radiation suit, computer map, light amp goggles, backpack
    * Wolf's score system + extra-life-at-N-points as a standalone mechanic

* HUD
    * Doom's status bar face that reacts to damage and direction of incoming fire
    * Wolf's BJ portrait that gets bloodier as you take damage
    * Doom's automap (with computer-map pickup to reveal everything)
    * Wolf's treasure / score / lives counter
    * key icons in the corner

* sound & music
    * Wolf3D Adlib music tracks (`wolf3d-data/AUDIOHED.WL1` etc.) — needs OPL emulation or pre-rendered
    * Doom MUS-format music via MUS→MIDI; Bobby Prince soundtrack
    * monster bark / death / pain sounds — these define the games' feel as much as the visuals

* decorations / props
    * Doom: barrels (explosive!), corpses, hanging bodies, candles, candelabra, tech columns, gore piles, evil eye, skull on stake
    * Wolf3D: lamps, dog food bowl, table & chairs, suit of armor, vases, skeleton in cage, well, barrels, plants
    * sprites with bounding boxes — easy port, big atmosphere payoff

* player movement
    * Doom's strafing-on-by-default and strafe-running bonus (SR40 / SR50)
    * Wolf's grid-locked turning vs Doom's free-look — expose as a movement-style cvar
    * Doom's separate "use" key for doors and switches (split from attack)

## Storytelling & atmosphere

* deeper storytelling — cinematics, voiced logs, lore (FF7, Metal Gear Solid)
* stronger atmosphere & horror — lighting, sound design, tension (Resident Evil, Silent Hill)
* humour & personality — Conker, Banjo-Kazooie tone
* Pixar-style story team for plot
* strong level themes & atmosphere — memorable, varied environments with great music

## Level design & exploration

* improved level design — exploration, verticality, secrets (Symphony of the Night, Tomb Raider)
* immersive open worlds — larger interconnected levels with secrets and progression (Ocarina, Majora's Mask)
* 3D platforming & movement — vertical exploration, wall jumps (Mario 64, Banjo-Kazooie)
* varied mission objectives — stealth, objectives, time-based challenges (Perfect Dark, GoldenEye)
* collectibles & replayability — stars, tokens, challenges (DK64, Mario 64)
* greater replayability — time attacks, scoring, extra modes (Tony Hawk, CTR)
* optional depth — RPG-lite, platforming, hub worlds without slowing core combat

## Controls & feel

* tighter controls & polish — responsive movement and weapon handling (Tekken 3, Tony Hawk's Pro Skater 2)
* tight first-person controls — smooth aiming, iron sights, console-style precision (GoldenEye, Perfect Dark)
* better visuals & effects — enhanced textures, particles, art direction (Gran Turismo, Crash Bandicoot)
* creative weapons & gadgets — remote mines, cloaking, special ammo (Perfect Dark, Star Fox 64)

## Multiplayer game modes

* multiplayer mayhem — split-screen deathmatch, bots, co-op (GoldenEye, Mario Kart 64)

## Content pipeline & CI

* BSP → glTF exporter
* MDL viewer panel in imgui
* PAK / WAD lump browser
* headless mode for batch testing / regression runs
* screenshot-diff CI on every commit

## Documentation

* docs on file formats
* docs on the code
