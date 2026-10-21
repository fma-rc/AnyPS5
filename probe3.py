import json, struct, sys
sys.path.insert(0, ".")
import tso
from tso import tsharp, f2u, u2f

# Linear RGBA8 3D textures. Texel i of the memory holds R, G, B = the three low bytes of i, A = 0x5a, so every
# gathered value names the exact texel the hardware read, whatever the level layout.
def texture(W, H, D):
    count = 1 << 16
    tex = bytearray()
    for i in range(count):
        tex += bytes([i & 0xff, (i >> 8) & 0xff, (i >> 16) & 0xff, 0x5a])
    return bytes(tex)

def sampler(clamp, border=0, filt=0, mip=0, min_lod=0, bias=0, aniso=0, max_lod=0xfff):
    w0 = clamp | (clamp << 3) | (clamp << 6) | (aniso << 9)
    w1 = (min_lod & 0xfff) | ((max_lod & 0xfff) << 12)
    w2 = (bias & 0x3fff) | (filt << 20) | (filt << 22) | (mip << 26)
    w3 = border << 30
    return [w0, w1, w2, w3]

def gather(t, s, tex, rows):
    out = {}
    for dmask in (1, 2, 4):
        op = f"image_gather4_lz v[10:13], v[4:6], s[8:15], s[16:19] dmask:0x{dmask:x} dim:SQ_RSRC_IMG_3D"
        res = tso.sample(t, s, tex, rows, op=op)
        out[dmask] = [[round(u2f(w) * 255) for w in r] for r in res]
    return out

cases = []
def add(name, W, H, D, s, rows, base_level=0, last_level=0):
    t = tsharp(56, W, H, ttype=10, depth=D - 1, base_level=base_level, last_level=last_level, max_mip=last_level)
    res = gather(t, s, texture(W, H, D), rows)
    cases.append({"name": name, "W": W, "H": H, "D": D, "sampler": s, "base_level": base_level, "last_level": last_level,
                  "rows": [[u2f(a), u2f(b), u2f(c)] for (a, b, c, _) in rows], "result": {str(k): v for k, v in res.items()}})

def grid(W, H, D):
    us = [-0.6, -0.13, 0.02, 0.31, 0.5, 0.77, 0.99, 1.04, 1.6]
    vs = [-0.4, 0.1, 0.45, 0.93, 1.3]
    rs = [-0.7, -0.05, 0.2, 0.55, 0.97, 1.2, 1.9]
    rows = []
    for i, u in enumerate(us):
        for j, v in enumerate(vs):
            r = rs[(i * 3 + j) % len(rs)]
            rows.append((f2u(u), f2u(v), f2u(r), 0))
    return rows

rows = grid(8, 4, 4)
for border in range(3):
    for clamp in range(8):
        add(f"clamp{clamp}_border{border}", 8, 4, 4, sampler(clamp, border), rows)
add("mips_baseline", 8, 4, 4, sampler(2, mip=1), rows, last_level=1)
for name, s in [("min_lod_1", sampler(2, mip=1, min_lod=0x100)), ("min_lod_1_mip_linear", sampler(2, mip=2, min_lod=0x100)),
                ("bias_plus_1", sampler(2, mip=1, bias=0x100)), ("bias_plus_half_linear", sampler(2, mip=2, bias=0x80)),
                ("max_lod_0", sampler(2, mip=1, max_lod=0)), ("filter_linear", sampler(2, filt=1)),
                ("filter_aniso16", sampler(2, filt=1, aniso=4))]:
    add(name, 8, 4, 4, s, rows, last_level=1)
add("base_level_1", 8, 4, 4, sampler(2), rows, base_level=1, last_level=1)
for name, filt in [("npot_point", 0), ("npot_linear", 1)]:
    add(name, 48, 5, 3, sampler(0, filt=filt), grid(48, 5, 3))
    add(name + "_clamp", 48, 5, 3, sampler(2, filt=filt), grid(48, 5, 3))
json.dump(cases, open(sys.argv[1], "w"))
print(len(cases), "cases")
