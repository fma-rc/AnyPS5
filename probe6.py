import json, struct, sys
sys.path.insert(0, ".")
import tso
from tso import tsharp, f2u, u2f
import math
def texture(count):
    tex = bytearray()
    for i in range(count):
        tex += bytes([i & 0xff, (i >> 8) & 0xff, (i >> 16) & 0xff, 0x5a])
    return bytes(tex)
vals = [0.3, -0.3, 7.3, -7.3, 1000.3, -1000.3, 65536.3, -65536.3, 3.0e6, -3.0e6, 1.0e9, -1.0e9, 3.0e38, -3.0e38, math.inf, -math.inf, math.nan]
rows = []
for i, u in enumerate(vals):
    for j, v in enumerate((0.3, vals[(i + 5) % len(vals)])):
        r = vals[(i * 3 + j) % len(vals)]
        rows.append((f2u(u), f2u(v), f2u(r), 0))
cases = []
for clamp in (0, 1, 2, 3, 4, 6):
    W, H, D = 7, 5, 3
    t = tsharp(56, W, H, ttype=10, depth=D - 1)
    s = [clamp | (clamp << 3) | (clamp << 6), 0xfff << 12, 0, 2 << 30]
    out = {}
    for dmask in (1, 2, 4):
        op = f"image_gather4_lz v[10:13], v[4:6], s[8:15], s[16:19] dmask:0x{dmask:x} dim:SQ_RSRC_IMG_3D"
        out[dmask] = [[round(u2f(w) * 255) for w in r] for r in tso.sample(t, s, texture(64 * H * D), rows, op=op)]
    cases.append({"clamp": clamp, "W": W, "H": H, "D": D, "rows": [[u2f(a), u2f(b), u2f(c)] for (a, b, c, _) in rows], "result": {str(k): v for k, v in out.items()}})
json.dump(cases, open(sys.argv[1], "w"))
print(len(cases), "cases")
