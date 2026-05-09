// editor_classlist.h -- thin cached view of the game DLL's spawn-class table.
// One copy per DLL load; the spawn dialog filter searches over it.

#ifndef EDITOR_CLASSLIST_H
#define EDITOR_CLASSLIST_H

// Returns a NULL-terminated array of classname strings the running game DLL
// can spawn. Pointers borrow from the DLL's static spawn table; valid until
// the DLL hot-reloads (this cache rebuilds on its own when that happens).
// *out_count receives the entry count, or 0 if the DLL hasn't loaded yet.
const char *const *Editor_ClassList_Get(int *out_count);

#endif // EDITOR_CLASSLIST_H
