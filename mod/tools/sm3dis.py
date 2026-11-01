#!/usr/bin/env python3
"""Minimal D3D9 shader model 2/3 bytecode disassembler, with CTAB constant names. Usage: sm3dis.py file.bin [...]"""
import struct, sys

OPS = {0:'nop',1:'mov',2:'add',3:'sub',4:'mad',5:'mul',6:'rcp',7:'rsq',8:'dp3',9:'dp4',10:'min',11:'max',12:'slt',13:'sge',
       14:'exp',15:'log',16:'lit',17:'dst',18:'lrp',19:'frc',20:'m4x4',21:'m4x3',22:'m3x4',23:'m3x3',24:'m3x2',25:'call',
       26:'callnz',27:'loop',28:'ret',29:'endloop',30:'label',31:'dcl',32:'pow',33:'crs',34:'sgn',35:'abs',36:'nrm',37:'sincos',
       38:'rep',39:'endrep',40:'if',41:'ifc',42:'else',43:'endif',44:'break',45:'breakc',46:'mova',47:'defb',48:'defi',
       64:'texcoord',65:'texkill',66:'texld',78:'expp',79:'logp',80:'cnd',81:'def',88:'cmp',89:'bem',90:'dp2add',
       91:'dsx',92:'dsy',93:'texldd',94:'setp',95:'texldl',96:'breakp'}
REG = {0:'r',1:'v',2:'c',3:'t',4:'rast',5:'attr',6:'o',7:'i',8:'oC',9:'oDepth',10:'s',11:'c',12:'c',13:'c',14:'b',15:'aL',
       16:'half',17:'misc',18:'l',19:'p'}
USAGE = {0:'position',1:'blendweight',2:'blendindices',3:'normal',4:'psize',5:'texcoord',6:'tangent',7:'binormal',8:'tessfactor',
         9:'positiont',10:'color',11:'fog',12:'depth',13:'sample'}
SAMP = {0:'',2:'2d',3:'cube',4:'volume'}

def regname(t):
    rt = ((t >> 28) & 7) | ((t >> 8) & 0x18)
    n = t & 0x7ff
    if rt == 17: return ['vPos', 'vFace'][n] if n < 2 else 'misc%d' % n
    if rt == 4: return ['oPos', 'oFog', 'oPts'][n] if n < 3 else 'rast%d' % n
    if rt == 3: return 'a%d' % n  # address reg in vs, texture in ps1 (rare here)
    return '%s%d' % (REG.get(rt, 'reg%d_' % rt), n)

def dst(t):
    m = (t >> 16) & 0xf; mod = (t >> 20) & 0xf
    s = regname(t)
    if m != 0xf: s += '.' + ''.join('xyzw'[i] for i in range(4) if m >> i & 1)
    pre = ('_sat' if mod & 1 else '') + ('_pp' if mod & 2 else '') + ('_centroid' if mod & 4 else '')
    return s, pre

def src(t, rel=None):
    sw = (t >> 16) & 0xff; mod = (t >> 24) & 0xf
    s = regname(t)
    if rel is not None: s = s[:1] + '[%s + %d]' % (regname(rel) + '.' + 'xyzw'[(rel >> 16) & 3], t & 0x7ff) if s[0] == 'c' else s + '[%s]' % regname(rel)
    comps = [(sw >> (2 * i)) & 3 for i in range(4)]
    if comps != [0, 1, 2, 3]:
        c = ''.join('xyzw'[x] for x in comps)
        if len(set(c)) == 1: c = c[0]
        s += '.' + c
    return {0: s, 1: '-' + s, 2: s + '_bias', 3: '-' + s + '_bias', 4: s + '_bx2', 5: '-' + s + '_bx2', 6: '(1-%s)' % s, 7: s + '_x2',
            8: '-' + s + '_x2', 9: s + '_dz', 10: s + '_dw', 11: 'abs(%s)' % s, 12: '-abs(%s)' % s, 13: '!' + s}.get(mod, s)

def ctab(data):
    if data[:4] != b'CTAB': return None
    d = data[4:]
    size, creator, ver, n, info, flags, target = struct.unpack_from('<7I', d, 0)
    def cstr(o): e = d.index(b'\0', o); return d[o:e].decode('latin1')
    out = []
    for i in range(n):
        name, rset, ridx, rcnt, _, tinfo, dflt = struct.unpack_from('<IHHHHII', d, info + i * 20)
        out.append((['bool', 'int4', 'float4', 'sampler'][rset] if rset < 4 else str(rset), ridx, rcnt, cstr(name)))
    return {'creator': cstr(creator), 'target': cstr(target), 'consts': out}

def dis(code):
    w = struct.unpack('<%dI' % (len(code) // 4), code[:len(code) // 4 * 4])
    ver = w[0]; kind = 'ps' if ver >> 16 == 0xffff else 'vs'; major = (ver >> 8) & 0xff
    lines = ['%s_%d_%d' % (kind, major, ver & 0xff)]; info = None
    i = 1
    while i < len(w):
        t = w[i]
        if t == 0x0000ffff: break
        op = t & 0xffff
        if op == 0xfffe:
            n = (t >> 16) & 0x7fff
            c = ctab(struct.pack('<%dI' % n, *w[i + 1:i + 1 + n]))
            if c: info = c
            i += 1 + n; continue
        n = (t >> 24) & 0xf if major >= 2 else None
        args = list(w[i + 1:i + 1 + n]); i += 1 + n
        name = OPS.get(op, 'op%d' % op)
        if op in (81,):
            d, _ = dst(args[0]); f = struct.unpack('<4f', struct.pack('<4I', *args[1:5]))
            lines.append('def %s, %s' % (d, ', '.join('%g' % x for x in f))); continue
        if op == 48:
            d, _ = dst(args[0]); lines.append('defi %s, %s' % (d, ', '.join(str(struct.unpack('<i', struct.pack('<I', x))[0]) for x in args[1:5]))); continue
        if op == 47:
            d, _ = dst(args[0]); lines.append('defb %s, %s' % (d, bool(args[1]))); continue
        if op == 31:
            u = args[0]; d, pre = dst(args[1]); rt = ((args[1] >> 28) & 7) | ((args[1] >> 8) & 0x18)
            if rt == 10: lines.append('dcl_%s %s' % (SAMP.get((u >> 27) & 0xf, '?'), d))
            else: lines.append('dcl_%s%s%s %s' % (USAGE.get(u & 0x1f, '?'), (u >> 16) & 0xf or '', pre, d))
            continue
        if op == 66 and ((t >> 16) & 0xff): name += ['', 'p', 'b'][(t >> 16) & 3]
        if op in (41, 45, 94): name += '_' + ['', 'gt', 'eq', 'ge', 'lt', 'ne', 'le'][(t >> 16) & 7]
        ops = []; pre = ''; k = 0; first = True
        hasdst = op not in (25, 26, 27, 28, 29, 30, 38, 39, 40, 41, 42, 43, 44, 45, 65, 96)
        while k < len(args):
            a = args[k]; k += 1
            if first and hasdst: s, pre = dst(a)
            else:
                rel = None
                if a & (1 << 13) and k < len(args) and major >= 2: rel = args[k]; k += 1
                s = src(a, rel)
            ops.append(s); first = False
        if t & (1 << 28): name = '(p0) ' + name
        lines.append('%s%s %s' % (name, pre, ', '.join(ops)))
    return lines, info

if __name__ == '__main__':
    for f in sys.argv[1:]:
        code = open(f, 'rb').read()
        lines, info = dis(code)
        print('==== %s (%d bytes)' % (f, len(code)))
        if info:
            print('// %s, %s' % (info['creator'], info['target']))
            for s, r, n, nm in info['consts']: print('//   %-8s %s%d%s  %s' % (s, 's' if s == 'sampler' else 'b' if s == 'bool' else 'i' if s == 'int4' else 'c', r, ('..%d' % (r + n - 1)) if n > 1 else '', nm))
        for l in lines: print('    ' + l)
