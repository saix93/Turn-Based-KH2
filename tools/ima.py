import struct
STEP=[7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767]
IDX=[-1,-1,-1,-1,2,4,6,8]*2
def dec(nib, pred, idx):
    st=STEP[idx]; diff=st>>3
    if nib&4: diff+=st
    if nib&2: diff+=st>>1
    if nib&1: diff+=st>>2
    pred = pred-diff if nib&8 else pred+diff
    pred=max(-32768,min(32767,pred)); idx=max(0,min(88,idx+IDX[nib]))
    return pred, idx
def decode(data, ch, mode):
    bs=0x24*ch; out=[[] for _ in range(ch)]; lasts=[]
    for b in range(0, len(data)-bs+1, bs):
        blk=data[b:b+bs]; st=[]
        for c in range(ch):
            p,i,_=struct.unpack_from('<hBB', blk, 4*c); st.append([p,min(i,88)])
        hdr=[s[0] for s in st]
        if out[0]: lasts.append(abs(hdr[0]-out[0][-1]))
        nibs=[[] for _ in range(ch)]
        q=4*ch
        while q<bs:
            for c in range(ch):
                for k in range(4):
                    byte=blk[q+k]; nibs[c]+= [byte&15, byte>>4]
                q+=4
        for c in range(ch):
            p,i=st[c]
            if mode==0: out[c].append(p); ns=nibs[c][:63]
            else: ns=nibs[c]
            for n in ns: p,i=dec(n,p,i); out[c].append(p)
    return out, sum(lasts)/max(1,len(lasts))
