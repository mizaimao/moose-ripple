/*
 * Direct3D's fixed-function vertex processing (transform, lighting, fog, texture coordinates)
 * as an ARB vertex program, for the Direct3D 8 and 9 devices that process vertices in
 * hardware. MIT licence, see LICENSE.
 *
 * The program reads the vertex through OpenGL's conventional arrays (vertex.position,
 * vertex.normal, vertex.color, vertex.color.secondary, vertex.texcoord[n]) and the state from
 * program.env, laid out as below; tnl_key() describes what it must do, tnl_text() writes it.
 */
#ifndef TNL_ARB_H
#define TNL_ARB_H

#define TNL_LIGHTS 8                    /* lights at once (D3DCAPS8.MaxActiveLights) */
#define TNL_STAGES 8                    /* texture coordinate outputs */

/* program.env */
#define TNL_ENV_MVP     0   /* 4: world x view x projection, with OpenGL's adjustments: columns */
#define TNL_ENV_MV      4   /* 4: world x view: columns (positions in view space) */
#define TNL_ENV_NORMAL  8   /* 3: the normals' matrix (inverse transpose of world x view): columns */
#define TNL_ENV_MAT     11  /* 4: the material's diffuse, ambient, specular and emissive colours */
#define TNL_ENV_POWER   15  /* x: the material's specular power */
#define TNL_ENV_AMBIENT 16  /* the ambient light (D3DRS_AMBIENT) */
#define TNL_ENV_FOG     17  /* end / (end - start), 1 / (end - start), density */
#define TNL_ENV_LIGHT   18  /* TNL_LIGHT_SIZE a light, see below */
#define TNL_LIGHT_SIZE  7   /* diffuse, specular, ambient, position (w: range), direction (to the
                               light for a directional one, the spot's own for a spot), attenuation
                               0, 1, 2 (w: falloff), spot (cos theta/2, cos phi/2, 1 / their
                               difference) */
#define TNL_ENV_TEX     (TNL_ENV_LIGHT + TNL_LIGHTS * TNL_LIGHT_SIZE)  /* 4 a stage: its texture
                                                                          matrix, columns */
#define TNL_ENV_COUNT   (TNL_ENV_TEX + 4 * TNL_STAGES)

/* light kinds, as Direct3D numbers them (0: no light in that place) */
#define TNL_POINT       1
#define TNL_SPOT        2
#define TNL_DIRECTIONAL 3

/* material sources */
#define TNL_MATERIAL    0
#define TNL_COLOR1      1                   /* the vertex's diffuse colour */
#define TNL_COLOR2      2                   /* its specular colour */

/* fog */
#define TNL_FOG_NONE    0
#define TNL_FOG_EXP     1                   /* Direct3D's D3DFOG_* */
#define TNL_FOG_EXP2    2
#define TNL_FOG_LINEAR  3
#define TNL_FOG_ALPHA   4                   /* the specular colour's alpha is the factor */
#define TNL_DEPTH_VIEW  0                   /* the factor from the view-space z */
#define TNL_DEPTH_RANGE 1                   /* the distance from the eye */
#define TNL_DEPTH_SCREEN 2                  /* the depth buffer's z (table fog) */

/* texture coordinate sources: 0 none (0, 0, 0, 1); 1 + n: the vertex's set n */
#define TNL_TEX_NORMAL     9                /* D3DTSS_TCI_CAMERASPACENORMAL */
#define TNL_TEX_POSITION   10               /* ...CAMERASPACEPOSITION */
#define TNL_TEX_REFLECTION 11               /* ...CAMERASPACEREFLECTIONVECTOR */
#define TNL_PROJECTED      0x80             /* in xform: D3DTTFF_PROJECTED */

/* What one program does; all bytes, compared whole (tnl_key zeroes it first). */
typedef struct {
    unsigned char lighting, normal, normalize, local_viewer;
    unsigned char diffuse, specular;        /* the vertices have these colours */
    unsigned char mcs[4];                   /* diffuse, ambient, specular, emissive: TNL_MATERIAL.. */
    unsigned char light[TNL_LIGHTS];        /* 0, or TNL_POINT.. */
    unsigned char fog, fog_depth;           /* TNL_FOG_*, TNL_DEPTH_* */
    unsigned char fog_coord;                /* the factor to OpenGL's fog coordinate (else to the
                                               fog unit's texture coordinates) */
    unsigned char fog_unit;                 /* the fixed-function fog's texture unit */
    unsigned char fog_specular;             /* the specular colour times the factor */
    unsigned char tex[TNL_STAGES];          /* each output's source, see above */
    unsigned char tex_size[TNL_STAGES];     /* the components of a vertex's set */
    unsigned char xform[TNL_STAGES];        /* D3DTTFF_COUNTn, | TNL_PROJECTED; 0 none */
} TNL_KEY;

#define TNL_TEXT_BYTES 12288

/* The program text for k into out (size bytes); its length with the final 0, 0 if too long. */
unsigned int tnl_text(const TNL_KEY *k, char *out, unsigned int size);

#endif
