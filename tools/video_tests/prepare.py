#!/usr/bin/env python3
"""Turn generated elementary streams into Tegra picture-info jobs with oracle pixels."""
import pathlib,struct,re,sys
out=pathlib.Path(sys.argv[1])
class Reader:
 def __init__(self,b):
  self.b=re.sub(b'\x00\x00\x03',b'\x00\x00',b);self.p=0
 def u(self,n):
  r=0
  for _ in range(n):r=r*2+((self.b[self.p//8]>>(7-self.p%8))&1);self.p+=1
  return r
 def ue(self):
  n=0
  while self.u(1)==0:n+=1
  return (1<<n)-1+self.u(n)
 def se(self):
  x=self.ue();return (x+1)//2 if x&1 else -x//2
nals=[n for n in re.split(b'\x00\x00(?:\x00)?\x01',(out/'input.h264').read_bytes()) if n]
s=Reader(next(n[1:] for n in nals if n[0]&31==7));profile=s.u(8);s.u(16);assert s.ue()==0
chroma=1;bypass=0
if profile>=100:
 chroma=s.ue();assert s.ue()==s.ue()==0;bypass=s.u(1);assert s.u(1)==0
lognum=s.ue();poc=s.ue();assert poc==0
logpoc=s.ue();refs=s.ue();s.u(1);w=(s.ue()+1)*16;h=(s.ue()+1)*16;frame=s.u(1);assert frame==1
direct=s.u(1)
p=Reader(next(n[1:] for n in nals if n[0]&31==8));assert p.ue()==p.ue()==0
entropy=p.u(1);order=p.u(1);assert p.ue()==0
l0=p.ue();l1=p.ue();weighted=p.u(1);bipred=p.u(2);qp=p.se();p.se();chromaOffset=p.se();deblock=p.u(1);constrained=p.u(1);redundant=p.u(1)
transform=p.u(1);assert p.u(1)==0
secondOffset=p.se()
base=bytearray(0x2fc)
def put(p,o,v):struct.pack_into('<I',p,o,v&0xffffffff)
for o,v in {0x58:logpoc,0x60:frame,0x64:w//16,0x68:h//16,0x70:entropy,0x74:order,0x78:l0,0x7c:l1,0x80:deblock,0x84:redundant,0x88:transform,0x8c:w,0x90:w,0x2d0:bypass<<1}.items():put(base,o,v)
base[0x1c0:0x2a0]=bytes([16])*224
flags=(direct<<1)|(weighted<<2)|(constrained<<3)|(lognum<<8)|(chroma<<12)|(poc<<14)|((qp&63)<<16)|((chromaOffset&31)<<22)|((secondOffset&31)<<27)|(bipred<<32)
yuv=(out/'expected_h264.yuv').read_bytes();frame_size=w*h*3//2
jobs=[]
for nal in nals:
 t=nal[0]&31
 if t not in (1,5):continue
 r=Reader(nal[1:]);assert r.ue()==0;r.ue();assert r.ue()==0
 frameNum=r.u(lognum+4)
 if t==5:r.ue()
 picOrder=r.u(logpoc+4)
 picture=bytearray(base);payload=b'\x00\x00\x00\x01'+nal;put(picture,0x48,len(payload));put(picture,0x4c,1)
 idx=len(jobs)%17
 struct.pack_into('<Q',picture,0xb0,flags|(idx<<34)|(frameNum<<46)|((nal[0]>>5!=0)<<4))
 jobs.append((picture,payload,yuv[(picOrder//2)*frame_size:(picOrder//2+1)*frame_size]))
def save(name,codec,w,h,jobs):
 with (out/name).open('wb') as f:
  f.write(struct.pack('<IIII',codec,w,h,len(jobs)))
  for p,b,y in jobs:
   assert len(y)==w*h*3//2
   f.write(struct.pack('<III',len(p),len(b),len(y)));f.write(p);f.write(b);f.write(y)
save('h264.jobs',3,w,h,jobs)
b=(out/'input.ivf').read_bytes();w,h=struct.unpack_from('<HH',b,12);i=32;jobs=[];yuv=(out/'expected_vp8.yuv').read_bytes();frame_size=w*h*3//2
while i<len(b):
 size=struct.unpack_from('<I',b,i)[0];payload=b[i+12:i+12+size];i+=12+size
 tag=int.from_bytes(payload[:3],'little');key=not(tag&1);picture=bytearray(0xc0)
 put(picture,0x38,w|(h<<16));picture[0x3c]=int(key);picture[0x3d]=(tag>>1)&7;put(picture,0x40,tag>>5)
 payload=payload[10 if key else 3:];put(picture,0x48,len(payload));n=len(jobs)
 jobs.append((picture,payload,yuv[n*frame_size:(n+1)*frame_size]))
save('vp8.jobs',5,w,h,jobs)
print('Prepared H264 (with reordered B frames) and VP8 decode jobs')
