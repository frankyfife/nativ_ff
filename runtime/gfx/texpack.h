/* texture packs: dumping a game's textures as PNGs and drawing PNGs in their place (texpack.c) */
#ifndef RECOMP_TEXPACK_H
#define RECOMP_TEXPACK_H
#include <stdint.h>
#include "gfx.h"
int texpack_on(void);                                         /* dumping or a pack in use: the GPU backend asks for keys */
int texpack_key(const GfxTexture *t, char key[40]);           /* 0: a texture packs leave alone (cube, strided, ...) */
uint8_t *texpack_replacement(const char *key, int *w, int *h);   /* the pack's RGBA8 for it (free() it), or 0 */
void texpack_dump(const GfxTexture *t, const char *key);      /* saved once (texdump/<key>.png) when dumping is on */
uint8_t *texpack_filtered(const GfxTexture *t, const char *key);   /* a colour map with the contrast/sharpening filters, RGBA8 at its own size (free() it), or 0 */
#endif
