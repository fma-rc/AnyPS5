import json, math
from fractions import Fraction as F
import numpy as np
B = "B"
def f32(x): return float(np.float32(x))
def reduced(c, mode):
    if math.isnan(c): return 0.0
    if math.isinf(c): c = -4.0 if c < 0 else 4.0
    if mode == 0: return f32(c - math.floor(c))
    if mode == 1: return f32(c - 2 * math.floor(c * 0.5))
    return min(max(c, -4.0), 4.0)
def snapped(c, n, off, mode):
    k = math.floor(F(reduced(c, mode)) * n * 256 + F(1, 2)) - off
    return k // 256
def fold(i): return -1 - i if i < 0 else i
def single(i, n, m):
    if m == 0: return i % n
    if m == 1:
        p = i % (2 * n); return p if p < n else 2 * n - 1 - p
    s = fold(i) if m & 1 else i
    if m < 6: return min(max(s, 0), n - 1)
    return s if 0 <= s < n else B
def foot(f, n, m):
    if (m & 6) != 4: return [single(f, n, m), single(f + 1, n, m)]
    src = [fold(f), fold(f + 1)] if m == 5 else [f, f + 1]
    low = min(src); pin = min(max(low, -1), n - 1)
    return [e if 0 <= e < n else B for e in (pin + s - low for s in src)]
def predict(u, v, r, W, H, D, m, P, S):
    X = foot(snapped(u, W, 128, m), W, m); Y = foot(snapped(v, H, 128, m), H, m)
    zm = (m & 1) | 2 if (m & 6) == 4 else m
    z = single(snapped(r, D, 0, m), D, zm)
    return [B if B in (x, y, z) else z * S + y * P + x for x, y in ((X[0], Y[1]), (X[1], Y[1]), (X[1], Y[0]), (X[0], Y[0]))]
def obs(c, k): return [c["result"]["1"][k][j] | c["result"]["2"][k][j] << 8 | c["result"]["4"][k][j] << 16 for j in range(4)]
good = tot = 0
for c in json.load(open("result3.json")):
    if not c["name"].startswith("clamp"): continue
    m = c["sampler"][0] & 7; bv = 0xffffff if c["sampler"][3] >> 30 == 2 else 0
    for k, (u, v, r) in enumerate(c["rows"]):
        p = [bv if e == B else e for e in predict(u, v, r, 8, 4, 4, m, 64, 256)]; tot += 1; good += p == obs(c, k)
print("oracle3 clamp cases", good, "/", tot); good = tot = 0
for c in json.load(open("result4.json")):
    for k, (u, v, r) in enumerate(c["rows"]):
        p = predict(u, v, r, c["W"], c["H"], c["D"], c["clamp"], 64, 64 * c["H"]); tot += 1; good += p == obs(c, k)
print("oracle4", good, "/", tot); good = tot = 0
for c in json.load(open("result5.json")):
    for k, (u, v, r) in enumerate(c["rows"]):
        p = predict(u, v, r, c["W"], 2, 2, 2, c["pitch"], c["pitch"] * 2); tot += 1; good += p == obs(c, k)
print("oracle5", good, "/", tot); good = tot = 0
for c in json.load(open("result6.json")):
    for k, (u, v, r) in enumerate(c["rows"]):
        p = [0xffffff if e == B else e for e in predict(u, v, r, 7, 5, 3, c["clamp"], 64, 320)]; tot += 1; ok = p == obs(c, k); good += ok
        if not ok: print("  miss clamp", c["clamp"], (u, v, r), p, obs(c, k))
print("oracle6", good, "/", tot)
good = tot = 0
for c in json.load(open("result7.json")):
    for k, row in enumerate(c["rows"]):
        u, v, r = (float(np.frombuffer(np.uint32(w).tobytes(), np.float32)[0]) for w in row)
        p = [0xffffff if e == B else e for e in predict(u, v, r, c["W"], c["H"], c["D"], c["clamp"], 64, 64 * c["H"])]; tot += 1; ok = p == obs(c, k); good += ok
        if not ok: print("  miss clamp", c["clamp"], row, p, obs(c, k))
print("oracle7 ties, tiny and denormal coordinates", good, "/", tot)
