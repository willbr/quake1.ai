#ifndef IQM_DYNAMICS_H
#define IQM_DYNAMICS_H

#include "iqm.h"

// R4 client-side ponytail dynamics. Simulates the actor's ponytail chain and
// overwrites the simulated (free) joints' skin matrices in skin[] (actor space,
// bind->posed). curwld/bindinv hold the R3-posed joint world matrices and bind
// inverses (actor space). ent identifies the actor (per-actor sim state).
// a2w_rot columns are the entity actor->world rotation (alias_forward,
// -alias_right, alias_up); a2w_org is the entity world origin. now = cl.time.
// No-op if the actor has no ponytail or dynamics are disabled.
struct entity_s;
void IQM_SolveDynamics (struct entity_s *ent, lm_iqm_t *iqm,
		float curwld[][3][4], float bindinv[][3][4], float skin[][3][4],
		float a2w_rot[3][3], float a2w_org[3], float now);

void IQM_DynamicsInitCvars (void);

#endif /* IQM_DYNAMICS_H */
