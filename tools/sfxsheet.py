import numpy as np, json, os, sys
from PIL import Image, ImageDraw
D=sys.argv[1]; F=json.load(open(os.path.join(D,'feat.json')))
rate={l.split('\t')[0]:int(l.split('\t')[2]) for l in open(os.path.join(D,'index.tsv'))}
C={
 'fire':    lambda f: 0.9<=f['dur']<=2.6 and f['flat']>0.25 and 1200<f['cent']<4000 and f['act']>0.6,
 'thunder': lambda f: 0.7<=f['dur']<=3 and f['att']<0.03 and f['flat']>0.3 and f['cent']>2500 and f['ppos']<0.25,
 'blizzard':lambda f: 0.8<=f['dur']<=2.6 and f['cent']>4500 and 0.08<f['flat']<0.4,
 'water':   lambda f: 0.8<=f['dur']<=2.6 and 600<f['cent']<2000 and f['flat']>0.25 and f['act']>0.6,
 'cure':    lambda f: 1.0<=f['dur']<=3.2 and f['flat']<0.25 and f['cent']>1800 and f['act']>0.6,
 'buff':    lambda f: 0.6<=f['dur']<=2.2 and f['flat']<0.2 and 1000<f['cent']<3000 and f['act']>0.5,
 'debuff':  lambda f: 0.6<=f['dur']<=2.2 and f['flat']<0.15 and f['cent']<1100 and f['act']>0.5,
 'scan':    lambda f: 0.2<=f['dur']<=0.9 and f['flat']<0.25 and f['cent']>1500,
}
def spec(i):
    x=np.fromfile(os.path.join(D,f"{i}.raw"),dtype=np.int16).astype(np.float32)/32768
    win=512; hop=256
    fr=[np.abs(np.fft.rfft(x[s:s+win]*np.hanning(win))) for s in range(0,max(1,len(x)-win),hop)]
    S=20*np.log10(np.array(fr).T+1e-6); S=np.clip((S+80)/80,0,1)[::-1]
    img=Image.fromarray((S*255).astype(np.uint8)).resize((300,120))
    return img
for k,fn in C.items():
    c=[f for f in F if fn(f)]
    c.sort(key=lambda f:-f['act'])
    c=c[:12]
    W=Image.new('RGB',(4*310,3*150),(0,0,0)); d=ImageDraw.Draw(W)
    for j,f in enumerate(c):
        x,y=(j%4)*310,(j//4)*150
        W.paste(spec(f['i']).convert('RGB'),(x,y+25))
        d.text((x+2,y+2),f"#{f['i']} n{f['name']} {f['dur']}s c{f['cent']} fl{f['flat']}",fill=(255,255,0))
    W.save(os.path.join(D,f"sheet_{k}.png")); print(k,len([f for f in F if fn(f)]),[f['i'] for f in c])
