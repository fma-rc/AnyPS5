import json, sys
sys.path.insert(0, ".")
import tso
from tso import tsharp, ssharp, f2u, u2f
W, H, D = 64, 4, 4
tex = bytearray()
for z in range(D):
    for y in range(H):
        for x in range(W):
            tex += bytes([x, 16 + y, 32 + z, 0x5a])
rs = [-1.3, -1.0, -0.75, -0.5, -0.3, -0.05, 0.0, 0.3, 0.99, 1.0, 1.05, 1.3, 1.6, 2.2]
uvs = [(0.1 / W, 0.1 / H), (63.9 / W, 3.9 / H), (1.0 / W, 1.0 / H)]
rows = [(f2u(u), f2u(v), f2u(r), 0) for (u, v) in uvs for r in rs]
out = {"rows": [[u2f(a), u2f(b), u2f(c)] for (a, b, c, _) in rows]}
t = tsharp(56, W, H, ttype=10, depth=D - 1)
for cname, clamp in [("wrap", 0), ("mirror", 1), ("clamp", 2)]:
    s = ssharp(clamp=clamp, mag=1, minf=1)
    for dmask in (1, 2, 4):
        op = f"image_gather4_lz v[10:13], v[4:6], s[8:15], s[16:19] dmask:0x{dmask:x} dim:SQ_RSRC_IMG_3D"
        res = tso.sample(t, s, bytes(tex), rows, op=op)
        out[f"{cname}{dmask}"] = [[round(u2f(w) * 255) for w in r] for r in res]
json.dump(out, open(sys.argv[1], "w"))
for i, row in enumerate(out["rows"]):
    print(f"u*W={row[0]*W:.2f} v*H={row[1]*H:.2f} r={row[2]:+.2f} " + " ".join(f"{k}={out[k][i]}" for k in out if k != "rows"))
