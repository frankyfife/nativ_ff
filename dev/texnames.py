#!/usr/bin/env python3
"""Uncharted: Golden Abyss's texture names for a texture dump (runtime/gfx/texpack.c): the key each .bstex in gamedata.bin
gets when the game uploads it, so a dump's <key>.png files can carry the game's own paths -- for upscaling by folder, and as
a pack whose names say what each picture is (texpack.c only reads the key at the end of a name).

gamedata.bin: the "HG2" container (MHVuze's UGAdata, github.com/mhvuze/UGAdata): magic "HG2\\0", the file count at 0x20,
the name pointer table's offset at 0x2c, then from 0x30 a 12-byte entry a file (a word, the size, the offset); the files are
stored whole, uncompressed. A .bstex is a 32-byte header and a GXT (Sony's texture file: "GXT\\0", the texture count at 8,
then 0x20 bytes a texture from 0x20 -- data offset and size, palette, flags, type, SceGxmTextureFormat, width, height).
The key hashes level 0 as stored: its size follows the format (PVRTC 2/4 bits a texel with its minimum blocks, BC1 4, BC2/3
8, 32-bit 32 at a row of (width+7)&~7).

  texnames.py <gamedata.bin> [--list keys.csv] [--sort <texdump dir> <out dir>]
    --list   key,path per texture
    --sort   each <key>.png of a dump copied to <out>/<the texture's folder>/<its name>__<key>.png (the ones it can't name:
             <out>/_unnamed/)
"""
import os, shutil, struct, sys, zlib


def level0_size(fmt, w, h):
    base = fmt >> 24
    if base in (0x80, 0x82):   # PVRT(II) 2bpp: 8x4 blocks, at least 2x2 of them
        return max(w, 16) * max(h, 8) // 4
    if base in (0x81, 0x83):   # PVRT(II) 4bpp: 4x4 blocks, at least 2x2
        return max(w, 8) * max(h, 8) // 2
    if base == 0x85:           # UBC1
        return max((w + 3) // 4, 1) * max((h + 3) // 4, 1) * 8
    if base in (0x86, 0x87):   # UBC2/3
        return max((w + 3) // 4, 1) * max((h + 3) // 4, 1) * 16
    if base == 0x0c:           # U8U8U8U8
        return ((w + 7) & ~7) * h * 4
    return None


def textures(path):
    with open(path, "rb") as f:
        head = f.read(0x30)
        if head[:4] != b"HG2\0":
            sys.exit(f"{path}: not an HG2 container")
        count, names = struct.unpack_from("<I", head, 0x20)[0], struct.unpack_from("<I", head, 0x2c)[0]
        table = f.read(count * 12)
        f.seek(names); ptrs = struct.unpack(f"<{count}I", f.read(count * 4))
        for i in range(count):
            _, size, off = struct.unpack_from("<IiI", table, i * 12)
            f.seek(ptrs[i]); name = b""
            while b"\0" not in name:
                chunk = f.read(64)
                if not chunk: break
                name += chunk
            name = name.split(b"\0", 1)[0].decode("utf-8", "replace")
            if not name.lower().endswith(".bstex"):
                continue
            f.seek(off); data = f.read(size)
            gxt = data[32:]
            if gxt[:4] != b"GXT\0":
                continue
            for k in range(struct.unpack_from("<I", gxt, 8)[0]):
                doff, dsize, _, _, _, fmt, w, h = struct.unpack_from("<IIiIIIHH", gxt, 0x20 + 0x20 * k)
                n = level0_size(fmt, w, h)
                if n is None or n > dsize:
                    continue
                crc = zlib.crc32(gxt[doff:doff + n]) & 0xffffffff
                yield f"{crc:08x}_{w}x{h}_{fmt & 0x9f007000:08x}", name   # (the format proper, as runtime/gfx/texpack.c keys it)


def main(argv):
    if not argv:
        sys.exit(__doc__)
    keys = {}
    for key, name in textures(argv[0]):
        keys.setdefault(key, name)
    print(f"{len(keys)} textures named", file=sys.stderr)
    if "--list" in argv:
        with open(argv[argv.index("--list") + 1], "w", encoding="utf-8") as o:
            for k, n in sorted(keys.items(), key=lambda x: x[1]):
                o.write(f"{k},{n}\n")
    if "--sort" in argv:
        i = argv.index("--sort"); dump, out = argv[i + 1], argv[i + 2]
        named = unnamed = 0
        for fn in sorted(os.listdir(dump)):
            if not fn.endswith(".png"):
                continue
            key = fn[:-4]
            if key in keys:
                folder, base = os.path.split(keys[key])
                dst = os.path.join(out, folder, os.path.splitext(base)[0] + "__" + fn); named += 1
            else:
                dst = os.path.join(out, "_unnamed", fn); unnamed += 1
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(os.path.join(dump, fn), dst)
        print(f"{named} named, {unnamed} not (render targets, movies, textures the game builds itself) -> {out}", file=sys.stderr)


if __name__ == "__main__":
    main(sys.argv[1:])
