#ifndef ACTOR_TEST_H
#define ACTOR_TEST_H

#include "game_types.h"

// Skeletal-actor in-game integration test (C1-lite). Proves the procedural
// expression (look-at / breathing / ponytail — all client-side, no protocol)
// works on a real server-spawned networked entity, not just the engine
// dev-spawn (actor_spawn).
void Actor_TestPrecache(void);              // call from worldspawn (level init)
void Actor_TestDebugSpawn(edict_t *player); // impulse 217: spawn an IQM actor ahead

#endif /* ACTOR_TEST_H */
