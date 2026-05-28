#ifndef MDL_FORMAT_H
#define MDL_FORMAT_H

/* Quake MDL (alias model) on-disk constants. The parser reads fields with a
   little-endian cursor rather than casting packed structs, so only the magic
   numbers and the layout (documented below) are needed here.

   Header (all little-endian):
     int   ident            "IDPO"
     int   version          6
     float scale[3]
     float scale_origin[3]
     float boundingradius
     float eyeposition[3]
     int   numskins
     int   skinwidth
     int   skinheight
     int   numverts
     int   numtris
     int   numframes
     int   synctype
     int   flags
     float size

   Then numskins skins. Each skin:
     int   type             0 = single, 1 = group
     single: skinwidth*skinheight bytes
     group:  int nb; nb floats (intervals); nb*(skinwidth*skinheight) bytes

   Then numverts stverts:  int onseam; int s; int t
   Then numtris triangles: int facesfront; int vertindex[3]
   Then numframes frames. Each frame:
     int   type             0 = single, 1 = group
     single: trivertx bboxmin(4B); trivertx bboxmax(4B); char name[16];
             numverts*trivertx
     group:  int nb; trivertx bboxmin(4B); trivertx bboxmax(4B);
             nb floats (intervals); nb*(bboxmin(4B),bboxmax(4B),name[16],
             numverts*trivertx)

   trivertx (4 bytes): unsigned char v[3]; unsigned char lightnormalindex; */

#define MDL_IDENT    (('O' << 24) + ('P' << 16) + ('D' << 8) + 'I') /* "IDPO" */
#define MDL_VERSION  6

#define MDL_TYPE_SINGLE 0
#define MDL_TYPE_GROUP  1

#endif /* MDL_FORMAT_H */
