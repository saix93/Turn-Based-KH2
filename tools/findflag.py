import pefile, capstone, sys
pe=pefile.PE("E:/SteamLibrary/steamapps/common/KINGDOM HEARTS -HD 1.5+2.5 ReMIX-/KINGDOM HEARTS II FINAL MIX.exe", fast_load=True)
img=pe.get_memory_mapped_image()
md=capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.skipdata=True
for s in pe.sections:
    if not (s.Characteristics & 0x20000000): continue
    a,n=s.VirtualAddress,s.Misc_VirtualSize
    for ins in md.disasm(img[a:a+n], a):
        o=ins.op_str
        if ('+ 0x120]' in o or '+ 0x122]' in o) and ins.mnemonic in ('or','and','bts','btr','xor','mov') and ('0x80000' in o or '0xfff7ffff' in o or ', 8' in o and '0x122' in o or '0x13' in o):
            print(hex(ins.address), ins.mnemonic, o)
