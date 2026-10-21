import struct, sys
sys.path.insert(0, "tools/hw-oracle")
from hw_oracle import run
FLAGS = dict(ieee=0, dx10_clamp=1, denorm32=3, denorm16=3, round32=0, round16=0, fp16_overflow=0)
ROWS = 512
TEX = 0x10000
def f2u(x): return struct.unpack("<I", struct.pack("<f", x))[0]
def u2f(x): return struct.unpack("<f", struct.pack("<I", x))[0]
def tsharp(fmt, w, h, sw=0, ttype=9, dst=0xfac, base_level=0, last_level=0, max_mip=0, depth=0, min_lod=0):
    w1 = (min_lod << 8) | (fmt << 20) | (((w - 1) & 3) << 30)
    w2 = ((w - 1) >> 2) | ((h - 1) << 14)
    w3 = dst | (base_level << 12) | (last_level << 16) | (sw << 20) | (ttype << 28)
    w4 = depth
    w5 = max_mip << 4
    return [0, w1, w2, w3, w4, w5, 0, 0]
def ssharp(clamp=2, mag=1, minf=1, mip=0, w0extra=0, max_lod=0xfff, w2extra=0, w3=0):
    w0 = clamp | (clamp << 3) | (clamp << 6) | w0extra
    w1 = max_lod << 12
    w2 = (mag << 20) | (minf << 22) | (mip << 26) | w2extra
    return [w0, w1, w2, w3]
def body(op):
    return f"""
  s_add_u32 s28, s4, {16*ROWS}
  s_addc_u32 s29, s5, 0
  s_load_dwordx8 s[8:15], s[28:29], 0x0
  s_load_dwordx4 s[16:19], s[28:29], 0x20
  s_add_u32 s30, s4, {TEX}
  s_addc_u32 s31, s5, 0
  s_lshr_b64 s[30:31], s[30:31], 8
  s_waitcnt lgkmcnt(0)
  s_mov_b32 s8, s30
  s_and_b32 s31, s31, 0xff
  s_or_b32 s9, s9, s31
  {op}
  s_waitcnt vmcnt(0)
"""
SAMPLE_LZ = "image_sample_lz v[10:13], v[4:5], s[8:15], s[16:19] dmask:0xf dim:SQ_RSRC_IMG_2D"
def sample(t, s, tex, rows, op=SAMPLE_LZ):
    extra_len = max(TEX - 16 * ROWS + len(tex), 320000)
    e = bytearray(extra_len)
    e[0:32] = struct.pack("<8I", *t)
    e[32:48] = struct.pack("<4I", *s)
    o = TEX - 16 * ROWS
    e[o:o + len(tex)] = tex
    out = []
    for i in range(0, len(rows), ROWS):
        chunk = list(rows[i:i + ROWS])
        n = len(chunk)
        chunk += [(0, 0, 0, 0)] * (ROWS - n)
        r = run(body(op), chunk, bytes(e), wave64=True, **FLAGS)
        out += [tuple(x[:4]) for x in r[:n]]
    return out
