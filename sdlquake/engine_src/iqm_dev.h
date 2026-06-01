#ifndef IQM_DEV_H
#define IQM_DEV_H

// Dev harness for the mod_iqm renderer: console commands to dump and spawn a
// client-side IQM actor without a server. R1 only.
void IQMDev_Init (void);        // register console commands (called from CL_Init)
void IQMDev_AddToScene (void);  // inject the persistent dev actor into cl_visedicts

#endif
