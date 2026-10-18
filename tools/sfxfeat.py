import numpy as np, sys, os, json
D=sys.argv[1]
rows=[l.rstrip('\n').split('\t') for l in open(os.path.join(D,'index.tsv'))]
out=[]
for i,name,rate,ns in rows:
    rate=int(rate); x=np.fromfile(os.path.join(D,i+'.raw'),dtype=np.int16).astype(np.float32)/32768
    if len(x)<rate*0.05: continue
    dur=len(x)/rate; env=np.abs(x); pk=env.max()
    if pk<0.02: continue
    win=1024; hop=512; fr=[]
    for s in range(0,len(x)-win,hop): fr.append(np.abs(np.fft.rfft(x[s:s+win]*np.hanning(win))))
    if not fr: continue
    S=np.array(fr)+1e-9; freqs=np.fft.rfftfreq(win,1/rate)
    pw=S**2; e=pw.sum(1); act=e>e.max()*0.01
    cen=(pw[act]*freqs).sum(1)/pw[act].sum(1)
    flat=np.exp(np.log(S[act]).mean(1))/S[act].mean(1)
    rmsf=np.sqrt(e); ppos=np.argmax(rmsf)/len(rmsf)
    att=np.argmax(rmsf>rmsf.max()*0.5)*hop/rate
    out.append(dict(i=int(i),name=name,dur=round(dur,2),cent=int(np.median(cen)),flat=round(float(np.median(flat)),3),ppos=round(float(ppos),2),att=round(att,3),act=round(act.sum()*hop/rate,2)))
json.dump(out,open(os.path.join(D,'feat.json'),'w'))
print(len(out))
