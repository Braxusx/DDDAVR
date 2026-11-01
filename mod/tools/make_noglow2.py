# Builds ddda_vr_mods/bbsrpg_core_noglow*.arc from the player's OWN nativePC/rom/bbsrpg_core.arc: the two sun-glow textures
# (effect\tex\cm\t_cm000_00_GM, t_cm040_00_GM; MT TEX v0x99, BC3 / DXT5, all mips) are dimmed in place: each block's colour
# endpoints and alpha values are multiplied by a factor that depends on where the block is in the effect atlas (RULES).
# No third-party assets.  Usage: make_noglow2.py <game arc> <out arc>
import struct, sys, zlib
from arc import parse
G, O = sys.argv[1:3]
# factor by atlas position (u, v in 0..1). cm000: 8x8 cells of 128 px; the starburst / cross-glint sprites (rows 2-3, columns 0-5)
# are removed, three sprites in row 1 (columns 0, 1, 3) are left as they are, the rest is dimmed (0.4 x colour x alpha ~ 16 %).
def rule000(u, v):
    cx, cy = int(u * 8), int(v * 8)
    if cy in (2, 3) and cx <= 5: return 0.0
    if cy == 1 and cx in (0, 1, 3): return 1.0
    return 0.4
RULES = { 'effect\\tex\\cm\\t_cm000_00_GM': rule000, 'effect\\tex\\cm\\t_cm040_00_GM': lambda u, v: 0.35 }
NAMES = tuple(RULES)
def alpha_block(b, k):
    a0, a1 = b[0], b[1]; bits = int.from_bytes(b[2:8], 'little')
    if a0 > a1: pal = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
    else: pal = [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
    v = [int(pal[(bits >> (3 * i)) & 7] * k + 0.5) for i in range(16)]
    hi, lo = max(v), min(v)
    if hi == lo: return bytes([hi, lo]) + b'\0' * 6
    pal = [hi, lo] + [((7 - i) * hi + i * lo) // 7 for i in range(1, 7)]
    bits = 0
    for i, x in enumerate(v): bits |= min(range(8), key=lambda j: abs(pal[j] - x)) << (3 * i)
    return bytes([hi, lo]) + bits.to_bytes(6, 'little')
def colour(c, k):
    r, g, b = c >> 11, (c >> 5) & 63, c & 31
    return (int(r * k + 0.5) << 11) | (int(g * k + 0.5) << 5) | int(b * k + 0.5)
def dim(tex, rule):
    d = bytearray(tex)
    assert d[:4] == b'TEX\0'
    p2 = struct.unpack_from('<I', d, 8)[0]; mips, W, H = p2 & 0x3f, (p2 >> 6) & 0x1fff, (p2 >> 19) & 0x1fff
    offs = [struct.unpack_from('<I', d, 16 + 4 * i)[0] for i in range(mips)]
    assert offs[0] == 16 + 4 * mips
    for m, o in enumerate(offs):
        w, h = max(1, W >> m), max(1, H >> m); bw, bh = (w + 3) // 4, (h + 3) // 4
        for by in range(bh):
            for bx in range(bw):
                k = rule((bx * 4 + 2) / w, (by * 4 + 2) / h)
                if k == 1.0: o += 16; continue
                d[o:o + 8] = alpha_block(d[o:o + 8], k)
                c0, c1 = struct.unpack_from('<HH', d, o + 8)
                struct.pack_into('<HH', d, o + 8, colour(c0, k), colour(c1, k))
                o += 16
    return bytes(d)
gd = open(G, 'rb').read(); _, _, n, E = parse(G)
ent = bytearray(); data = bytearray(); off = 0x8000; done = 0
for i, e in enumerate(E):
    raw, cs, ds = e['raw'], e['cs'], e['ds']
    if e['name'] in NAMES:
        t = dim(zlib.decompress(raw), RULES[e['name']]); raw = zlib.compress(t, 9); cs = len(raw); ds = (ds & 0xE0000000) | len(t); done += 1
    ent += gd[8 + 80 * i:8 + 80 * i + 64] + struct.pack('<IIII', e['type'], cs, ds, off + len(data)); data += raw
assert done == 2
out = gd[:8] + bytes(ent); out += b'\0' * (0x8000 - len(out)); out += bytes(data)
open(O, 'wb').write(out); print(O, len(out))
