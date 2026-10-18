import struct, sys
p = sys.argv[1]
with open(p,'rb') as f:
    magic, hlen, n = struct.unpack('<4sIQ', f.read(16))
    hdr = f.read(hlen-16)
md5 = hdr[:16*n]; o = 16*n
ents = []
for i in range(n):
    blk, unk, size, off, noff = struct.unpack_from('<IIQQQ', hdr, o); o += 32
    ents.append((blk,unk,size,off,noff))
ntsize = struct.unpack_from('<I', hdr, o)[0]
names = hdr[o+4:o+ntsize]
print('names table size', ntsize, 'remaining', len(hdr)-o-ntsize)
for e in ents[:5]+ents[-3:]:
    s = names[e[4]:names.index(b'\0', e[4])].decode()
    print(e, s)
flt = sys.argv[2] if len(sys.argv)>2 else None
if flt:
    for e in ents:
        s = names[e[4]:names.index(b'\0', e[4])].decode()
        if flt in s: print(e[2], s)
