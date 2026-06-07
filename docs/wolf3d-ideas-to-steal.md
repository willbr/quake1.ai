# Wolf3D mechanics worth porting into Quake

Mined from the id Wolfenstein-3D source (`ref/wolf3d-master/WOLFSRC/`) before that
reference tree was removed from the repo (2026-06-07). The Wolf3D AI is a tiny,
elegant 4-state machine with several cheap, high-leverage tricks that complement
this project's immersive-sim layer (reactive AI FSM, stimulus bus, navmesh,
fire/oil, wind/smoke, M5 light tiers). Line numbers refer to the original WOLFSRC
files as they existed in `ref/wolf3d-master/` at deletion time.

## Top picks (most portable / highest payoff)

### 1. Area/door sensory connectivity — "close the door to lose them"
`WL_ACT1.C:308 ConnectAreas` (flood from `RecursiveConnect`, `WL_ACT1.C:293`),
gated in `WL_PLAY.C:1264 DoActor`, `WL_STATE.C:1194 CheckSight`, `T_Shoot:3451`.

The map is partitioned into numbered rooms ("areas"). Opening a door
increments `areaconnect[a][b]`; any door open/close re-floods from the player's
area into `areabyplayer[]`. Monsters in an area **not** connected to the player
don't see, don't hear, and **don't even think**. Closing a door behind you
literally severs sight + sound + simulation in ~30 lines.

Quake already has PVS / areaportals (`func_door` splits vis). Expose door/leaf
connectivity to the AI: suppress a monster's sight/sound stimulus when the player
is in a vis-disconnected leaf, and slamming a door drops you off its sensor grid.
Biggest single stealth payoff; maps directly onto existing engine structure.

### 2. Per-class reaction delay — the "double-take" on first sighting
`WL_STATE.C:1404 SightPlayer` (`ob->temp2 = 1+US_RndT()/4` for guards, `2` for
officers, `1+US_RndT()/6` for SS/mutants, `1` for bosses; counted down 1409–1418).

On first detection an enemy does **not** insta-attack — it keeps idling for a
randomized, per-class countdown, giving the player a duck-back window. This is a
tunable gate on the existing `SUSPICIOUS → COMBAT` edge: a per-monster-class
"reaction time." Officers reacting 2× faster than guards reads as competence and
is a free difficulty/character lever. Bosses react instantly.

### 3. Dodge-aware enemy hit chance — "you can dodge what you can see"
`WL_STATE.C:3444 T_Shoot`:
```c
if (thrustspeed >= RUNSPEED)
    hitchance = ob->flags&FL_VISABLE ? 160-dist*16 : 160-dist*8;
else
    hitchance = ob->flags&FL_VISABLE ? 256-dist*16 : 256-dist*8;
```
Enemy to-hit drops with distance, drops more when the player is moving fast, and
drops **further** when the enemy is on-screen (`FL_VISABLE`). The on-screen
penalty is the subtle bit: enemies you face are deliberately less accurate
(rewards confronting threats); off-screen flankers are deadlier (rewards
situational awareness). SS/bosses get an effective range bonus (`dist = dist*2/3`).
Turns strafing into a real defensive skill — drop-in for hitscan grunts.

### 4. Self-respawning dormant ambusher (the Spectre "fake death")
`WL_ACT2.C:1934 A_Dormant`, state chain `s_spectredie1..4 → s_spectrewake`
(`WL_ACT2.C:1899`). On "death" it fades invisible, waits ~300 tics, then checks
the player is far enough and no actor/wall is adjacent, and **re-materializes** as
a fresh `FL_AMBUSH` enemy in place.

Reusable "phasing ambusher" archetype. Tie to the M5 light-tier system → a
shadow-wraith that only re-forms in darkness: `th_die` enters a `SOLID_NOT`
invisible dormant think that re-spawns when the player leaves its area and the
tile is in shadow. Flagship immersive-sim enemy built entirely from systems that
already exist here.

### 5. Backstab damage bonus
`WL_STATE.C:971`: `if (!(ob->flags & FL_ATTACKMODE)) damage <<= 1;`

One line: pre-combat (IDLE/SUSPICIOUS) enemies take 2× damage. Instant
stealth-takedown incentive — drop into `T_Damage`.

## Honorable mentions

- **Deaf "ambush" guards** — `WL_STATE.C:1424`. `FL_AMBUSH` flag = wakes only on
  direct line-of-sight, ignores sound entirely. Trivial add to the sense filter;
  gives dormant ambushers for free.
- **Dodge-weave movement** — `WL_STATE.C:359 SelectDodgeDir` vs `475
  SelectChaseDir`. Enemies build 5 ranked candidate dirs and randomly swap
  priority pairs so they weave sideways while still closing, instead of beelining;
  may turn around only once, on `FL_FIRSTATTACK`. Add a randomized lateral bias to
  the A* line when a charging monster has LOS.
- **Contact-only pursuer (Pac-Man ghosts)** — `WL_ACT2.C:3207 T_Ghosts`. Relentless
  `SelectChaseDir`, no ranged attack, lethal on touch — "evade, don't fight."
  Pairs with the Blink ability (M3): break LOS / close a door / Blink past it.
- **Boss patterns** — kite-away-while-firing (`WL_ACT2.C:2632`, `dist<4 →
  SelectRunDir`); angle-jittered projectile spread (`1636 T_Launch`, ±4 alternating);
  death-morph into a harder form (`2886 A_HitlerMorph`: `KillActor` then spawn a
  faster entity at the same origin).
- **Chase-fire probability scales inversely with distance** — `WL_ACT2.C:3084`,
  `chance = (tics<<4)/dist`; point-blank ≈ guaranteed. "Press harder when close."
- **Difficulty via tile-code fallthrough** — `WL_GAME.C:316 ScanInfoPlane` (`tile
  -= 36;` case-fallthrough chain). One map encodes 3 enemy densities at zero
  runtime cost. Per-class HP in `starthitpoints[4][NUMENEMIES]`
  (`WL_ACT2.C:42`) — note trash-mob HP is constant across difficulty; only bosses
  get spongier and more bodies appear. Difficulty = more bodies + better aim, not
  stat inflation.
- **Completionist scoring** — `WL_INTER.C:652` kill%/secret%/treasure% with a flat
  `PERCENT100AMT` bonus at exactly 100% per category, plus par-time bonus. Totals
  accumulated at spawn (`killtotal++`, `treasuretotal++`, `secrettotal++`). Whole
  system is ~50 lines.
- **Context-sensitive loot drops** — `WL_STATE.C:815 KillActor`. Bosses drop no
  ammo, troops do; SS drop a machinegun only if the player lacks one (line 845).
- **Area-gated thinking as a free AI LOD** — `WL_PLAY.C:1264`. Dormant actors in
  disconnected areas don't tick at all (perf + the stealth gate, same mechanism).
- **Pushwall secret + tracked secret count** — `WL_ACT1.C:732 PushWall`, `809
  MovePWalls`. `+use` slides a wall tile 2 cells; the `secretcount/secrettotal`
  bookkeeping is the part to lift. `func_door`/`func_wall` already exist here.

## Top 3 to port first
1. **#1 area/door sensory connectivity** — maps onto Quake vis/areaportals; "close
   the door to lose them," biggest stealth win.
2. **#2 + #3 reaction delay + dodge-aware hit chance** — turns hitscan FSM enemies
   from instant-lasers into readable, dodgeable, per-class-tuned threats.
3. **#4 self-respawning dormant ambusher** tied to **M5 light tiers** — a
   shadow-wraith that re-forms in darkness, built from existing systems.
