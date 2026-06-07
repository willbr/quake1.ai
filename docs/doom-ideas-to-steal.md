# Doom mechanics worth porting into Quake

Salvage notes taken before deleting the `ref/DOOM-master/` source tree
(linuxdoom-1.10), mirroring `docs/wolf3d-ideas-to-steal.md`. The Phase 6 Doom
*guns* were already ported from this source; this doc captures the **systemic**
mechanics — AI, stimulus, environment, powerups — that would add emergent depth
on top of this fork's immersive-sim substrate (stimulus bus, FSM + navmesh AI,
fire/oil propagation, light-tier stealth, wind/smoke, Blink + Gust).

All file/line references are to `ref/DOOM-master/linuxdoom-1.10/` and were
verified against the tree at removal time (2026-06-07); line numbers are for
orientation, not exact pins.

## Top picks (highest payoff / best fit for this fork)

### 1. Infighting — monsters turn on whoever hurt them

`p_inter.c:894-911` (`P_DamageMobj`). When a monster takes damage from a
*different-species* source, it retargets the attacker:

```c
if ((!target->threshold || target->type == MT_VILE)
    && source && source != target && source->type != MT_VILE) {
    target->target = source;        // p_inter.c:910
    target->threshold = BASETHRESHOLD;
}
```

The species check (`source->type != target->type` for the projectile case) is
what makes a stray rocket turn an ogre against a nearby grunt. This is the
single highest-payoff steal: it opens a whole second combat layer where crossfire
is a weapon.

**Fit here:** the stimulus bus already carries damage events — they just need
**source/inflictor attribution**. When a monster's FSM processes a `STIM_DAMAGE`
whose source is another monster of a different class, switch its target. The
fire system already wrestles with igniter attribution (see
`docs/phase8-fire-oil.md` — contact-spread "never credits the burning monster"),
so the plumbing for source-credit is half-built. Quake's monsters have only
vestigial infighting; this is mostly new.

### 2. Sound propagation that wakes monsters spatially

`p_enemy.c:106-166` (`P_RecursiveSound` / `P_NoiseAlert`). Firing a weapon floods
sound through the sector graph: each two-sided line propagates the alert to the
neighbour sector, and a line flagged `ML_SOUNDBLOCK` (`p_enemy.c:141`) stops it
after one hop. Every monster in a reached sector gets its `soundtarget` set
(`p_enemy.c:123`); idle monsters pick it up in `A_Look` (`p_enemy.c:609`) unless
they're `MF_AMBUSH` (deaf). Range is *topological*, not radial — a gunshot in a
sealed room doesn't leak.

**Fit here:** this is the missing half of the stimulus bus. Today sound stimuli
are emitted; Doom's lesson is that **propagation should follow connectivity, not
a radius**. Walk the navmesh graph (or do LOS/portal hops) instead of
`P_RecursiveSound`'s sector recursion, with a per-edge attenuation that mimics
`ML_SOUNDBLOCK`. Makes silenced weapons, closed doors, and "fire into a wall to
deaden it" mechanically real — and it dovetails with the existing
`Wind_PathOcclusion` LOS work.

### 3. Deterministic RNG table (game vs. cosmetic split)

`m_random.c:31-72`. A fixed 256-byte `rndtable[]` with **two** cursors:
`P_Random` (`prndindex`) for game-affecting rolls and `M_Random` (`rndindex`)
for cosmetic ones, so visual jitter never desyncs gameplay. `P_ClearRandom`
resets both to 0 at level start (`m_random.c:71`).

**Fit here:** this fork has a headless bot and `scripts/run_ai_tests.sh`.
Swapping the platform-seeded RNG for a fixed table behind a "deterministic mode"
flag buys **reproducible bot runs, regression-stable combat, and demo
playback** — infrastructure value out of proportion to its ~40 lines. Keep the
game/cosmetic cursor split so particle/blood jitter stays free-running.

### 4. Pain chance — probabilistic flinch / stun-lock

`p_inter.c:894-900` + `p_enemy.c:1577` (`A_Pain`). Each monster type carries a
`painchance` (0-255). On damage, `if (P_Random() < painchance)` it enters its
short pain state and sets `MF_JUSTHIT` to swing back on recovery. Tanky monsters
(Baron, 50) barely flinch; weak ones (Zombieman, 200) stagger constantly, so
sustained fire stun-locks them.

**Fit here:** add a per-class `pain_chance` and a brief FSM "flinch" state that
suspends attacks for a few ticks. Instantly makes high-burst weapons feel like
stun tools and chip weapons feel like suppression — combat texture Quake lacks.
Tiny change, big feel.

### 5. Target memory / threshold — pursuit momentum after losing sight

`p_enemy.c:680-690` + `BASETHRESHOLD` (100 tics). A monster that acquires a
target keeps `threshold` ticking down; while it's non-zero the monster *chases
even without line of sight* and ignores new target-switch stimuli. Break sight
and you're not instantly safe — it remembers for ~3 seconds.

**Fit here:** a per-monster "target lease" countdown layered on the FSM. Raises
the stakes of breaking LoS in the stealth loop: pillar-hugging buys time, not
instant invisibility. Also gates infighting (#1) cleanly — a monster mid-lease
won't flip targets on a glancing hit.

### 6. Counter-stealth & stealth powerups that invert the light-tier system

Three timed powerups (`powers[]` array, decremented in `P_PlayerThink`,
`p_user.c:339-381`) interact directly with this fork's systems:

- **Light-amp goggles** (`pw_infrared`, `p_user.c:373-377`): forces
  `fixedcolormap = 1` → full-bright vision regardless of ambient light. A
  **direct counter to light-tier stealth** — give it to certain enemies and dark
  corners stop being safe, or hand it to the player to read a pitch-black room.
- **Partial invisibility** (`pw_invisibility` + `MF_SHADOW`): the monster aim
  fuzz at `p_enemy.c:794-796` adds `(P_Random()-P_Random())<<21` to the firing
  angle, so shots scatter. A stealth powerup that degrades enemy *accuracy*
  rather than detection — distinct from light-hiding.
- **Radiation suit** (`pw_ironfeet`): immunity to damaging-floor sectors. Maps
  straight onto the fire/oil hazard layer — a fireproof-suit pickup for
  fire-heavy encounters, with the iconic green screen tint as the HUD tell.

**Fit here:** the light-tier and fire systems already compute exactly the state
these powerups would override; they're cheap to bolt on and each one *reshapes*
an existing system rather than adding an isolated buff.

### 7. Dynamic sector lighting coupled to stealth

`p_lights.c`: strobe (`T_StrobeFlash`, :153), random flicker (`T_LightFlash`),
fire-flicker (`T_FireFlicker`, :43), and smooth glow (`T_Glow`, :314) all animate
sector brightness between a min/max derived from neighbours.

**Fit here:** this fork already pushes per-texel **lightmap deltas** (fire light,
M5) and reads brightness for the **light-tier stealth** check. Animated lights
turn a room into a *visibility oscillator*: a strobing corridor periodically
exposes you; `T_FireFlicker` is the natural model for the oil-fire glow that's
currently a static delta. The coupling — animated light that genuinely changes
whether the AI can see you — is the emergent payoff Quake's static lightmaps
can't give.

## Honorable mentions

- **Radius damage: LOS gating + linear falloff** (`p_map.c:1191-1194`,
  `P_RadiusAttack` → `PIT_RadiusAttack`). Splash damage is gated by
  `P_CheckSight(thing, bombspot)` so explosions don't hurt through walls, with
  linear `bombdamage - dist` falloff. This fork's `T_RadiusDamage` (combat.c,
  already hooked for oil ignition) uses half-distance falloff and **no** LoS
  gate — adding an `SV_Traceline` check is a correctness win, especially with
  barrels/oil chains in the mix.
- **Telefrag on Blink** (`p_map.c:79-105`, `PIT_StompThing`). A teleport stomps
  whatever occupies the destination for 10000 damage. The Blink ability currently
  box-traces the destination; gibbing an occupant (behind a `ph_blink_crush`
  cvar) is the iconic, expected behaviour.
- **Material-reactive hit feedback** (`p_map.c:1003` + `p_mobj.c:807-859`).
  `MF_NOBLOOD` decides puff (wall/metal) vs. blood (flesh), and blood frame is
  **damage-scaled** (`p_mobj.c:855`). A natural extension of the fork's
  material-aware direction (oil-coated → ignite instead of bleed).
- **Mass-aware directional knockback + decaying damage tint**
  (`p_inter.c:806-832`). Knockback thrust scales by `damage*100/mass` along the
  attacker→victim angle; the red `damagecount` tint *decays per-frame* rather
  than a binary flash. Richer than the fork's current fire red-flash — heavy
  burns could glow red and fade.
- **Tag-based multi-target activation** (`p_spec.c:P_FindSectorFromLineTag`).
  One trigger affects *every* sector sharing a numeric tag — area-centric and
  multi-target, vs. Quake's one-to-one `targetname`/`target`. Worth exposing in
  the in-game map editor for synchronized door/light/lift puzzles.
- **Crushers / gib-on-crush** (`p_ceilng.c:T_MoveCeiling`, `EV_DoCrusher`). A
  closing ceiling that damages and reverses on contact; corpses can jam it. A
  classic environmental hazard the brush-entity system could grow.
- **Reaction time before ranged fire** (`reactiontime`, suppresses ranged
  attacks in `P_CheckMissileRange`, `p_enemy.c:212`). A freshly-alerted monster
  delays ~0.2 s before its first shot — a small "double-take" that gives the
  player a fair beat, and pairs well with the FSM's SUSPICIOUS state.

## Top 3 to port first

1. **Infighting (#1)** — the biggest emergent-gameplay return, and most of the
   work is already implied by the stimulus bus's damage events. Just needs source
   attribution + a species check.
2. **Deterministic RNG table (#3)** — pure infrastructure: it makes the existing
   headless bot and AI-test rig *reproducible*, paying for itself every test run,
   independent of any gameplay change.
3. **Sound propagation alerting (#2)** — completes the stimulus/stealth loop this
   whole fork is built around, turning weapon noise and closed doors into real
   mechanics. Reuse the navmesh graph + the existing LOS/occlusion work.
