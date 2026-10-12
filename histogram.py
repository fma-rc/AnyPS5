#!/usr/bin/env python3
"""Opcode histogram over the census disassembly (<ID>/asm/*.asm written by census.py).

usage: histogram.py [evidence dir]  -> histogram.tsv (family, opcode, mnemonic, wave32, wave64, shaders32, shaders64)
and histogram-by-title.tsv. The wave size is the last token of each file's first line. Mnemonics come from the
decoder's opcode tables; families without a flat table (scalar ops, VINTRP, EXP) keep family + hardware opcode.
"""
import collections
import pathlib
import re
import sys

REPO = pathlib.Path("<AnyPS5 checkout>/core/shader/recompiler/RdnaDecoder/src")
FAMILY = ["Unknown", "SOP1", "SOP2", "SOPK", "SOPC", "SOPP", "VOP1", "VOP2", "VOP3", "VOP3P", "VOPC", "VINTRP", "SMEM",
          "MUBUF", "MTBUF", "FLAT", "DS", "MIMG", "EXP"]


def snake(name):
    return "_".join(t.lower() for t in re.findall(r"[A-Z][a-z]*[0-9]*[a-z]*", name))


def table(path, var):
    text = (REPO / path).read_text()
    start = text.index(f"{var}[] = {{")
    body = text[start:text.index("};", start)]
    return {int(m.group(1), 16): snake(m.group(2)) for m in re.finditer(r"\{0x([0-9a-fA-F]+)u?,\s*RdnaOpcode::(\w+)", body)}


def tables():
    v, m = "RdnaVectorOpDecoder.cpp", "RdnaMemoryOpDecoder.cpp"
    vop2, vop1, vopc = table(v, "vop2Opcodes"), table(v, "vop1Opcodes"), table(v, "vopcOpcodes")
    v3 = {0x180 + op: n for op, n in table(v, "vop3EncodedVop1Opcodes").items()}
    v3.update({0x100 + op: n for op, n in vop2.items()})
    v3.update(vopc)
    v3.update(table(v, "vop3Opcodes"))
    return {6: vop1, 7: vop2, 8: v3, 9: table(v, "vop3pOpcodes"), 10: vopc, 12: table(m, "smemOpcodes"),
            13: table(m, "mubufOpcodes"), 14: table(m, "mtbufOpcodes"), 15: table(m, "flatOpcodes"),
            16: table(m, "dsOpcodes"), 17: table("RdnaImageOpDecoder.cpp", "imageOpcodes")}


INSTR = re.compile(r"^raw=[0-9a-f]+ 0x[0-9a-f]+: family=(\d+) opcode=0x([0-9a-f]+)")
root = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parent
tabs = tables()
count = collections.Counter()
shaders = collections.defaultdict(set)
bytitle = collections.Counter()
files = collections.Counter()
for asm in sorted(root.glob("PPSA*/asm/*.asm")):
    lines = asm.read_text(errors="replace").splitlines()
    wave = lines[0].rsplit(None, 1)[-1] if lines else ""
    if wave not in ("wave32", "wave64"):
        files["no wave size"] += 1
        continue
    files[wave] += 1
    title = asm.parts[-3]
    for line in lines:
        m = INSTR.match(line)
        if not m:
            continue
        key = (int(m.group(1)), int(m.group(2), 16))
        count[key, wave] += 1
        shaders[key, wave].add(asm.name)
        bytitle[title, key, wave] += 1


def name(key):
    return tabs.get(key[0], {}).get(key[1], "")


keys = sorted({k for k, _ in count}, key=lambda k: -(count[k, "wave32"] + count[k, "wave64"]))
with open(root / "histogram.tsv", "w") as f:
    f.write("family\topcode\tmnemonic\twave32\twave64\tshaders_wave32\tshaders_wave64\n")
    for k in keys:
        f.write(f"{FAMILY[k[0]]}\t0x{k[1]:x}\t{name(k)}\t{count[k, 'wave32']}\t{count[k, 'wave64']}\t"
                f"{len(shaders[k, 'wave32'])}\t{len(shaders[k, 'wave64'])}\n")
with open(root / "histogram-by-title.tsv", "w") as f:
    f.write("title\tfamily\topcode\tmnemonic\twave\tinstructions\n")
    for (title, k, wave), c in sorted(bytitle.items(), key=lambda i: (i[0][0], -i[1])):
        f.write(f"{title}\t{FAMILY[k[0]]}\t0x{k[1]:x}\t{name(k)}\t{wave}\t{c}\n")
print(dict(files), "instructions", sum(count.values()), "distinct", len(keys),
      "named", sum(1 for k in keys if name(k)))
