/*
 * Texture packs: a game's textures saved as PNGs (dumping) and PNGs of any size drawn in their place (replacing) -- the
 * way to play with upscaled textures without touching the game's files or its memory: a replacement lives on the host
 * GPU only, where the game's own copy would have gone (gfx_gpu.c upload_texture), so the game's pools, sized for the
 * Vita, never see it.
 *
 * A texture is known by its key, "<crc32>_<w>x<h>_<format>": the CRC-32 of its level 0 as the game holds it (the bytes
 * of the texture file's own data -- PVRTC/BC blocks, swizzled as stored: the key can be worked out from the game's
 * files too, dev/texnames.py), its size and its SceGxmTextureFormat. File names only have to end in the key, so a
 * pack can keep the game's own names and folders ("game/ui/textures/foo__1a2b3c4d_256x256_83000000.png").
 *
 * In the game's run folder (recomp/):
 *   texdump/   with RECOMP_TEX_DUMP=1 (or texture_dump = 1 in settings.ini): each texture the first time it is
 *              uploaded, level 0 decoded to RGBA (a worker thread writes the PNG, so dumping costs no frame time)
 *   texpack/   the replacements, at any depth; read once at start. RECOMP_TEX_REPLACE=0: ignored
 * A replacement's mip chain is made on the GPU from it; its proportions should match the original's (4x: 512x512 ->
 * 2048x2048). Cube maps, strided-linear and float formats are left alone.
 *
 * Filters, for a game's own textures with no replacement (the "texture_filters" game option: its menu and the launcher's
 * game settings): texture_gamma (1 = the game's own; above 1 lifts the dark and middle tones, the whites stay),
 * texture_saturation (1 = the game's own) and texture_sharpen (0 = off; an unsharp mask), on the colour maps only --
 * normal maps, masks and the HUD are left as they are. Which textures are colour maps comes from texnames.csv in the run
 * folder (key,path; the names ending in _c, outside game/ui/): made at the first start that needs it from the game's own
 * HG2 containers (app0's gamedata.bin, then patch.bin over it: Uncharted's -- dev/texnames.py is the same reading).
 */
#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>
#include "hle.h"
#include "gfx.h"
#include "settings.h"
#include "texpack.h"
#define STBI_NO_STDIO
#include "third_party/stb/stb_image.h"   /* (its implementation, with PNG: runtime/media/jpeg.c) */
#define STBI_WRITE_NO_STDIO
#include "third_party/stb/stb_image_write.h"

#define DUMP_DIR "texdump"
#define PACK_DIR "texpack"

static int g_dump = -1, g_replace = -1, g_filter;
static float g_gamma = 1, g_saturation = 1, g_sharpen;
static uint8_t g_curve[256];   /* the gamma curve, byte to byte */
static char (*g_colour)[40]; static int g_ncolour;   /* the colour maps' keys (sorted), from texnames.csv */
typedef struct { char key[40]; char *path; } PackFile;
static PackFile *g_pack; static int g_npack;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static char **g_seen; static int g_nseen, g_capseen;   /* keys dumped this session (sorted) */

static int key_cmp(const void *a, const void *b) { return strcmp(((const PackFile *)a)->key, ((const PackFile *)b)->key); }
/* the key a file name ends in (before ".png"): 8 hex, '_', WxH, '_', 8 hex */
static int key_of_name(const char *name, char key[40]) {
	size_t n = strlen(name);
	if (n < 4 || strcmp(name + n - 4, ".png") != 0) return 0;
	const char *end = name + n - 4, *p = end;
	int us = 0;
	while (p > name && us < 3) { p--; if (*p == '_') us++; }
	if (us == 3) p++;   /* past the '_' that comes before the key */
	else if (p != name) return 0;
	unsigned crc, w, h, fmt; char tail;
	if (end - p >= 40 || sscanf(p, "%8x_%ux%u_%8x%c", &crc, &w, &h, &fmt, &tail) != 5 || tail != '.') return 0;
	snprintf(key, 40, "%08x_%ux%u_%08x", crc, w, h, fmt);
	return 1;
}
static void scan(const char *dir, int depth) {
	DIR *d = opendir(dir); if (!d) return;
	struct dirent *de;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.') continue;
		char path[4096]; snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
		struct stat st; if (stat(path, &st) != 0) continue;
		if (S_ISDIR(st.st_mode)) { if (depth < 16) scan(path, depth + 1); continue; }
		char key[40]; if (!key_of_name(de->d_name, key)) continue;
		PackFile *g = realloc(g_pack, (size_t)(g_npack + 1) * sizeof *g_pack); if (!g) break;
		g_pack = g; snprintf(g_pack[g_npack].key, sizeof g_pack[g_npack].key, "%s", key); g_pack[g_npack].path = strdup(path); g_npack++;
	}
	closedir(d);
}
static int str_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
static void load_names(void) {   /* texnames.csv: key,path -- the colour maps' keys */
	FILE *f = fopen("texnames.csv", "rb"); if (!f) return;
	char line[1024]; int cap = 0;
	while (fgets(line, sizeof line, f)) {
		char *comma = strchr(line, ','); if (!comma || comma - line >= 40) continue;
		*comma = 0; char *path = comma + 1; size_t n = strlen(path);
		while (n && (path[n - 1] == '\n' || path[n - 1] == '\r')) path[--n] = 0;
		if (n < 8 || strcmp(path + n - 8, "_c.bstex") != 0 || strncmp(path, "game/ui/", 8) == 0) continue;
		if (g_ncolour == cap) { int nc = cap ? 2 * cap : 1024; char (*c)[40] = realloc(g_colour, (size_t)nc * sizeof *g_colour); if (!c) break; g_colour = c; cap = nc; }
		snprintf(g_colour[g_ncolour++], 40, "%s", line);
	}
	fclose(f);
	if (g_ncolour) qsort(g_colour, (size_t)g_ncolour, sizeof *g_colour, str_cmp);
}
/* ---- the colour maps' names, from the game's HG2 containers (MHVuze's UGAdata layout: the file count at 0x20, the name
 * pointers' table at 0x2c, from 0x30 a 12-byte entry a file -- name hash, size, offset; stored whole). A .bstex is a
 * 32-byte header and a GXT ("GXT\0", the texture count at 8, from 0x20 0x20 bytes a texture: data offset and size, palette,
 * flags, type, format, width, height); its key hashes level 0, whose size follows the format. */
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t level0_size(uint32_t fmt, uint32_t w, uint32_t h) {
	switch (fmt >> 24) {
	case 0x80: case 0x82: return (w > 16 ? w : 16) * (h > 8 ? h : 8) / 4;   /* PVRT(II) 2bpp */
	case 0x81: case 0x83: return (w > 8 ? w : 8) * (h > 8 ? h : 8) / 2;     /* PVRT(II) 4bpp */
	case 0x85: return ((w + 3) / 4 ? (w + 3) / 4 : 1) * ((h + 3) / 4 ? (h + 3) / 4 : 1) * 8;               /* UBC1 */
	case 0x86: case 0x87: return ((w + 3) / 4 ? (w + 3) / 4 : 1) * ((h + 3) / 4 ? (h + 3) / 4 : 1) * 16;   /* UBC2/3 */
	case 0x0c: return ((w + 7) & ~7u) * h * 4;   /* U8U8U8U8 */
	default: return 0;
	}
}
typedef struct { char name[200]; char key[40]; } NameKey;
static int colour_name(const char *s) { size_t n = strlen(s); return n > 8 && !strcmp(s + n - 8, "_c.bstex") && strncmp(s, "game/ui/", 8) != 0; }
static void hg2_colour_maps(const char *path, NameKey **v, int *n, int *cap) {
	FILE *f = fopen(path, "rb"); if (!f) return;
	uint8_t h[0x30];
	if (fread(h, 1, sizeof h, f) != sizeof h || memcmp(h, "HG2", 4) != 0) { fclose(f); return; }
	uint32_t count = rd32(h + 0x20), names = rd32(h + 0x2c);
	uint8_t *tab = count && count < (1u << 20) ? malloc((size_t)count * 12) : 0, *ptr = tab ? malloc((size_t)count * 4) : 0;
	if (!ptr || fread(tab, 12, count, f) != count || fseeko(f, (off_t)names, SEEK_SET) || fread(ptr, 4, count, f) != count) { free(tab); free(ptr); fclose(f); return; }
	uint8_t *data = 0; size_t datacap = 0;
	for (uint32_t i = 0; i < count; i++) {
		char name[200] = {0};
		if (fseeko(f, (off_t)rd32(ptr + 4 * i), SEEK_SET) || !fread(name, 1, sizeof name - 1, f) || !colour_name(name)) continue;
		uint32_t size = rd32(tab + 12 * i + 4), off = rd32(tab + 12 * i + 8);
		uint8_t gh[32 + 0x40];
		if (size < sizeof gh || fseeko(f, (off_t)off, SEEK_SET) || fread(gh, 1, sizeof gh, f) != sizeof gh || memcmp(gh + 32, "GXT", 4) != 0) continue;
		const uint8_t *ti = gh + 32 + 0x20;
		uint32_t doff = rd32(ti), dsize = rd32(ti + 4), fmt = rd32(ti + 20), w = ti[24] | ti[25] << 8, ht = ti[26] | ti[27] << 8, l0 = level0_size(fmt, w, ht);
		if (!l0 || l0 > dsize || 32 + doff + l0 > size) continue;
		if (l0 > datacap) { uint8_t *d = realloc(data, l0); if (!d) continue; data = d; datacap = l0; }
		if (fseeko(f, (off_t)off + 32 + doff, SEEK_SET) || fread(data, 1, l0, f) != l0) continue;
		int k = 0; while (k < *n && strcmp((*v)[k].name, name)) k++;   /* (an update's entry of a name replaces the game's) */
		if (k == *n) { if (*n == *cap) { int nc = *cap ? 2 * *cap : 1024; NameKey *x = realloc(*v, (size_t)nc * sizeof **v); if (!x) break; *v = x; *cap = nc; } (*n)++; }
		snprintf((*v)[k].name, sizeof (*v)[k].name, "%s", name);
		snprintf((*v)[k].key, sizeof (*v)[k].key, "%08x_%ux%u_%08x", (uint32_t)crc32(0L, data, l0), w, ht, fmt & 0x9f007000u);
	}
	free(data); free(tab); free(ptr); fclose(f);
}
static void make_names(void) {   /* texnames.csv, once: the colour maps of fs/app0's gamedata.bin, patch.bin over it */
	struct stat st; if (stat("texnames.csv", &st) == 0) return;
	NameKey *v = 0; int n = 0, cap = 0;
	hg2_colour_maps("fs/app0/gamedata.bin", &v, &n, &cap);
	hg2_colour_maps("fs/app0/patch.bin", &v, &n, &cap);
	if (n) {
		FILE *o = fopen("texnames.csv", "wb");
		if (o) { for (int i = 0; i < n; i++) fprintf(o, "%s,%s\n", v[i].key, v[i].name); fclose(o); }
		fprintf(stderr, "texture filters: %d colour maps named from the game's files (texnames.csv)\n", n);
	}
	free(v);
}
static float setting(const char *env, const char *key, float def) { const char *e = getenv(env); return e && *e ? (float)atof(e) : (float)settings_float(key, def); }
static void init(void) {
	if (g_dump >= 0) return;
	const char *e = getenv("RECOMP_TEX_DUMP");
	g_dump = e && *e ? atoi(e) != 0 : settings_int("texture_dump", 0) != 0;
	e = getenv("RECOMP_TEX_REPLACE");
	g_replace = !(e && *e && atoi(e) == 0);
	if (g_replace) {
		scan(PACK_DIR, 0);
		if (g_npack) qsort(g_pack, (size_t)g_npack, sizeof *g_pack, key_cmp);
		g_replace = g_npack > 0;
		if (g_npack) fprintf(stderr, "texture pack: %d replacement%s in %s/\n", g_npack, g_npack == 1 ? "" : "s", PACK_DIR);
	}
	if (g_dump) { mkdir(DUMP_DIR, 0755); fprintf(stderr, "texture dump: new textures saved to %s/\n", DUMP_DIR); }
	g_gamma = setting("RECOMP_TEX_GAMMA", "texture_gamma", 1.0f);
	g_saturation = setting("RECOMP_TEX_SATURATION", "texture_saturation", 1.0f);
	g_sharpen = setting("RECOMP_TEX_SHARPEN", "texture_sharpen", 0.0f);
	if (!(g_gamma >= 0.5f && g_gamma <= 2.0f)) g_gamma = 1;
	if (!(g_saturation >= 0 && g_saturation <= 2.0f)) g_saturation = 1;
	if (!(g_sharpen >= 0 && g_sharpen <= 2.0f)) g_sharpen = 0;
	const int any = fabsf(g_gamma - 1) > 0.005f || fabsf(g_saturation - 1) > 0.005f || g_sharpen > 0.005f;
	if (!any || !settings_game_option("texture_filters")) return;
	make_names(); load_names();
	for (int i = 0; i < 256; i++) g_curve[i] = (uint8_t)(255.0 * pow(i / 255.0, 1.0 / g_gamma) + 0.5);
	g_filter = g_ncolour > 0;
	if (g_filter) fprintf(stderr, "texture filters: gamma %.2f, saturation %.2f, sharpening %.2f on %d colour maps\n", g_gamma, g_saturation, g_sharpen, g_ncolour);
	else fprintf(stderr, "texture filters: no colour maps found in the game's files: off\n");
}
int texpack_on(void) { init(); return g_dump || g_replace || g_filter; }

/* the key, from level 0's bytes: 0 for a texture this leaves alone */
int texpack_key(const GfxTexture *t, char key[40]) {
	int layout = (t->u_mode >> 4) & 3;
	if (layout == 3 || (t->format & 0xfff) || !t->width || !t->height) return 0;
	GfxTexture l1; gfx_texture_level(t, 1, &l1);   /* (the level after level 0 starts where it ends, even with no mips) */
	uint32_t n = l1.data - t->data;
	if (!n || n > 64u << 20) return 0;
	uint32_t crc = (uint32_t)crc32(0L, (const Bytef *)G(t->data), n);
	snprintf(key, 40, "%08x_%ux%u_%08x", crc, t->width, t->height, t->format & 0x9f007000u);   /* (the format proper: bits 31, 24-28, 12-14 -- the rest of the word carries the texture's other control bits) */
	return 1;
}

uint8_t *texpack_replacement(const char *key, int *w, int *h) {
	init();
	if (!g_replace) return 0;
	PackFile k; snprintf(k.key, sizeof k.key, "%s", key);
	const PackFile *f = bsearch(&k, g_pack, (size_t)g_npack, sizeof *g_pack, key_cmp);
	if (!f) return 0;
	FILE *in = fopen(f->path, "rb"); if (!in) return 0;
	fseek(in, 0, SEEK_END); long sz = ftell(in); fseek(in, 0, SEEK_SET);
	uint8_t *buf = sz > 0 ? malloc((size_t)sz) : 0;
	int ok = buf && fread(buf, 1, (size_t)sz, in) == (size_t)sz; fclose(in);
	int comp; uint8_t *px = ok ? stbi_load_from_memory(buf, (int)sz, w, h, &comp, 4) : 0;
	free(buf);
	if (!px) { fprintf(stderr, "texture pack: %s isn't a PNG it can read\n", f->path); return 0; }
	return px;   /* (stbi's malloc: free() releases it) */
}

/* ---- dumping: a queue the worker writes out */
typedef struct DumpJob { struct DumpJob *next; char key[40]; int w, h; uint8_t *rgba; } DumpJob;
static DumpJob *g_q; static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER; static int g_worker;
static void write_fn(void *ctx, void *data, int size) { fwrite(data, 1, (size_t)size, (FILE *)ctx); }
static void *worker(void *arg) {
	(void)arg;
	for (;;) {
		pthread_mutex_lock(&g_mu);
		while (!g_q) pthread_cond_wait(&g_cv, &g_mu);
		DumpJob *j = g_q; g_q = j->next;
		pthread_mutex_unlock(&g_mu);
		char path[256]; snprintf(path, sizeof path, "%s/%s.png", DUMP_DIR, j->key);
		FILE *o = fopen(path, "wb");
		if (o) { stbi_write_png_to_func(write_fn, o, j->w, j->h, 4, j->rgba, j->w * 4); fclose(o); }
		free(j->rgba); free(j);
	}
	return 0;
}
static int seen_add(const char *key) {   /* 1: new this session (and not on disk from an earlier one) */
	int lo = 0, hi = g_nseen;
	while (lo < hi) { int m = (lo + hi) / 2, c = strcmp(g_seen[m], key); if (!c) return 0; if (c < 0) lo = m + 1; else hi = m; }
	if (g_nseen == g_capseen) { int nc = g_capseen ? 2 * g_capseen : 1024; char **s = realloc(g_seen, (size_t)nc * sizeof *s); if (!s) return 0; g_seen = s; g_capseen = nc; }
	memmove(g_seen + lo + 1, g_seen + lo, (size_t)(g_nseen - lo) * sizeof *g_seen);
	g_seen[lo] = strdup(key); g_nseen++;
	char path[256]; snprintf(path, sizeof path, "%s/%s.png", DUMP_DIR, key);
	struct stat st; return stat(path, &st) != 0;
}
void texpack_dump(const GfxTexture *t, const char *key) {
	init();
	if (!g_dump) return;
	pthread_mutex_lock(&g_mu);
	int fresh = seen_add(key);
	if (fresh && !g_worker) { pthread_t th; if (pthread_create(&th, 0, worker, 0) == 0) { pthread_detach(th); g_worker = 1; } }
	pthread_mutex_unlock(&g_mu);
	if (!fresh) return;
	DumpJob *j = calloc(1, sizeof *j); uint8_t *rgba = malloc((size_t)t->width * t->height * 4);
	if (!j || !rgba) { free(j); free(rgba); return; }
	GfxTexture l0; gfx_texture_level(t, 0, &l0);
	gfx_decode_texture_rgba8(&l0, rgba);
	snprintf(j->key, sizeof j->key, "%s", key); j->w = t->width; j->h = t->height; j->rgba = rgba;
	pthread_mutex_lock(&g_mu); j->next = g_q; g_q = j; pthread_cond_signal(&g_cv); pthread_mutex_unlock(&g_mu);
}

/* ---- filters: a colour map's level 0, decoded: the gamma curve, the saturation around each texel's own brightness, then
 * an unsharp mask (3x3, wrapping: the textures tile) */
uint8_t *texpack_filtered(const GfxTexture *t, const char *key) {
	init();
	if (!g_filter || !bsearch(key, g_colour, (size_t)g_ncolour, sizeof *g_colour, str_cmp)) return 0;
	const int w = t->width, h = t->height; const size_t n = (size_t)w * h;
	uint8_t *px = malloc(n * 4); float *v = malloc(n * 3 * sizeof *v);
	if (!px || !v) { free(px); free(v); return 0; }
	GfxTexture l0; gfx_texture_level(t, 0, &l0);
	gfx_decode_texture_rgba8(&l0, px);
	for (size_t i = 0; i < n; i++) {
		float r = g_curve[px[4 * i]], g = g_curve[px[4 * i + 1]], b = g_curve[px[4 * i + 2]], l = 0.299f * r + 0.587f * g + 0.114f * b;
		v[3 * i] = l + (r - l) * g_saturation; v[3 * i + 1] = l + (g - l) * g_saturation; v[3 * i + 2] = l + (b - l) * g_saturation;
	}
	for (int y = 0; y < h; y++) {
		const int ym = (y + h - 1) % h, yp = (y + 1) % h;
		for (int x = 0; x < w; x++) {
			const int xm = (x + w - 1) % w, xp = (x + 1) % w;
			for (int k = 0; k < 3; k++) {
				#define V(xx, yy) v[3 * ((size_t)(yy) * w + (xx)) + k]
				float blur = (V(xm, ym) + 2 * V(x, ym) + V(xp, ym) + 2 * V(xm, y) + 4 * V(x, y) + 2 * V(xp, y) + V(xm, yp) + 2 * V(x, yp) + V(xp, yp)) / 16.0f;
				float o = V(x, y) + g_sharpen * (V(x, y) - blur);
				#undef V
				px[4 * ((size_t)y * w + x) + k] = (uint8_t)(o < 0 ? 0 : o > 255 ? 255 : o + 0.5f);
			}
		}
	}
	free(v);
	return px;
}
