import struct, zlib
class VBF:
    def __init__(s, p):
        s.f = open(p,'rb')
        magic, hlen, n = struct.unpack('<4sIQ', s.f.read(16))
        assert magic == b'SRYK'
        hdr = s.f.read(hlen-16); o = 16*n
        ents = [struct.unpack_from('<IIQQQ', hdr, o+32*i) for i in range(n)]; o += 32*n
        nts = struct.unpack_from('<I', hdr, o)[0]; names = hdr[o+4:o+nts]; o += nts
        s.blocks = struct.unpack_from('<%dH' % ((len(hdr)-o)//2), hdr, o)
        s.files = {}
        for e in ents:
            nm = names[e[4]:names.index(b'\0', e[4])].decode()
            s.files[nm] = e
    def read(s, name, limit=None):
        blk, _, size, off, _ = s.files[name]
        want = size if limit is None else min(size, limit)
        out = bytearray(); s.f.seek(off); b = blk
        while len(out) < want:
            usz = min(0x10000, size - (b-blk)*0x10000)
            c = s.blocks[b] or 0x10000
            data = s.f.read(c)
            out += data if c == usz else zlib.decompress(data)
            b += 1
        return bytes(out[:want])
