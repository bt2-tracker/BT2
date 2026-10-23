# Kjor fra prosjektmappa: python verktoy\lag_logo.py   (krever Pillow: pip install pillow)
# Skriver src/gfxdata/ft2_bernt_logo.h og verktoy/logo_forhaandsvisning.png.
import os
ROOT=os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
from PIL import Image
OW,H=154,32   # originalens logo-bredde
W=150         # Bernttracker-logoen (4 px smalere, saa boksen ved siden av faar plass til mer tekst)
BW=29         # boksen til hoeyre ("by Saddam Hussein & The Whole King Random"), originalen: 25 px
im=Image.open(os.path.join(ROOT,'src','gfxdata','bmp','ft2LogoBadges.bmp')); pal=im.getpalette(); px=im.load()
rgb2ft={(0,0,0):0,(0x53,0x97,0xFF):1,(0,0,0x67):2,(0x4B,0xFF,0xFF):3,(0xAB,0x77,0x87):4,(255,255,255):5,(0x7F,0x7F,0x7F):6,(0xAB,0xCD,0xEF):7,(0x73,0x37,0x47):8,(0xF7,0xCB,0xDB):9,(0x43,0x43,0x43):10,(0xD3,0xD3,0xD3):11}
def frame(f):
    return [[rgb2ft[tuple(pal[px[x,f*H+y]*3:px[x,f*H+y]*3+3])] for x in range(OW)] for y in range(H)]
F=[frame(i) for i in range(4)]

# Lager Bernttracker II-logoen ut fra originalgrafikken (ft2LogoBadges.bmp, CC BY-NC-SA 4.0, Magnus "Vogue" Hoegdahl).
# Bokstavene e, r, t, a, c, k og bakgrunnene (graa II-plate og rutemoenster) er originalens.
# Strekene er gjort 4 px tykke (originalen: 5 px) saa "Bernttracker" faar plass. B og n er tegnet i samme stil.
BCK,DESK,WHITE,G2=0,4,5,10

def comp_mask(f, x0, x1):
    # hvite piksler i kolonnene x0..x1, rad 7..22, som tilhoerer bokstaven
    return [''.join('W' if f[y][x]==5 else '.' for x in range(x0,x1+1)) for y in range(7,23)]

src=F[2]
orig={'e':comp_mask(src,124,136),'r':comp_mask(src,69,80),'t':comp_mask(src,45,55),
      'a':comp_mask(src,82,94),'c':comp_mask(src,96,108),'k':comp_mask(src,110,122)}
# fjern 1 kolonne i hver loddrett strek (5 -> 4 px)
drop={'e':[2,10],'r':[2],'t':[2],'a':[2,10],'c':[2,10],'k':[2]}
G={}
for ch,rows in orig.items():
    G[ch]=[''.join(c for i,c in enumerate(r) if i not in drop[ch]) for r in rows]

B=["WWWWWWWWWW.",
   "WWWWWWWWWWW",
   "WWWW...WWWW",
   "WWWW...WWWW",
   "WWWW...WWWW",
   "WWWW..WWWW.",
   "WWWWWWWWWW.",
   "WWWWWWWWWWW",
   "WWWW...WWWW",
   "WWWW...WWWW",
   "WWWW...WWWW",
   "WWWW...WWWW",
   "WWWW...WWWW",
   "WWWW...WWWW",
   "WWWWWWWWWWW",
   "WWWWWWWWWW."]
n=["...........",]*6+[
   ".WWWWWWWWW.",
   "WWWWWWWWWWW",
   "WWWW...WWWW",
   "WWWW...WWWW",
   "WWWW...WWWW",
   "WWWW...WWWW",
   "WWWW...WWWW",
   "WWWW...WWWW",
   "WWWW...WWWW",
   "WWWW...WWWW"]
G['B']=B; G['n']=n

WORD='Bernttracker'
def strip(rows):
    # fjern tomme kolonner til hoeyre/venstre
    w=len(rows[0]); cols=[i for i in range(w) if any(r[i]=='W' for r in rows)]
    return [r[cols[0]:cols[-1]+1] for r in rows]
for k in G: G[k]=strip(G[k])
widths=[len(G[c][0]) for c in WORD]
total=sum(widths)+len(WORD)-1+1
x0=1+((W-2)-total)//2
print('bredder',dict(zip(WORD,widths)),'total',total,'start',x0)
mask=set(); x=x0
for c in WORD:
    for yy,r in enumerate(G[c]):
        for xx,ch in enumerate(r):
            if ch=='W': mask.add((x+xx,7+yy))
    x+=len(G[c][0])+1
shadow={(x+dx,y+dy) for (x,y) in mask for dx,dy in ((1,1),(1,2))}-mask

# Bakgrunnene gjoeres 4 px smalere: kolonne 1-3 (venstre) og 152 (hoeyre) fjernes, rammen beholdes.
# Da staar den lille II-en nede til hoeyre fortsatt under siste "r", med luft til rammen.
KEEP=[0]+list(range(4,152))+[153]
assert len(KEEP)==W
def crop(f): return [[row[x] for x in KEEP] for row in f]
C=[crop(f) for f in F]

def old_text(f,shcol):
    return {(x,y) for y in range(7,25) for x in range(1,W-1) if f[y][x]==WHITE or (f[y][x]==shcol)}

def build(variant):
    f=[row[:] for row in C[0 if variant==0 else 2]]
    if variant==0:
        # rad 7..23: samme moenster som rad 24 (II-soeylene uten tekst)
        for y in range(7,24): f[y]=f[24][:]
        shcol=G2
    else:
        shcol=BCK
        holes=old_text(C[2],BCK)
        vis=[(x,y) for y in range(1,31) for x in range(1,W-1) if (x,y) not in holes]
        for (hx,hy) in holes:
            best=min(vis,key=lambda p:abs(p[0]-hx)+2.5*abs(p[1]-hy))
            f[hy][hx]=C[2][best[1]][best[0]]
    for (x,y) in shadow:
        if 1<=x<W-1 and 1<=y<31: f[y][x]=shcol
    for (x,y) in mask: f[y][x]=WHITE
    return f

def pressed(u):
    p=[[0]*W for _ in range(H)]
    for y in range(H):
        for x in range(W):
            p[y][x]=u[max(y-1,1)][max(x-1,1)]
    for x in range(W): p[0][x]=8
    for y in range(H): p[y][0]=8
    return p

u0=build(0); u1=build(1)
frames=[u0,pressed(u0),u1,pressed(u1)]

# ---- Boksen til hoeyre for logoen ----
# Samme stil som originalens "by Triton Prod." / "by Vogue 'n Mr.H": hvit pikseltekst med moerk skygge,
# lys kant oppe/venstre og moerk kant nede/hoeyre. Skriften er 3 piksler hoey (originalen: 4) for aa faa plass.
# Boksen ser lik ut hele tiden, ogsaa naar man trykker paa den.
FONT={
 'A':[".#.","###","#.#"],'B':["##.","###","##."],'D':["##.","#.#","##."],'E':["###","##.","###"],
 'G':["##.","#.#","###"],'H':["#.#","###","#.#"],'I':["#","#","#"],'K':["#.#","##.","#.#"],
 'L':["#..","#..","###"],'M':["#...#","##.##","#.#.#"],'N':["##.","#.#","#.#"],'O':["###","#.#","###"],
 'R':["##.","##.","#.#"],'S':[".##",".#.","##."],'T':["###",".#.",".#."],'U':["#.#","#.#","###"],
 'W':["#.#.#","#.#.#",".#.#."],'Y':["#.#",".#.",".#."],'&':[".#..","#.##","####"],' ':["..","..",".."],
}
LINES=["BY","SADDAM","HUSSEIN","& THE","WHOLE","KING","RANDOM"]
D1,D2=9,8  # PAL_DSKTOP1 (lys), PAL_DSKTOP2 (moerk)
def text_w(s): return sum(len(FONT[c][0]) for c in s)+len(s)-1
badge=[[DESK]*BW for _ in range(H)]
for x in range(BW): badge[0][x]=D1; badge[H-1][x]=D2
for y in range(H): badge[y][0]=D1; badge[y][BW-1]=D2
badge[0][BW-1]=D2
for i,s in enumerate(LINES):
    w=text_w(s); assert w+1<=BW-2, s
    x=1+((BW-2)-(w+1)+1)//2; y=2+i*4
    pts=[]
    for c in s:
        g=FONT[c]
        for yy,r in enumerate(g):
            for xx,ch in enumerate(r):
                if ch=='#': pts.append((x+xx,y+yy))
        x+=len(g[0])+1
    for (bx,by) in pts:
        if 0<bx+1<BW-1 and 0<by+1<H-1: badge[by+1][bx+1]=D2
    for (bx,by) in pts: badge[by][bx]=WHITE

RGB={0:(0,0,0),4:(171,119,135),5:(255,255,255),6:(127,127,127),8:(115,55,71),9:(247,203,219),10:(67,67,67),11:(211,211,211)}
im=Image.new('RGB',(W+BW,H*4))
for i,fr in enumerate(frames):
    for y in range(H):
        for x in range(W): im.putpixel((x,i*H+y),RGB[fr[y][x]])
        for x in range(BW): im.putpixel((W+x,i*H+y),RGB[badge[y][x]])
im.resize(((W+BW)*4,H*16),Image.NEAREST).save(os.path.join(ROOT,'verktoy','logo_forhaandsvisning.png'))

with open(os.path.join(ROOT,'src','gfxdata','ft2_bernt_logo.h'),'w',newline='\r\n') as fh:
    fh.write("// Bernttracker II-logo (%dx32 piksler, 4 bilder: vanlig, trykket, variant 2 vanlig, variant 2 trykket)\n" % W)
    fh.write("// og boksen ved siden av (%dx32 piksler, ser lik ut hele tiden).\n" % BW)
    fh.write("// Bearbeidet fra originalens ft2LogoBadges.bmp av Magnus \"Vogue\" Hoegdahl, lisens CC BY-NC-SA 4.0\n")
    fh.write("// (se src/gfxdata/bmp/LICENSE.txt). Kun ikke-kommersiell bruk.\n")
    fh.write("// Hver verdi er et nummer i FT2-paletten (PAL_*), saa logoen foelger valgt fargetema.\n")
    fh.write("// Laget med verktoy/lag_logo.py, ikke rediger for haand.\n\n#pragma once\n\n#include <stdint.h>\n\n")
    fh.write("#define BERNT_LOGO_W %d\n#define BERNT_BADGE_W %d\n\n" % (W,BW))
    fh.write("static const uint8_t berntLogo[4 * BERNT_LOGO_W * 32] =\n{\n")
    for fr in frames:
        for row in fr: fh.write("\t"+",".join(str(v) for v in row)+",\n")
    fh.write("};\n\n")
    fh.write("static const uint8_t berntBadge[BERNT_BADGE_W * 32] =\n{\n")
    for row in badge: fh.write("\t"+",".join(str(v) for v in row)+",\n")
    fh.write("};\n")
