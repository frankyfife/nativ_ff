#!/usr/bin/env python3
"""Uncharted 1.03 with 1.00's shaders: a copy of the update's patch.bin whose two shader bundles (game/bspsh.bin, the
fragment programs, and game/bsvsh.bin, the vertex programs) are the game's own 1.00 ones.

1.03 rebuilt all 429 programs of each bundle with a newer shader compiler -- the same programs in the same order under
the same names, but not one of them byte for byte 1.00's -- and the translation (runtime/gfx/usse*.c, this pack's
shaders.txt) is proven on 1.00's: 7 of 1.03's fall back to the software path, whose render-target downloads took a frame
to ~85 ms, and skinned attachments (hair, the flashlight) drew away from the body. The bundles go on at the end of the
HG2 container (MHVuze's UGAdata layout: the file count at 0x20, from 0x30 a 12-byte entry a file -- name hash, size,
offset) and their two entries point there; nothing else moves.

  uc_shaders103.py <gamedata.bin (1.00)> <patch.bin (1.03)> <out patch.bin>
"""
import struct, sys

NAMES = ("game/bspsh.bin", "game/bsvsh.bin")


def table(f):
    f.seek(0); h = f.read(0x30)
    if h[:4] != b"HG2\0":
        sys.exit(f"{f.name}: not an HG2 container")
    count, names = struct.unpack_from("<I", h, 0x20)[0], struct.unpack_from("<I", h, 0x2c)[0]
    f.seek(0x30); tab = f.read(count * 12)
    f.seek(names); ptrs = struct.unpack(f"<{count}I", f.read(count * 4))
    out = {}
    for i in range(count):
        f.seek(ptrs[i]); n = b""
        while b"\0" not in n:
            c = f.read(64)
            if not c: break
            n += c
        name = n.split(b"\0", 1)[0].decode("utf-8", "replace")
        if name in NAMES:
            out[name] = (i,) + struct.unpack_from("<IiI", tab, i * 12)
    return out


def main(game, patch, out):
    with open(game, "rb") as g, open(patch, "rb") as p:
        gt, pt = table(g), table(p)
        if set(gt) != set(NAMES) or set(pt) != set(NAMES):
            sys.exit("the shader bundles aren't in both containers")
        p.seek(0); data = bytearray(p.read())
        for name in NAMES:
            _, ghash, gsize, goff = gt[name]; i, phash, _, _ = pt[name]
            g.seek(goff); blob = g.read(gsize)
            while len(data) % 0x40: data.append(0)   # (the container's own files start on 64-byte boundaries)
            off = len(data); data += blob
            struct.pack_into("<IiI", data, 0x30 + 12 * i, phash, gsize, off)
            print(f"{name}: 1.00's ({gsize} bytes) at {off:#x}")
    with open(out, "wb") as o:
        o.write(data)


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
