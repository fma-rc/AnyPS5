import json, struct, sys
sys.path.insert(0, ".")
import tso
from tso import tsharp, f2u, u2f
import random, struct as st
from fractions import Fraction as F
import numpy as np
def texture(count):
    tex = bytearray()
    for i in range(count):
        tex += bytes([i & 0xff, (i >> 8) & 0xff, (i >> 16) & 0xff, 0x5a])
    return bytes(tex)
def rnd(x): return math.floor(x * 256 + F(1, 2))
import math
random.seed(5)
cases = []
for W in (3001, 12345):
    H, D = 2, 2
    disagree, agree = [], []
    while len(disagree) < 96 or len(agree) < 32:
        u = np.float32(random.uniform(0.0, 1.0))
        exact = rnd(F(float(u)) * W)
        f32 = rnd(F(float(np.float32(u * np.float32(W)))))
        row = (f2u(float(u)), f2u(0.25), f2u(0.25), 0)
        if (exact - 128) // 256 != (f32 - 128) // 256:
            if len(disagree) < 96: disagree.append(row)
        elif len(agree) < 32: agree.append(row)
    rows = disagree + agree
    t = tsharp(56, W, H, ttype=10, depth=D - 1)
    s = [2 | (2 << 3) | (2 << 6), 0xfff << 12, 0, 0]
    pitch = -(-W // 64) * 64
    out = {}
    for dmask in (1, 2, 4):
        op = f"image_gather4_lz v[10:13], v[4:6], s[8:15], s[16:19] dmask:0x{dmask:x} dim:SQ_RSRC_IMG_3D"
        out[dmask] = []
        for i in range(0, len(rows), 64):
            out[dmask] += [[round(u2f(w) * 255) for w in r] for r in tso.sample(t, s, texture(pitch * H * D), rows[i:i + 64], op=op)]
    cases.append({"W": W, "H": H, "D": D, "pitch": pitch, "disagree": len(disagree), "rows": [[u2f(a), u2f(b), u2f(c)] for (a, b, c, _) in rows], "result": {str(k): v for k, v in out.items()}})
json.dump(cases, open(sys.argv[1], "w"))
print(len(cases), "cases")
