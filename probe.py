import json, struct, sys
sys.path.insert(0, ".")
import tso
from tso import tsharp, ssharp, f2u, u2f

W, H, D = 64, 4, 4
tex = bytearray()
for z in range(D):
    for y in range(H):
        for x in range(W):
            tex += bytes([x, 16 + y, 32 + z, 0x5a])

rs = [0.0, 0.05, 0.1, 0.124, 0.125, 0.126, 0.2, 0.25, 0.3, 0.37, 0.375, 0.38, 0.5, 0.6, 0.625, 0.7, 0.75, 0.8, 0.875, 0.9, 0.99, 1.0, 1.1, 1.5, -0.05, -0.2, -1.0]
uvs = [(1.0 / W, 1.0 / H), (2.5 / W, 1.5 / H), (3.25 / W, 2.75 / H), (0.1 / W, 0.1 / H)]
rows = [(f2u(u), f2u(v), f2u(r), 0) for (u, v) in uvs for r in rs]
s = ssharp(clamp=2, mag=1, minf=1)
out = {"rows": [[u2f(a), u2f(b), u2f(c)] for (a, b, c, _) in rows]}
for name, ttype, depth, dim in [("3d_tsharp_dim3d", 10, D - 1, "SQ_RSRC_IMG_3D"), ("2d_tsharp_dim3d", 9, 0, "SQ_RSRC_IMG_3D"), ("3d_tsharp_dim2d", 10, D - 1, "SQ_RSRC_IMG_2D")]:
    t = tsharp(56, W, H, ttype=ttype, depth=depth)
    vaddr = "v[4:6]" if dim == "SQ_RSRC_IMG_3D" else "v[4:5]"
    for dmask in (1, 2, 4):
        op = f"image_gather4_lz v[10:13], {vaddr}, s[8:15], s[16:19] dmask:0x{dmask:x} dim:{dim}"
        res = tso.sample(t, s, bytes(tex), rows, op=op)
        out[f"{name}_dmask{dmask}"] = [[round(u2f(w) * 255) for w in r] for r in res]
json.dump(out, open(sys.argv[1], "w"))
for i, row in enumerate(out["rows"]):
    line = " ".join(f"{k.split('_dmask')[0][:2]}{k[-1]}={out[k][i]}" for k in out if k != "rows")
    print(f"u={row[0]:.4f} v={row[1]:.4f} r={row[2]:+.3f} {line}")
