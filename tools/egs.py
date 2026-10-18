import struct, hashlib, zlib, os
MK = bytes.fromhex("7E8897550B06F108EBBB141CD87AEC4134B2A346EF6BFEE1CF53A50512D28E524A80E981B0F0B49CFF0F1513DA734E77BED730E5F65A113767BC836F2776D0CD690D2E514290B8B64CADCE5B1A1FF5AF01F85E3A6E688BE89FC9D9269229C833983254D444256 6AC5F9921E48F1DC2D5A462F90261DE59E7079AFA2F953F86D378A775EDD62D6487BDC7C1AAF28C17CB318AC3CC04EE6AAB5C2270CA9E716D85455DB9A9A61047FB827D847BC6E238FC2B0E209DC5F339A8A06558437CE336187249 79AED17440C4914F2463BFBA239650B357DF1E03487F354D3EE6A1DD093C3D3B568D932A9B4B0C28B1E0608919DB2CF76CB51B94C0DCEAB70AF416FDA200".replace(" ",""))
SK = [1,0,0,0,2,0,0,0,4,0,0,0,8,0,0,0,0x10,0,0,0,0x20,0,0,0,0x40,0,0,0,0x80,0,0,0,0x1B,0,0,0,0x36,0,0,0]
def genkey(seed, passes=10):
    k = bytearray(0xB0)
    for i,b in enumerate(seed): k[i] = i if b==0 else b
    for i in range(passes*4):
        fr = k[0x0C+i*4:0x10+i*4]
        if i%4==0:
            fr = bytes([MK[fr[1]]^SK[i], MK[fr[2]]^SK[i+1], MK[fr[3]]^SK[i+2], MK[fr[0]]^SK[i+3]])
        for j in range(4): k[0x10+i*4+j] = k[i*4+j]^fr[j]
    return bytes(k)
def decchunk(key, d, idx, passes=10):
    n = min(len(d)-idx, 16)
    for i in range(passes, -1, -1):
        for j in range(n): d[idx+j] ^= key[0x10*i+j]
class Pkg:
    def __init__(s, hed):
        s.pkg = open(hed[:-4]+'.pkg','rb'); s.ent = {}
        h = open(hed,'rb').read()
        for i in range(0, len(h), 0x20):
            md5, off, dl, al = struct.unpack_from('<16sqii', h, i)
            s.ent[md5] = (off, dl, al)
    def has(s, name): return hashlib.md5(name.encode()).digest() in s.ent
    def read(s, name):
        off, dl, al = s.ent[hashlib.md5(name.encode()).digest()]
        s.pkg.seek(off); seed = s.pkg.read(16)
        declen, rcount, complen, _ = struct.unpack('<iiii', seed)
        s.pkg.seek(off+16+rcount*0x30)
        ln = complen if complen>=0 else declen
        d = bytearray(s.pkg.read(ln))
        if complen > -2:
            key = genkey(seed)
            for i in range(0, min(ln,0x100), 16): decchunk(key, d, i)
        if complen > -1: d = zlib.decompress(bytes(d[2:]), -15)
        return bytes(d)
