# QuakeC → C Port Audit

Function-level diff between the original Quake-1 v101 QuakeC source
(`Quake-Tools-master/qcc/v101qc/*.qc`) and the C port in `sdlquake/game/`.

Method: extracted function definitions from each suspect QC file and matched
against function definitions in the corresponding C file. False alarms from
the LOC-based audit (e.g. wizard `−121 lines`) turned out to be QC's denser
frame-state syntax compressing into C macros, not missing logic.

## Confirmed gaps (real fixes needed)

All six bugs below share the same failure mode: a `__attribute__((weak))`
stub was declared in one translation unit, but the strong override that
was supposed to live in another file was either missing entirely, named
differently (`kn_atk1` vs `knight_atk1`), declared `static` (internal
linkage doesn't override the weak symbol), or had a mismatched signature.
None of these produce compile errors — the code links to the no-op stub
and the feature silently doesn't work in-game.

### 1. `WizardCheckAttack` permanent zero stub  →  wizards never attack [FIXED]

**File:** `sdlquake/game/ai.c:35`

```c
__attribute__((weak)) int WizardCheckAttack(void) { return 0; }
```

Was the only definition. Dispatch in `ai.c:351` (`CheckAnyAttack` →
`WizardCheckAttack`) always returned 0, so the scrag/wizard's attack-state
was never set. Comment block at `monster_wizard.c:61` ("`// ---
WizardCheckAttack and WizardAttackFinished ---`") implied both would be
defined but only `WizardAttackFinished` actually was.

**Fix applied:** ported `wizard.qc:107-179` (`WizardCheckAttack`) into
`monster_wizard.c` with external linkage (no `static`) so it overrides the
weak stub. Verify on `e2m2` (Ogre Citadel) or `e1m4` (Grisly Grotto) —
scrags should swoop and fire spike volleys.

### 2. `DemonCheckAttack` permanent zero stub  →  fiends never attack [FIXED]

**File:** `sdlquake/game/fight.c:35`

```c
__attribute__((weak)) int DemonCheckAttack(void) { return 0; }
```

Same bug class as wizard. `monster_demon.c` had the frame functions and
`Demon_MeleeAttack` (set as `th_melee`), but `DemonCheckAttack`,
`CheckDemonMelee`, `CheckDemonJump` were all missing. Without them the
demon never enters AS_MELEE/AS_MISSILE state, so `ai_run` never invokes
`th_melee` or `th_missile` — the fiend just runs in circles.

**Fix applied:** ported `demon.qc:250-315` (`CheckDemonMelee`,
`CheckDemonJump`, `DemonCheckAttack`) into `monster_demon.c` near
`Demon_JumpTouch`. `DemonCheckAttack` has external linkage to override the
weak stub. Verify on `e1m5` (Gloom Keep) or `e1m6` (The Door To Chthon) —
fiends should leap and slash.

### 3. `knight_atk1` / `knight_runatk1` weak stubs  →  knight frozen on contact [FIXED]

**Files:** `sdlquake/game/fight.c:28-29` (weak stubs);
`sdlquake/game/monster_knight.c` (defined attack frames as `kn_atk1` /
`kn_runatk1` — different name, also `static`).

`fight.c:55-57` calls `knight_atk1` / `knight_runatk1` *by name* (special-
case dispatch for monster_knight to choose between melee and running
attack based on distance). Both name-resolved to the no-op weak stubs, so
on melee contact `CheckAttack` returned 1 (claiming the attack started)
but no animation, charge, or damage actually fired. `ai_run` then returned
early without `MoveToGoal`, leaving the knight idling-in-place at melee
range. User-reported as "knight pathfinding doesn't seem too good" — the
real symptom was attack-state stall, not navigation.

**Fix applied:** added `void knight_atk1(edict_t *self) { kn_atk1(self); }`
and matching `knight_runatk1` wrappers in `monster_knight.c` to bridge
the name gap. Verify on `e1m3` (Necropolis) — knights should lunge with
overhead sword swings on approach.

### 4. `LaunchLaser` weak stub  →  trap_shooter laser traps silent [FIXED]

**Files:** `sdlquake/game/misc.c:22` (weak stub, 2-arg);
`sdlquake/game/monster_enforcer.c:76` (was `static`, 3-arg with explicit
`owner`).

`misc.c:250` (`spikeshooter_use` for `trap_shooter` with `LASER` spawnflag)
called `LaunchLaser(self->v.origin, self->v.movedir)` — 2-arg, hit the
weak stub, did nothing. The enforcer's static 3-arg `LaunchLaser` was
unrelated due to the signature mismatch.

**Fix applied:** changed `monster_enforcer.c` `LaunchLaser` to the QC-
matching 2-arg signature and removed `static`; uses `g->self` as the
owner. `enforcer_fire` updated to set `g->self = e` before the call.
Trap_shooter laser variants (e.g. parts of `e2m6`, `e3m4`) should now
fire enforcer-style lasers.

### 5. `bubble_bob` weak stub  →  player bubbles don't bob [FIXED]

**Files:** `sdlquake/game/player.c:145` (weak stub);
`sdlquake/game/misc.c:333` (was `static`).

`player.c:363` assigned `bubble->v.think = bubble_bob` for the player's
underwater air bubbles. Resolved to the weak stub — bubbles spawned but
their think function was a no-op, so they sat motionless instead of
floating up with their characteristic random wobble.

**Fix applied:** removed `static` from misc.c's `bubble_bob` definition
and forward decl. Cosmetic, but visible whenever the player is underwater.

### 7. `t_movetarget` end-of-path null-check wrong  →  monsters walk toward map origin at end of patrol [FIXED]

**File:** `sdlquake/game/ai.c:80` — `if (!next)` checked for C `NULL`, but
the engine's `ED_Find` (`engine_ed_find` in `sdlquake/engine/hotreload.c:191`)
returns `g->world` (the world entity) on no match, NOT `NULL` — matching
the original Quake `find()` builtin convention.

When a walking monster reached the *last* `path_corner` in a non-looping
patrol route (one with no `target` field), the code tried to look up the
non-existent next corner, got `g->world` back, the `!next` check fell
through, and `ideal_yaw` was set to `VectorToYaw(g->world.origin -
self.origin)` — a yaw vector pointing at map origin `(0,0,0)`. The
monster then kept walking in that direction instead of stopping.

User-visible: "demon doesn't face the correct direction while patrolling."
Most apparent on demons (large bbox, fast walk), but affects every
walkmonster on a non-looping path.

**Fix applied:** check `next == g->world` after `ED_Find`, and short-
circuit the find with `corner->v.target ? ED_Find(...) : g->world` to
avoid feeding NULL into the field search. Matches the QC convention.

**New failure mode pattern to grep for in future audits:** QC's
`if (!entity_var)` (where `world` is treated as falsy) translates to C
`if (entity_var == g->world)`, NOT `if (!entity_var)`. The C NULL check
silently fails to fire and the code falls through. `walkmonster_start_go`
already used the right idiom (`!= g->world`); only `t_movetarget` had it
wrong. No other instances found in the rest of `sdlquake/game/`.

### 6. `spawn_tfog` / `spawn_tdeath` weak stubs  →  DM respawn fog/telefrag broken [FIXED]

**Files:** `sdlquake/game/client.c:37-38` (weak stubs);
`sdlquake/game/triggers.c:212,244` (were `static`).

`client.c:426,429` (`PutClientInServer`) called these on every player
spawn — the fog effect for DM/coop visible-respawn and the telefrag
trigger for spawning on top of another player. Both resolved to the weak
stubs. Singleplayer-irrelevant; DM-only.

**Fix applied:** removed `static` from triggers.c's two definitions.

## Intentional cuts (deathmatch/multiplayer)

These QC functions are not ported. Singleplayer Quake doesn't need them, so
they're acceptable as cuts — but document for clarity:

| QC function | QC location | C status | Why skipped |
|---|---|---|---|
| `CheckSpawnPoint` | `client.qc:403` | absent | DM spawn-point validity helper used by `SelectSpawnPoint`'s DM path. `SelectSpawnPoint` in `client.c:327` may need spot-check that the singleplayer path doesn't accidentally call it. |
| `PrintClientScore` | `client.qc:583` | absent | Console score listing for `impulse 28` / `dumpscore` cheat. Out-of-band debug. |
| `DumpScore` | `client.qc:600` | absent | As above. |

If any of these gets wanted later, port from `client.qc` and add a flag in
`spawn.c` if it needs an entity classname.

## False alarms (refactored, not missing)

The first audit's "100% complete" verdict was over-confident on totals but
*right* about most named functions. Apparent gaps from LOC drops were:

| Apparent gap | Actual location | Notes |
|---|---|---|
| `LaunchMissile` (wizard.qc:65) | folded into `monster_wizard.c:155` Wiz_FastFire | uses `launch_spike` from `weapons.c:588` |
| `ChangeYaw` (ai.qc:243) | engine-side `eng->SV_ChangeYaw` | `ai.c:300, 361, 374, 387`; `fight.c:71, 144` |
| `movetarget_f` (ai.qc:94) | inlined into `spawn_path_corner` (`ai.c:97`) | sets touch handler directly, no helper needed |
| `FindIntermission` (client.qc:115) | `client.c:67` (`static edict_t *FindIntermission(void)`) | regex missed `edict_t *FuncName` style |
| `SelectSpawnPoint` (client.qc:415) | `client.c:327` | as above |
| `spawn_field` (doors.qc:291) | `doors.c:213` | as above |
| `spawn_tfog`, `spawn_tdeath` | `monsters.c` (referenced from `client.c:426, 429`) | shared monster-spawn helpers |

## Weak stubs (all overridden — confirmed working)

These appear as `__attribute__((weak)) FuncName(...) {}` and are properly
overridden by strong definitions elsewhere — they are the C equivalent of
QC's forward-declared mutual references and are fine:

| Weak stub | Strong override |
|---|---|
| `combat.c:17` `monster_death_use` | overridden in `monsters.c` |
| `combat.c:18` `FoundTarget` | `ai.c:213` |
| `client.c:33` `W_WeaponFrame` | `weapons.c:920` |
| `client.c:34` `W_SetCurrentAmmo` | `weapons.c:654` |
| `ai.c:36` `DogCheckAttack` | `monster_dog.c:256` |
| `items.c:16` `W_BestWeapon` | `weapons.c:719` |
| `subs.c:200` `visible` | `ai.c:127` |
| `misc.c:21` `enforcer_attack` | `monster_enforcer.c` |
| `fight.c:27, 37` (knight, ogre, shalrath check-attack stubs) | each monster file |
| `player.c:146-150` (`DropBackpack`, `W_FireAxe`, `W_FireLightning`) | `items.c:897`, `weapons.c:95, 440` |

The exception is `ai.c:35` `WizardCheckAttack` — see "Confirmed gaps" above.

## Out of scope

- `weapons_phase6.c:122-130` — 9 empty `W_FirePhase6_*` bodies. Phase 6
  Doom/Wolf3D weapons not yet implemented. Tracked under Phase 6, not Phase 5.
- `game_main.c:28` `game_shutdown` — intentional empty hook for the
  hot-reload DLL ABI.
- `defs.qc`, `models.qc`, `amtest.qc`, `jctest.qc`, `sprites.qc` — type
  declarations / precaches / dev test files. Precaches are wired into
  `world.c` worldspawn.

## Spawn-table cross-check

`sdlquake/game/spawn.c` registers 89 classnames covering every monster,
item, weapon pickup, light entity, ambient, trigger, door/plat/button,
and player-start variant from the QC. Spot-checked: `monster_tarbaby` (the
QC defines `monster_tarbaby`, registered correctly).

`weapon_axe` and `weapon_shotgun` are intentionally absent — the player
starts with both via `IT_AXE | IT_SHOTGUN` in `PutClientInServer`; they
are not world-spawnable pickups in stock Quake.

## Summary

- **7 real bugs found and fixed**:
  1. `WizardCheckAttack` zero-stub → wizards didn't attack.
  2. `DemonCheckAttack` zero-stub → fiends didn't attack.
  3. `knight_atk1`/`knight_runatk1` weak stubs (fight.c special-case caller
     resolved them by name to no-ops) → knights frozen at melee range.
  4. `LaunchLaser` signature mismatch + `static` → `trap_shooter` lasers
     silent.
  5. `bubble_bob` `static` → player underwater bubbles motionless.
  6. `spawn_tfog`/`spawn_tdeath` `static` → DM/coop respawn fog and
     telefrag damage broken.
  7. `t_movetarget` `if (!next)` instead of `next == g->world` →
     end-of-path monsters walked toward map origin instead of stopping.
- **3 deathmatch-only helpers** intentionally cut from `client.qc`
  (`CheckSpawnPoint`, `PrintClientScore`, `DumpScore`) — acceptable for SP.
- **All other "missing" functions** are refactors or weak-stub overrides
  that *are* properly paired — verified by grep against the C source.

### Lessons / preventive recommendation

Two distinct silent-failure patterns surfaced in this audit:

**Pattern A — weak-stub override mismatches** (bugs 1–6). The
linker accepts the weak stub as the resolved symbol when the strong
override has any of these problems:

| Failure mode | Example |
|---|---|
| Override never written | `WizardCheckAttack`, `DemonCheckAttack` |
| Override has wrong name | `kn_atk1` vs `knight_atk1` |
| Override has `static` (internal linkage) | `bubble_bob`, `spawn_tfog`, etc. |
| Override has different signature | `LaunchLaser` 3-arg vs 2-arg weak stub |

A ~50-line build script using `nm`/`objdump` against the linked
`game.dll` could enumerate weak symbols and verify each has a matching
strong override of the right signature. Worth doing — six of these
slipped through.

**Pattern B — QC `world`-as-falsy idiom** (bug 7). In QC, `if (!entity_var)`
returns true when `entity_var == world` (entity 0). In our C port,
`engine_ed_find` and other engine builtins return `g->world` (a valid
non-NULL pointer) on no-match. So the QC pattern translates to
`if (entity_var == g->world)`, NOT `if (!entity_var)`. The latter
silently never fires.

A targeted grep for `if\s*\(\s*!\s*\w+_ent\b|if\s*\(\s*!\s*\w*entity\w*\b`
in `sdlquake/game/*.c` would catch any remaining instances. As of this
audit, `t_movetarget` was the only one; `walkmonster_start_go` already
used the correct idiom (`!= g->world`). The grep is cheap to run during
review of any new QC→C port work.
