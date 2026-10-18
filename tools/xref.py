import sys, pefile, struct
pe = pefile.PE(sys.argv[1], fast_load=True)
img = pe.get_memory_mapped_image()
targets = [int(x,16) for x in sys.argv[2:]]
for s in pe.sections:
    if not (s.Characteristics & 0x20000000): continue
    a, n = s.VirtualAddress, s.Misc_VirtualSize
    buf = img[a:a+n]
    i = buf.find(b'\xe8')
    while i != -1 and i+5 <= len(buf):
        rel = struct.unpack_from('<i', buf, i+1)[0]
        t = a+i+5+rel
        if t in targets: print("call %X -> %X" % (a+i, t))
        i = buf.find(b'\xe8', i+1)
