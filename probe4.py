import json, struct, sys
sys.path.insert(0, ".")
import tso
from tso import tsharp, f2u, u2f
import struct as st
def texture(W, H, D):
    tex = bytearray()
    for i in range(1 << 16):
        tex += bytes([i & 0xff, (i >> 8) & 0xff, (i >> 16) & 0xff, 0x5a])
    return bytes(tex)
def sampler(clamp):
    return [clamp | (clamp << 3) | (clamp << 6), 0xfff << 12, 0, 0]
def gather(t, s, tex, rows):
    out = {}
    for dmask in (1, 2, 4):
        op = f"image_gather4_lz v[10:13], v[4:6], s[8:15], s[16:19] dmask:0x{dmask:x} dim:SQ_RSRC_IMG_3D"
        out[dmask] = [[round(u2f(w) * 255) for w in r] for r in tso.sample(t, s, tex, rows, op=op)]
    return out
def f32(x): return st.unpack("<f", st.pack("<f", x))[0]
eps = [k / 4096 for k in range(-40, 41, 1)]
cases = []
for (W, H, D) in ((5, 3, 3), (48, 5, 3), (7, 6, 5)):
    rows = []
    for e in eps:
        for k in (1, 3):
            u = f32((k + 0.5 + e) / W); v = f32((1.5 + 2 * e) / H); r = f32((1 + e) / D)
            rows.append((f2u(u), f2u(v), f2u(r), 0))
    for clamp in (0, 2):
        t = tsharp(56, W, H, ttype=10, depth=D - 1)
        for i in range(0, len(rows), 64):
            part = rows[i:i + 64]
            res = gather(t, sampler(clamp), texture(W, H, D), part)
            cases.append({"W": W, "H": H, "D": D, "clamp": clamp, "rows": [[u2f(a), u2f(b), u2f(c)] for (a, b, c, _) in part],
                          "result": {str(k): v for k, v in res.items()}})
json.dump(cases, open(sys.argv[1], "w"))
print(len(cases), "chunks")
