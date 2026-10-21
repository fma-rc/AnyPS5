import json, struct, sys
sys.path.insert(0, ".")
import tso
from tso import tsharp, f2u, u2f
import struct as st
def texture(count):
    tex = bytearray()
    for i in range(count):
        tex += bytes([i & 0xff, (i >> 8) & 0xff, (i >> 16) & 0xff, 0x5a])
    return bytes(tex)
def bits(x): return st.unpack("<I", st.pack("<f", x))[0]
W, H, D = 8, 4, 4
values = []
for k in range(-7, 8):
    values.append(k / 4096)
for x in range(0, 9):
    for d in (-1, 1, -2, 2, -3, 3):
        values.append((x * 256 + 128 + d * 0.5) / 2048)
values += [2.0 ** -20, -(2.0 ** -20), 2.0 ** -40, -(2.0 ** -40), 1.0 - 2.0 ** -24, -1.0 + 2.0 ** -24, 1.0 + 1 / 4096, -1.0 - 1 / 4096, 2.0 - 1 / 4096]
specials = [0x80000000, 0x80000001, 0x00000001, 0x807fffff]
us = [bits(v) for v in values] + specials
rows = []
for i, u in enumerate(us):
    rows.append((u, f2u(0.3), f2u(0.3), 0))
    rows.append((f2u(0.3), u, f2u(0.3), 0))
    rows.append((f2u(0.3), f2u(0.3), u, 0))
cases = []
for clamp in (0, 1, 2, 4, 6):
    t = tsharp(56, W, H, ttype=10, depth=D - 1)
    s = [clamp | (clamp << 3) | (clamp << 6), 0xfff << 12, 0, 2 << 30]
    out = {1: [], 2: [], 4: []}
    for i in range(0, len(rows), 256):
        part = rows[i:i + 256]
        for dmask in (1, 2, 4):
            op = f"image_gather4_lz v[10:13], v[4:6], s[8:15], s[16:19] dmask:0x{dmask:x} dim:SQ_RSRC_IMG_3D"
            out[dmask] += [[round(u2f(w) * 255) for w in r] for r in tso.sample(t, s, texture(64 * H * D), part, op=op)]
    cases.append({"clamp": clamp, "W": W, "H": H, "D": D, "rows": [[a, b, c] for (a, b, c, _) in rows], "result": {str(k): v for k, v in out.items()}})
json.dump(cases, open(sys.argv[1], "w"))
print(len(cases), "cases", len(rows), "rows")
