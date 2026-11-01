# payload -> payload_lzma: LZMA "alone" (.lzma) files with the real uncompressed size in the header
import lzma, struct, os, sys
here = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'launcher')
src, dst = os.path.join(here, 'payload'), os.path.join(here, 'payload_lzma')
os.makedirs(dst, exist_ok=True)
for f in sorted(os.listdir(src)):
    d = open(os.path.join(src, f), 'rb').read(); o = os.path.join(dst, f)
    if os.path.exists(o) and os.path.getmtime(o) > os.path.getmtime(os.path.join(src, f)): continue
    c = bytearray(lzma.compress(d, format=lzma.FORMAT_ALONE, filters=[{'id': lzma.FILTER_LZMA1, 'preset': 9 | lzma.PRESET_EXTREME, 'dict_size': 1 << 24}]))
    c[5:13] = struct.pack('<Q', len(d))
    open(o, 'wb').write(c); print(f, len(d), '->', len(c))
