import sys, pefile, struct, capstone
pe = pefile.PE(sys.argv[1], fast_load=True)
img = pe.get_memory_mapped_image()
targets = [int(x,16) for x in sys.argv[2:]]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
hits=set()
for s in pe.sections:
    if not (s.Characteristics & 0x20000000): continue
    a, n = s.VirtualAddress, s.Misc_VirtualSize
    for i in range(a, a+n-4):
        d = struct.unpack_from('<i', img, i)[0]
        for k in (0,1,2,4):
            if i+4+k+d in targets:
                # try decode starting a few bytes before
                for back in range(2,8):
                    st=i-back
                    ins = next(md.disasm(img[st:st+16], st), None)
                    if ins and ins.address+ins.size == i+4+k and 'rip' in ins.op_str:
                        hits.add((st, ins.mnemonic+' '+ins.op_str)); break
for h in sorted(hits): print("%X  %s" % h)
