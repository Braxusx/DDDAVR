import struct, zlib, sys
def parse(fn):
    d=open(fn,'rb').read()
    magic,ver,n=struct.unpack_from('<4sHH',d,0)
    out=[]; off=8
    if d[8:12]==b'\0\0\0\0' and ver!=7: off=12
    for i in range(n):
        name=d[off:off+64].split(b'\0')[0].decode('latin1')
        th,cs,ds,fo=struct.unpack_from('<IIII',d,off+64)
        out.append(dict(name=name,type=th,cs=cs,ds=ds,off=fo,raw=d[fo:fo+cs]))
        off+=80
    return magic,ver,n,out
if __name__=='__main__':
    for f in sys.argv[1:]:
        m,v,n,e=parse(f); print(f,m,v,n)
