import struct, sys
sys.path.insert(0, '.')
from egs import Pkg
KH = "E:/SteamLibrary/steamapps/common/KINGDOM HEARTS -HD 1.5+2.5 ReMIX-/Image/dt/kh2_first.hed"
def bar(d):
    assert d[:4] == b'BAR\x01', d[:4]
    n = struct.unpack_from('<i', d, 4)[0]; out = []
    for i in range(n):
        t, dup, name, off, size = struct.unpack_from('<HH4sII', d, 16+16*i)
        out.append((t, name.rstrip(b'\0').decode('latin1'), d[off:off+size]))
    return out
def objentry(p):
    d = p.read("00objentry.bin")
    ver, n = struct.unpack_from('<ii', d, 0); objs = {}
    for i in range(n):
        o = 8 + i*0x60
        oid = struct.unpack_from('<I', d, o)[0]; typ = d[o+4]
        nm = d[o+8:o+0x28].split(b'\0')[0].decode('latin1')
        objs[oid] = (typ, nm)
    return objs
def scripts(d):
    progs = {}; o = 0
    while o + 4 <= len(d):
        pid, ln = struct.unpack_from('<HH', d, o)
        if pid == 0xFFFF or ln == 0: break
        body = d[o+4:o+ln]; ops = []; q = 0
        while q + 4 <= len(body):
            op, cnt = struct.unpack_from('<HH', body, q)
            params = body[q+4:q+4+4*cnt]; ops.append((op, params)); q += 4 + 4*cnt
        progs[pid] = ops; o += ln
    return progs
def spawn(d):
    if len(d) < 8: return []
    typ, n = struct.unpack_from('<ii', d, 0); o = 8; groups = []
    for _ in range(n):
        t, fl, gid, ne, nev, nwp, rp, nsig = struct.unpack_from('<BBHHHHHi', d, o)
        o += 0x2C; ents = []
        for _ in range(ne):
            mid = struct.unpack_from('<I', d, o)[0]; x,y,z = struct.unpack_from('<fff', d, o+4)
            ents.append((mid, round(x), round(y), round(z))); o += 0x40
        o += nev*0x40
        for _ in range(nwp):
            cnt = struct.unpack_from('<H', d, o+2)[0]; o += 8 + 12*cnt
        o += rp*0x10 + nsig*8
        groups.append((gid, ents))
    return groups
