import json, math, sys
def addr(c, n, m):
    if m == "clamp": return min(max(c, 0), n - 1)
    if m == "wrap": return c % n
    p = c % (2 * n); return p if p < n else 2 * n - 1 - p
W, H, D = 64, 4, 4
bad = tot = 0
for f, modes in (("result.json", {"3d_tsharp_dim3d": "clamp"}), ("result2.json", {"wrap": "wrap", "mirror": "mirror", "clamp": "clamp"})):
    o = json.load(open(f))
    for i, (u, v, r) in enumerate(o["rows"]):
        x0, y0, z = math.floor(u * W - 0.5), math.floor(v * H - 0.5), math.floor(r * D)
        for pre, m in modes.items():
            for dm, fn in ((1, lambda x, y, z: x), (2, lambda x, y, z: 16 + y), (4, lambda x, y, z: 32 + z)):
                k = f"{pre}_dmask{dm}" if pre.startswith("3d") else f"{pre}{dm}"
                e = [fn(addr(x, W, m), addr(y, H, m), addr(z, D, m)) for x, y in ((x0, y0 + 1), (x0 + 1, y0 + 1), (x0 + 1, y0), (x0, y0))]
                tot += 1; bad += o[k][i] != e
print(f"{tot - bad} of {tot} gathers match the model")
