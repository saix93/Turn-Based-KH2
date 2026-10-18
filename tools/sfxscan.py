import sys, struct, math
sys.path.insert(0,'.')
from vbf import VBF
from ima import decode
RATES=[4000,8000,11000,11025,16000,22050,24000,32000,44100,48000,96000]
v=VBF("F:/SteamLibrary/steamapps/common/FINAL FANTASY FFX&FFX-2 HD Remaster/data/FFX_Data.vbf")
name="ffx_data/gamedata/ps3data/sound_pc/sfx/us/0000_bank00.fsb"
d=v.read(name)
_,ver,n,shs,nts,ds,mode=struct.unpack_from('<4s6I',d,0)
o=0x3C; info=[]
for i in range(n):
    q=struct.unpack_from('<Q',d,o)[0]; o+=8
    extra=q&1; fr=RATES[(q>>1)&0xF]; ch=((q>>5)&1)+1; off=((q>>6)&0x0FFFFFFF)*16; ns=q>>34
    while extra:
        c=struct.unpack_from('<I',d,o)[0]; o+=4; extra=c&1; o+=(c>>1)&0xFFFFFF
    info.append([fr,ch,off,ns])
nb=0x3C+shs
names=[d[nb+struct.unpack_from('<I',d,nb+4*i)[0]:].split(b'\0')[0].decode() for i in range(n)]
base=0x3C+shs+nts
for i in range(n):
    fr,ch,off,ns=info[i]
    end = info[i+1][2] if i+1<n else ds
    seg=d[base+off:base+end]
    out,_=decode(seg,ch,0); s=out[0][:ns]
    if not s: continue
    dur=ns/fr; rms=math.sqrt(sum(x*x for x in s)/len(s))
    zc=sum(1 for a,b in zip(s,s[1:]) if (a<0)!=(b<0))/len(s)*fr/2
    # envelope: peak position
    pk=max(range(len(s)),key=lambda k:abs(s[k]))/len(s)
    print("%d\t%s\t%.2f\t%d\t%d\t%.2f\t%d" % (i,names[i],dur,rms,zc,pk,ch))
