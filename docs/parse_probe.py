#!/usr/bin/env python3
"""Faithful port of ModelParser.cpp's LOD0 parse path, with an OOB-RAISING reader that
reproduces where the ORIGINAL (unhardened) C++ reader would read past the buffer and
segfault. Runs on the crashing models to pinpoint the exact bad read.

Reader.raise_oob=True → any u8/u16/u32/f32 read past the buffer raises OOB(offset,size).
That is exactly the point the old `d[o]` pointer arithmetic would dereference wild memory.
"""
import struct, sys

STRIDE_SIMPLE, STRIDE_SKINNED = 36, 44
SEM_POSITION, SEM_TEXCOORD_0, SEM_TEXCOORD_1 = 0, 1, 2
SEM_COLOR_0, SEM_COLOR_1, SEM_NORMAL, SEM_TANGENT = 7, 8, 9, 10
SEM_BLENDINDICES, SEM_BLENDWEIGHTS = 11, 12
FMT_SIZE = {1:12, 2:4, 4:4, 5:4, 7:4, 8:4}
SIG = bytes.fromhex("000000000100000000000000")

class OOB(Exception):
    def __init__(self, off, size, n, where):
        super().__init__(f"OOB read off={off} size={size} bufN={n} at {where}")
        self.off, self.size, self.n, self.where = off, size, n, where

class R:
    def __init__(self, b, name):
        self.d = b; self.n = len(b); self.name = name; self.where = "?"
    def _chk(self, o, s):
        if o < 0 or s < 0 or o + s > self.n:
            raise OOB(o, s, self.n, f"{self.name}:{self.where}")
    def in_(self, o, s): return self.d is not None and o >= 0 and s >= 0 and o + s <= self.n
    def u8(self, o):  self._chk(o,1); return self.d[o]
    def u16(self, o): self._chk(o,2); return self.d[o] | (self.d[o+1]<<8)
    def u32(self, o): self._chk(o,4); return int.from_bytes(self.d[o:o+4],'little')
    def i32(self, o): v=self.u32(o); return v-0x100000000 if v>=0x80000000 else v
    def f32(self, o): self._chk(o,4); return struct.unpack_from('<f', self.d, o)[0]

def formatSize(f): return FMT_SIZE.get(f, 0)

def scanVertexBuffers(meta, payloadSize):
    out=[]
    off=0
    meta.where="scanVB"
    while off + 80 <= meta.n:
        vbf=meta.u32(off)
        if vbf in (4,6,27):
            stride=meta.u32(off+4)
            if stride in (STRIDE_SIMPLE,STRIDE_SKINNED):
                do=meta.u32(off+0x38); ds=meta.u32(off+0x3C)
                if do and ds and do+ds<=payloadSize and ds%stride==0:
                    fopt=meta.u32(off+0x4C)
                    if fopt in (0,1):
                        out.append(dict(fileOffset=off,arrayIndex=-1,stride=stride,dataOffset=do,dataSize=ds,fOpt=fopt==1))
        off+=4
    out.sort(key=lambda v:v['fileOffset'])
    for i,v in enumerate(out): v['arrayIndex']=i
    return out

def scanIndexBuffers(meta, payloadSize, vbs):
    out=[]; off=0; meta.where="scanIB"
    while off+24<=meta.n:
        if meta.u32(off)==0 and meta.u32(off+4)==0:
            do=meta.u32(off+0x08); ds=meta.u32(off+0x0C)
            if do and ds and do>meta.n and do+ds<=payloadSize and ds%2==0:
                ibid=meta.i32(off+0x10); fopt=meta.u32(off+0x14)
                if fopt in (0,1) and (ibid==-1 or 0<=ibid<=1024):
                    out.append(dict(fileOffset=off,arrayIndex=-1,dataOffset=do,dataSize=ds,fOpt=fopt==1))
        off+=4
    out.sort(key=lambda v:v['fileOffset'])
    # (filterIbCluster omitted — doesn't read the buffer, can't OOB)
    for i,v in enumerate(out): v['arrayIndex']=i
    return out

def tryReadLayout(meta, origin, stride):
    lo=max(0,origin-0x800); hi=min(meta.n,origin+0x800)
    hay=bytes(meta.d[lo:hi]); rel=hay.find(SIG)
    if rel<0: return None
    cursor=lo+rel; elems=[]; last=-1; meta.where="tryLayout"
    while cursor+12<=meta.n:
        sem=meta.u32(cursor); fmt=meta.u32(cursor+4); off=meta.u32(cursor+8)
        sz=formatSize(fmt)
        if sz==0 or off<last or off+sz>stride: break
        elems.append((sem,fmt,off)); last=off; cursor+=12
        if off+sz==stride: return dict(stride=stride,elems=elems)
    return None

def canonical(stride):
    if stride==STRIDE_SIMPLE:
        return dict(stride=36,elems=[(0,1,0),(9,8,12),(7,5,16),(8,5,20),(1,7,24),(2,7,28),(10,8,32)])
    if stride==STRIDE_SKINNED:
        return dict(stride=44,elems=[(0,1,0),(9,8,12),(10,8,16),(7,5,20),(8,5,24),(1,7,28),(2,7,32),(11,4,36),(12,5,40)])
    return None

def resolveLayout(stride, meta, origin):
    p=tryReadLayout(meta,origin,stride)
    return p if (p and p['elems']) else canonical(stride)

def findElem(L, sem):
    for e in L['elems']:
        if e[0]==sem: return e
    return None

def readBonePalette(meta, structOffset):
    if structOffset+16>meta.n: return []
    meta.where="bonePalette"
    doff=meta.u32(structOffset+8); dsz=meta.u32(structOffset+12)
    if doff==0 or dsz==0 or dsz%4!=0: return []
    base=doff+16
    if base<0 or base+dsz>meta.n: return []
    return [meta.i32(base+i*4) for i in range(dsz//4)]

def scanSegments(meta):
    out=[]; off=16; meta.where="scanSeg"
    while off+16<=meta.n:
        if meta.u32(off-16)==0 and meta.u32(off-12)==0:
            vc=meta.u32(off); vo=meta.u32(off+4); ic=meta.u32(off+8); io=meta.u32(off+12)
            if 1<=vc<=200000 and 3<=ic<=5000000 and ic%3==0 and io<=5000000 and io%3==0:
                out.append(dict(fileOffset=off,vc=vc,vo=vo,ic=ic,io=io,pal=readBonePalette(meta,off-16)))
        off+=4
    return out

def findSubObjects(meta, mb, segs):
    out=[]; used=set()
    for seg in segs:
        if (seg['fileOffset']-16) in used: continue
        target=seg['fileOffset']-32
        if target<0: continue
        pat=struct.pack('<I',target); pos=0; meta.where="findSub"
        while True:
            p=mb.find(pat,pos)
            if p<0: break
            if p<8 or p+8>meta.n: pos=p+1; continue
            if meta.u32(p-8)!=0 or meta.u32(p-4)!=0 or meta.u32(p+4)!=32: pos=p+1; continue
            so=p-8-0xC8
            if so<0 or so+240>meta.n: pos=p+1; continue
            mat=meta.i32(so+0x60); vbi=meta.i32(so+0x68); ibi=meta.i32(so+0x6C)
            if not(-1<=vbi<=16) or not(-1<=ibi<=16): pos=p+1; continue
            h=meta.u32(so+0x64); slot=meta.u32(so+0x38+0x10)
            out.append(dict(fileOffset=so,mat=mat,vbi=vbi,ibi=ibi,seg=seg))
            used.add(seg['fileOffset']-16); break
    out.sort(key=lambda s:s['fileOffset'])
    return out

def parse(sno, do_cloth_note=True):
    mb=open(f'/tmp/crash/{sno}_meta.bin','rb').read()
    pb=open(f'/tmp/crash/{sno}_payload.bin','rb').read()
    meta=R(mb,'meta'); payload=R(pb,'payload')
    if meta.n<0xC0 or meta.u32(0)!=0xDEADBEEF: return "not-a-model"
    if payload.n<0x10: return "payload-too-small"
    vbs=scanVertexBuffers(meta,payload.n)
    ibs=scanIndexBuffers(meta,payload.n,vbs)
    if not vbs or not ibs: return f"no VB/IB (vbs={len(vbs)} ibs={len(ibs)})"
    vb0=next((v for v in vbs if v['fOpt']),None) or vbs[0]
    ib0=next((i for i in ibs if i['fOpt']),None) or ibs[0]
    stride=vb0['stride']
    vcount=vb0['dataSize']//stride
    layout=resolveLayout(stride,meta,vb0['fileOffset'])
    if not layout or not layout['elems']: return "no layout"
    if not payload.in_(vb0['dataOffset'],vb0['dataSize']): return "vb span OOB (rejected)"
    ePos=findElem(layout,SEM_POSITION)
    if not ePos: return "no POSITION"
    elems={s:findElem(layout,s) for s in (SEM_NORMAL,SEM_TEXCOORD_0,SEM_COLOR_0,SEM_COLOR_1,SEM_TEXCOORD_1,SEM_BLENDINDICES,SEM_BLENDWEIGHTS)}
    # vertex decode (raise-on-OOB reproduces the old crash)
    payload.where="vtxDecode"
    for i in range(vcount):
        base=vb0['dataOffset']+i*stride
        payload.f32(base+ePos[2]); payload.f32(base+ePos[2]+4); payload.f32(base+ePos[2]+8)
        for sem,e in elems.items():
            if e is None: continue
            sz=formatSize(e[1])
            for k in range(sz): payload.u8(base+e[2]+k)
    icount=ib0['dataSize']//2
    if not payload.in_(ib0['dataOffset'],ib0['dataSize']): return "ib span OOB (rejected)"
    payload.where="idxDecode"
    rawIdx=[payload.u16(ib0['dataOffset']+i*2) for i in range(icount)]
    segs=scanSegments(meta)
    subs=findSubObjects(meta,mb,segs)
    lod0=[s for s in subs if s['vbi']==vb0['arrayIndex'] and s['ibi']==ib0['arrayIndex']]
    if not lod0: return f"no lod0 subobjects (subs={len(subs)} segs={len(segs)})"
    # gather
    for s in lod0:
        baseV=s['seg']['vo']//stride
        endIdx=s['seg']['io']+s['seg']['ic']
        if endIdx>len(rawIdx): return f"gather: endIdx {endIdx} > rawIdx {len(rawIdx)} (rejected)"
        i=s['seg']['io']
        while i+2<endIdx:
            a,b,c=rawIdx[i],rawIdx[i+1],rawIdx[i+2]
            for g in (a+baseV,b+baseV,c+baseV):
                if g<0 or g>=vcount: return f"gather: vert index {g} >= vcount {vcount} (rejected)"
            i+=3
    return f"OK  vbs={len(vbs)} ibs={len(ibs)} vcount={vcount} stride={stride} segs={len(segs)} subs={len(subs)} lod0={len(lod0)}"

for sno in [2646190,2642003,2636865,2627290,283174]:
    try:
        print(f"sno={sno}: {parse(sno)}")
    except OOB as e:
        print(f"sno={sno}: *** {e} ***")
    except Exception as e:
        print(f"sno={sno}: EXC {type(e).__name__}: {e}")

# ── parseClothCapsules port (OOB-raising) ────────────────────────────────────
def parseCloth(sno):
    mb=open(f'/tmp/crash/{sno}_meta.bin','rb').read()
    pb=open(f'/tmp/crash/{sno}_payload.bin','rb').read()
    meta=R(mb,'meta'); payload=R(pb,'payload')
    def valid(r,b):
        if not r.in_(b,720): return False
        r.where="clothValid"
        vc=r.u16(b+252); tc=r.u16(b+258); con=r.u16(b+268); cc=r.u16(b+278)
        return vc>0 and vc<4000 and tc<4000 and con>0 and cc<=64
    nsim=0; ncap=0
    off=0
    while off+16<=meta.n:
        meta.where=f"clothHdr@{off}"
        if meta.u32(off)==0 and meta.u32(off+4)==0 and meta.i32(off+12)==720:
            dOff=meta.i32(off+8)
            cd = payload if valid(payload,dOff) else (meta if valid(meta,dOff) else None)
            if cd is not None:
                base=dOff; cd.where="clothCaps"
                caps=cd.u16(base+278)
                capOff=cd.i32(base+656+8); capSz=cd.i32(base+656+12)
                cr = payload if payload.in_(capOff,capSz) else (meta if meta.in_(capOff,capSz) else None)
                if cr is not None:
                    for i in range(caps):
                        b=capOff+i*80
                        if not cr.in_(b,80): break
                        cr.where=f"cap{i}"
                        for k in range(3): cr.f32(b+k*4)
                        for k in range(4): cr.f32(b+16+k*4)
                        cr.f32(b+48);cr.f32(b+52);cr.f32(b+56);cr.f32(b+60);cr.u16(b+64)
                        ncap+=1
                # sim cage
                def arr(fo):
                    cd.where=f"arr@{fo}"
                    return cd.i32(base+fo+8), cd.i32(base+fo+12)
                def rdr(o,s):
                    return payload if payload.in_(o,s) else (meta if meta.in_(o,s) else None)
                bvO,bvS=arr(288); imO,imS=arr(320); ciO,ciS=arr(528); clO,clS=arr(544)
                trO,trS=arr(512); dmO,dmS=arr(704); plO,plS=arr(672)
                nCage=bvS//16; nMass=imS//4; nPair=ciS//2//2; nLen=clS//4; nTri=trS//2//3; nMap=dmS//2; nPlane=plS//48
                rbv=rdr(bvO,bvS); rim=rdr(imO,imS); rci=rdr(ciO,ciS); rcl=rdr(clO,clS); rtr=rdr(trO,trS)
                if rbv and 0<nCage<8000:
                    rbv.where="cage"
                    for k in range(nCage):
                        rbv.f32(bvO+k*16);rbv.f32(bvO+k*16+4);rbv.f32(bvO+k*16+8)
                    if rim and nMass==nCage:
                        rim.where="mass"
                        for k in range(nMass): rim.f32(imO+k*4)
                    if rci:
                        rci.where="constraintIdx"
                        for k in range(nPair*2): rci.u16(ciO+k*2)
                    if rcl:
                        rcl.where="constraintLen"
                        for k in range(nLen): rcl.f32(clO+k*4)
                    if rtr:
                        rtr.where="tri"
                        for k in range(nTri*3): rtr.u16(trO+k*2)
                    rpl=rdr(plO,plS)
                    if rpl:
                        for k in range(min(nPlane,256)):
                            b=plO+k*48
                            if not rpl.in_(b,48): break
                            rpl.where=f"plane{k}"
                            for j in range(3): rpl.f32(b+j*4)
                            for j in range(4): rpl.f32(b+16+j*4)
                            rpl.f32(b+32);rpl.f32(b+36);rpl.u16(b+40)
                    alO,alS=arr(400); ral=rdr(alO,alS)
                    if ral and alS//4==nCage:
                        ral.where="attachLen"
                        for k in range(nCage): ral.f32(alO+k*4)
                    cd.where="name"
                    for k in range(32): cd.u16(base+216+k)
                    nsim+=1
        off+=4
    return f"OK  caps={ncap} sims={nsim}"

print("=== CLOTH path ===")
for sno in [2646190,2642003,2636865,2627290,283174]:
    try:
        print(f"sno={sno}: {parseCloth(sno)}")
    except OOB as e:
        print(f"sno={sno}: *** {e} ***")
    except Exception as e:
        import traceback
        print(f"sno={sno}: EXC {type(e).__name__}: {e}")

# ── parseSkeleton port ───────────────────────────────────────────────────────
BONE_SZ=232; BONE_PARENT=0x28; BONE_HASH=0x20; BONE_LOCAL_TRS=0x94
import struct as _st
def parseSkel(sno):
    mb=open(f'/tmp/crash/{sno}_meta.bin','rb').read()
    pb=open(f'/tmp/crash/{sno}_payload.bin','rb').read()
    meta=R(mb,'meta'); payload=R(pb,'payload')
    off=0; found=None
    while off+16<=meta.n:
        meta.where=f"boneScan@{off}"
        if meta.u32(off)==0 and meta.u32(off+4)==0:
            doff=meta.u32(off+8); dsz=meta.u32(off+12)
            if doff and dsz and dsz%BONE_SZ==0 and doff+dsz<=payload.n:
                payload.where="boneRoot"
                parent=payload.u16(doff+BONE_PARENT); parent = parent-0x10000 if parent>=0x8000 else parent
                if parent==-1:
                    q=doff+BONE_LOCAL_TRS
                    qx=payload.f32(q);qy=payload.f32(q+4);qz=payload.f32(q+8);qw=payload.f32(q+12)
                    mag2=qx*qx+qy*qy+qz*qz+qw*qw
                    if 0.5<mag2<1.5:
                        found=(doff, dsz//BONE_SZ); break
        off+=4
    if not found: return "no skeleton"
    base,count=found
    if count>200000 or base+count*BONE_SZ>payload.n: return f"skel rejected count={count}"
    payload.where="boneRecs"
    for i in range(count):
        rec=base+i*BONE_SZ
        payload.u32(rec+BONE_HASH); payload.u16(rec+BONE_PARENT)
    return f"OK  bones={count}"

print("=== SKELETON path ===")
for sno in [2646190,2642003,2636865,2627290,283174]:
    try: print(f"sno={sno}: {parseSkel(sno)}")
    except OOB as e: print(f"sno={sno}: *** {e} ***")
    except Exception as e: print(f"sno={sno}: EXC {type(e).__name__}: {e}")
