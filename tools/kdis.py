import sys, pefile, capstone
pe = pefile.PE(sys.argv[1], fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
rva = int(sys.argv[2],16); n = int(sys.argv[3]) if len(sys.argv)>3 else 120
data = pe.get_memory_mapped_image()[rva:rva+n*8]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for i,ins in enumerate(md.disasm(data, rva)):
    if i>=n: break
    print("%08X  %s %s" % (ins.address, ins.mnemonic, ins.op_str))
