// lighting.c

#include <stdint.h>
#include "light.h"

/*

NOTES
-----

*/

float		scaledist = 1.0;
float		scalecos = 0.5;
float		rangescale = 0.5;

qboolean	dirt_enable = false;
float		dirt_gain   = 1.0f;
float		dirt_depth  = 128.0f;
int		dirt_samples = 32;
qboolean	dirt_debug  = false;

byte		*filebase, *file_p, *file_end;

dmodel_t	*bspmodel;
int			bspfileface;	// next surface to dispatch

vec3_t	bsp_origin;

qboolean	extrasamples;

float		minlights[MAX_MAP_FACES];

/* Parallel-to-dlightdata RGB sidecar buffer (Stage 2). LightFace writes
 * three bytes here for every byte it writes into dlightdata, so byte
 * offsets translate by *3. light_lib.c slices [0..lightdatasize*3] out
 * of this buffer as the .lit payload after LightWorld returns. */
byte		light_rgb_dlightdata[MAX_MAP_LIGHTING * 3];


byte *GetFileSpace (int size)
{
	byte	*buf;

	LOCK;
	/* Original id source used `(long)file_p` for the 4-byte alignment
	 * step. On Windows x64 `long` is 32-bit but pointers are 64-bit, so
	 * that cast silently truncates the upper half and produces a wild
	 * pointer when the buffer's high address is non-zero. Use uintptr_t
	 * so the alignment math stays inside the pointer's true width. */
	file_p = (byte *)(((uintptr_t)file_p + 3) & ~(uintptr_t)3);
	buf = file_p;
	file_p += size;
	UNLOCK;
	if (file_p > file_end)
		Error ("GetFileSpace: overrun");
	return buf;
}


void LightThread (void *junk)
{
	int			i;

	while (1)
	{
		LOCK;
		i = bspfileface++;
		UNLOCK;
		if (i >= numfaces)
			return;

		LightFace (i);
	}
}

/*
=============
LightWorld
=============
*/
void LightWorld (void)
{
	filebase = file_p = dlightdata;
	file_end = filebase + MAX_MAP_LIGHTING;

	RunThreadsOn (LightThread);

	lightdatasize = file_p - filebase;

	printf ("lightdatasize: %i\n", lightdatasize);
}


/*
========
main

light modelfile

NOTE: this function is renamed to light_main_unused by light_namespace.h
and never called. The real entry point is light_compile_to_memory() in
light_lib.c, which is invoked by the editor's "Compile + Light" command.
The body is retained as documentation of the original argv-driven flow.
========
*/
int main (int argc, char **argv)
{
	int		i;
	double		start, end;
	char		source[1024];

	printf ("----- LightFaces ----\n");

	for (i=1 ; i<argc ; i++)
	{
		if (!strcmp(argv[i],"-threads"))
		{
			numthreads = atoi (argv[i+1]);
			i++;
		}
		else if (!strcmp(argv[i],"-extra"))
		{
			extrasamples = true;
			printf ("extra sampling enabled\n");
		}
		else if (!strcmp(argv[i],"-dist"))
		{
			scaledist = atof (argv[i+1]);
			i++;
		}
		else if (!strcmp(argv[i],"-range"))
		{
			rangescale = atof (argv[i+1]);
			i++;
		}
		else if (argv[i][0] == '-')
			Error ("Unknown option \"%s\"", argv[i]);
		else
			break;
	}

	if (i != argc - 1)
		Error ("usage: light [-threads num] [-extra] bspfile");

	InitThreads ();

	start = I_FloatTime ();

	strcpy (source, argv[i]);
	StripExtension (source);
	DefaultExtension (source, ".bsp");

	LoadBSPFile (source);
	LoadEntities ();

	MakeTnodes (&dmodels[0]);

	LightWorld ();

	WriteEntitiesToString ();
	WriteBSPFile (source);

	end = I_FloatTime ();
	printf ("%5.1f seconds elapsed\n", end-start);

	return 0;
}

/* Reset light.c's file-scope state between bakes. Exposed so light_lib.c
 * can clear it without needing the symbols themselves on its include path
 * (the namespace header makes externing them awkward). */
void light_reset_lightc (void)
{
	scaledist    = 1.0f;
	scalecos     = 0.5f;
	rangescale   = 0.5f;
	dirt_enable  = false;
	dirt_gain    = 1.0f;
	dirt_depth   = 128.0f;
	dirt_samples = 32;
	dirt_debug   = false;
	filebase     = (byte *)0;
	file_p       = (byte *)0;
	file_end     = (byte *)0;
	bspfileface  = 0;
	extrasamples = false;
	bsp_origin[0] = bsp_origin[1] = bsp_origin[2] = 0;
	memset (minlights, 0, sizeof(minlights));
	memset (light_rgb_dlightdata, 0, sizeof(light_rgb_dlightdata));
}
