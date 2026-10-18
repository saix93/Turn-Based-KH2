import sys, struct
from vbf import VBF
v = VBF(sys.argv[1]); name = sys.argv[2]
d = v.read(name, 1*1024*1024)
_, ver, n, shs, nts, ds, mode = struct.unpack_from('<4s6I', d, 0)
print('ver',ver,'n',n,'mode',mode)
hs = 0x3C if ver==1 else 0x40
o = hs; info=[]
for i in range(n):
    q = struct.unpack_from('<Q', d, o)[0]; o += 8
    extra = q & 1; freq=(q>>1)&0xF; ch=((q>>5)&1)+1; off=((q>>6)&0x0FFFFFFF)*16; ns=q>>34
    while extra:
        c = struct.unpack_from('<I', d, o)[0]; o+=4
        extra = c & 1; ln=(c>>1)&0xFFFFFF; o+=ln
    info.append((ch,off,ns))
nb = hs+shs
for i in range(n):
    no = struct.unpack_from('<I', d, nb+4*i)[0]
    s = d[nb+no:d.index(b'\0', nb+no)].decode('latin1')
    print(i, s, info[i])
