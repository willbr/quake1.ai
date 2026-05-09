// edit_history.h -- snapshot-based undo/redo stack.
//
// Strategy: serialize the entire edit_scene to .map text on every mutation
// boundary (drag start, atomic op like AddCube/Group/Ungroup) and push the
// snapshot. Undo restores the most recent snapshot; redo replays a snapshot
// that was popped by undo. Cheap because editor scenes are small (a few KB
// of text) and mutation boundaries are infrequent.

#ifndef EDIT_HISTORY_H
#define EDIT_HISTORY_H

void History_Init    (void);
void History_Shutdown(void);

// Push the *current* scene as a recoverable state, with a short user-facing
// description. Call this immediately BEFORE mutating, so undo replays the
// pre-mutation state. Clears the redo stack — once you make a new edit
// after undoing, the alternate timeline is gone.
void History_Push    (const char *desc);

// Returns 1 if a snapshot was applied, 0 if nothing to do.
int  History_Undo    (void);
int  History_Redo    (void);

int  History_CanUndo (void);
int  History_CanRedo (void);

// Drop both stacks. Used when the scene's identity changes (Scene_Load,
// editor close) so undo never tries to restore a different map's state.
void History_Clear   (void);

#endif // EDIT_HISTORY_H
