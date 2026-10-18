import sys, struct
from vbf import VBF
v = VBF(sys.argv[1]); name = sys.argv[2]
d = v.read(name, 4*1024*1024)
print(d[:4], struct.unpack_from('<IIII', d, 4))
n, shdr, data, ver, mode = struct.unpack_from('<IIIII', d, 4)
o = 48
for i in range(n):
    sz = struct.unpack_from('<H', d, o)[0]
    nm = d[o+2:o+32].split(b'\0')[0].decode('latin1')
    lensamp, comp = struct.unpack_from('<II', d, o+32)[0], struct.unpack_from('<I', d, o+36)[0]
    mode_ = struct.unpack_from('<I', d, o+48)[0]
    freq = struct.unpack_from('<i', d, o+52)[0]
    print(i, nm, lensamp, comp, hex(mode_), freq)
    o += sz
