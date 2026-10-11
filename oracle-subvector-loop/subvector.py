import sys

sys.path.insert(0, "tools/hw-oracle")
from hw_oracle import run, target

# One wave64 wave per run. v4 != 0 selects the lanes in EXEC before the loop.
# Outputs per lane: v10 passes this lane executed, v11 pass index of its last pass (1-based),
# v12 s38 seen in that pass, v13/v14 EXEC lo/hi seen in that pass,
# v15 s38 after the loop, v16/v17 EXEC lo/hi after the loop, v18 passes in total.
BODY = """
  s_mov_b32 s20, 0
  s_mov_b32 s38, 0x5a5a5a5a
  v_cmpx_ne_u32 0, v4
  s_subvector_loop_begin s38, .Lafter
.Lbody:
  s_add_u32 s20, s20, 1
  v_add_nc_u32 v10, 1, v10
  v_mov_b32 v11, s20
  v_mov_b32 v12, s38
  s_mov_b64 s[22:23], exec
  v_mov_b32 v13, s22
  v_mov_b32 v14, s23
  s_cmp_ge_u32 s20, 4
  s_cbranch_scc1 .Lafter
@TAIL@
  s_subvector_loop_end s38, .Lbody
.Lafter:
  s_mov_b64 s[24:25], exec
  s_mov_b64 exec, -1
  v_mov_b32 v15, s38
  v_mov_b32 v16, s24
  v_mov_b32 v17, s25
  v_mov_b32 v18, s20
"""

VARIANTS = {
    "exec kept by the body": "",
    "body ends with s_mov_b64 exec, -1": "  s_mov_b64 exec, -1",
}

MASKS = {
    "all 64 lanes": lambda lane: True,
    "low half only": lambda lane: lane < 32,
    "high half only": lambda lane: lane >= 32,
    "no lane": lambda lane: False,
    "lanes 3, 40": lambda lane: lane in (3, 40),
    "lane 3": lambda lane: lane == 3,
    "lane 40": lambda lane: lane == 40,
}

print("target", target())
for variant, tail in VARIANTS.items():
    for name, mask in MASKS.items():
        rows = [(1 if mask(lane) else 0, 0, 0, 0) for lane in range(64)]
        out = run(BODY.replace("@TAIL@", tail), rows, wave64=True, ieee=0, dx10_clamp=1, denorm32=0, denorm16=3, round32=0, round16=0, fp16_overflow=0)
        lanes = {}
        for lane, row in enumerate(out):
            lanes.setdefault(tuple(row[:9]), []).append(lane)
        print(f"== {variant} | initial EXEC: {name}")
        for values, members in lanes.items():
            span = f"{members[0]}-{members[-1]}" if len(members) > 1 else f"{members[0]}"
            print(f"  lanes {span} ({len(members)}): passes={values[0]} lastPass={values[1]} s38InPass={values[2]:08x} execInPass={values[4]:08x}:{values[3]:08x} | after: s38={values[5]:08x} exec={values[7]:08x}:{values[6]:08x} totalPasses={values[8]}")
