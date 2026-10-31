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
 */
#include <dirent.h>
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

static int g_dump = -1, g_replace = -1;
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
}
int texpack_on(void) { init(); return g_dump || g_replace; }

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
