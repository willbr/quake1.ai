// cmdlib.h

#ifndef __CMDLIB__
#define __CMDLIB__

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <stdarg.h>

/* Tracked allocator — qbsp's malloc/free go through qbsp_malloc/qbsp_free
 * which keep a doubly-linked list of all live allocations. qbsp_reset_state
 * (called at the top of each qbsp_compile_to_memory) walks the list and
 * frees stragglers, so qbsp's many file-scope buffers + linked structures
 * (winding_t, face_t, surface_t, portal_t, mface_t, ...) don't leak across
 * compiles. Free during a single compile still works (qbsp_free unlinks),
 * so peak memory stays bounded. Defined here so every qbsp .c file picks
 * up the redirect after stdlib.h has already declared the real symbols. */
extern void *qbsp_malloc(size_t size);
extern void  qbsp_free  (void *p);
#define malloc(s)  qbsp_malloc(s)
#define free(p)    qbsp_free(p)

/* Output redirect — qbsp's printf calls land in the engine console.
 * fprintf is intentionally NOT redirected: every fprintf call site in
 * qbsp writes to a real FILE * (hull files, portal files, leak files)
 * — never to stderr/stdout. Routing fprintf through Con_Printf would
 * silently swallow the hull-file output, leaving ReadClipHull(1) to
 * parse an empty test.h1 and bail.
 *
 * sprintf is also untouched (writes to caller-provided buffer).
 * Defined here (not in qbsp_namespace.h) because force-including it
 * before stdio.h breaks Windows MinGW's __format__ attribute on
 * printf's declaration. */
extern void qbsp_con_print(const char *fmt, ...);
#define printf            qbsp_con_print
/* vprintf -> drop. qbsp's Error() in cmdlib.c uses vsnprintf into a
 * buffer (we patched that path); other vprintf calls would silently
 * lose their output, which is fine — we only care about the formatted
 * status / error stream from this library. */
#define vprintf(fmt, ap)  ((void)0)

#ifndef __BYTEBOOL__
#define __BYTEBOOL__
typedef enum {false, true} qboolean;
typedef unsigned char byte;
#endif

// the dec offsetof macro doesn't work very well...
#define myoffsetof(type,identifier) ((size_t)&((type *)0)->identifier)


// set these before calling CheckParm
extern int myargc;
extern char **myargv;

char *strupr (char *in);
char *strlower (char *in);
int Q_strncasecmp (char *s1, char *s2, int n);
int Q_strcasecmp (char *s1, char *s2);
void Q_getwd (char *out);

/* renamed from filelength: collides with Windows io.h's filelength(int) */
int qbsp_filelength (FILE *f);
int	FileTime (char *path);

void	Q_mkdir (char *path);

extern	char		qdir[1024];
extern	char		gamedir[1024];
void SetQdirFromPath (char *path);
char *ExpandPath (char *path);
char *ExpandPathAndArchive (char *path);


double I_FloatTime (void);

void	Error (char *error, ...);
int		CheckParm (char *check);

FILE	*SafeOpenWrite (char *filename);
FILE	*SafeOpenRead (char *filename);
void	SafeRead (FILE *f, void *buffer, int count);
void	SafeWrite (FILE *f, void *buffer, int count);

int		LoadFile (char *filename, void **bufferptr);
void	SaveFile (char *filename, void *buffer, int count);

void 	DefaultExtension (char *path, char *extension);
void 	DefaultPath (char *path, char *basepath);
void 	StripFilename (char *path);
void 	StripExtension (char *path);

void 	ExtractFilePath (char *path, char *dest);
void 	ExtractFileBase (char *path, char *dest);
void	ExtractFileExtension (char *path, char *dest);

int 	ParseNum (char *str);

short	BigShort (short l);
short	LittleShort (short l);
int		BigLong (int l);
int		LittleLong (int l);
float	BigFloat (float l);
float	LittleFloat (float l);


char *COM_Parse (char *data);

extern	char		com_token[1024];
extern	qboolean	com_eof;

char *copystring(char *s);


void CRC_Init(unsigned short *crcvalue);
void CRC_ProcessByte(unsigned short *crcvalue, byte data);
unsigned short CRC_Value(unsigned short crcvalue);

void	CreatePath (char *path);
void CopyFile (char *from, char *to);

extern	qboolean		archive;
extern	char			archivedir[1024];


#endif
