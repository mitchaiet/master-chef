#!/usr/bin/env python3
"""Adapt a narrow CEnshine 1.0.0 subset to retail Halo's shader ABI.

Inputs are local Composer-decrypted retail fx.bin and CEnshine collection.
No game or mod bytecode belongs in Git. The D3DX object-table layout and CE
collection layout are format facts; this parser does not embed upstream code.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

class Reader:
    def __init__(self, data, pos=0): self.data, self.pos = data, pos
    def take(self, n):
        if n < 0 or n > len(self.data)-self.pos: raise ValueError('Truncated shader input')
        v=self.data[self.pos:self.pos+n]; self.pos+=n; return v
    def u32(self): return struct.unpack('<I', self.take(4))[0]
    def words(self,n): return tuple(self.u32() for _ in range(n))
    def skip(self,n): self.take(n)
    def string(self): return self.take(self.u32()).split(b'\0',1)[0].decode('ascii')

def retail_shaders(data):
    outer=Reader(data); found={}
    while outer.pos<len(data):
        block=Reader(outer.take(outer.u32()))
        if block.u32()!=0xffffffff: raise ValueError('Not a retail D3DX effect')
        start=block.u32(); payload=block.data[8:]; r=Reader(payload,start)
        parameters,techniques,objects=r.words(3); names={}
        if max(parameters,techniques,objects)>4096: raise ValueError('Excessive effect table')
        for _ in range(parameters):
            typ,val,flags,annotations=r.words(4); r.skip(annotations*8)
            t,cl,name,semantic,elements=Reader(payload,typ).words(5)
            if t==15 and cl==4 and elements==0:
                oid=Reader(payload,val).u32()
                if oid>=objects: raise ValueError('Invalid effect object')
                names[oid]=Reader(payload,name).string()
        for _ in range(techniques):
            name,annotations,passes=r.words(3); r.skip(annotations*8)
            for _ in range(passes):
                name,annotations,states=r.words(3); r.skip(annotations*8+states*16)
        strings,resources=r.words(2)
        for _ in range(strings):
            oid,n=r.words(2); code=r.take(n); r.skip((-n)%4)
            if oid in names: found.setdefault(names[oid],[]).append(code)
    return found

def collection_shaders(data):
    # The released collection has an inner ASCII MD5 footer, in addition to
    # the outer checksum already verified and removed by Composer.
    if len(data)<37 or data[-1:]!=b'\0' or hashlib.md5(data[:-33]).hexdigest().encode()!=data[-33:-1]:
        raise ValueError('CEnshine collection checksum mismatch')
    r=Reader(data[:-33]); count=r.u32(); result={}
    if count!=126: raise ValueError('Unexpected collection revision')
    for _ in range(count):
        group=r.string(); functions=r.u32()
        if functions>256: raise ValueError('Excessive shader count')
        for _ in range(functions):
            name=r.string(); code=r.take(r.u32()*4)
            if name in result: raise ValueError('Duplicate shader name')
            result[name]=(group,code)
    if r.pos!=len(r.data): raise ValueError('Trailing collection data')
    return result

def adapt(code,model=False,complex_fog=False):
    if len(code)%4: raise ValueError('Unaligned bytecode')
    w=list(struct.unpack('<%dI'%(len(code)//4),code))
    if not w or w[0]!=0xffff0200: raise ValueError('Expected ps_2_0')
    # Retail environment-model shaders expose fog in c0..c2. CE's generalized
    # model shader expects c1..c3 plus tint/illumination/alpha-ref. Preserve
    # retail's white tint, no extra illumination and renderer-side alpha test.
    # Simple fog remains the retail renderer's responsibility; complex fog
    # uses the original three correction vectors.
    remap=({0:16,1:0,2:1,3:2,4:17,5:17,**({6:17} if not complex_fog else {})}
           if model else {6:17})
    i=1
    while i<len(w):
        op=w[i]&0xffff
        if op==0xffff:
            if i!=len(w)-1: raise ValueError('Trailing shader tokens')
            break
        if op==0xfffe: raise ValueError('Expected stripped release bytecode')
        n=(w[i]>>24)&15
        if not n or i+n>=len(w): raise ValueError('Invalid instruction length')
        if op==81: # DEF: literal floats are not register operands.
            if w[i+1]&2047 in (16,17): raise ValueError('Reserved constant conflict')
        else:
            for j in range(i+1,i+n+1):
                p=w[j]; kind=((p>>28)&7)|((p>>8)&24); index=p&2047
                if p&0x2000: raise ValueError('Relative addressing is not supported')
                if kind==2 and index in remap: w[j]=(p&~2047)|remap[index]
                # Non-complex shaders use v0.w for CE's in-shader fog. The
                # retail original has no such blend; retain its external fog.
                elif model and not complex_fog and kind==1 and index==0 and ((p>>16)&255)==255:
                    w[j]=(p&~0x70001800&~2047)|0x20000000|17
        i+=n+1
    else: raise ValueError('Missing END token')
    def define(index,values):
        return [0x05000051,0xa00f0000|index,*struct.unpack('<4I',struct.pack('<4f',*values))]
    defs=define(17,(0,0,0,0))
    if model: defs+=define(16,(1,1,1,1))
    w=w[:1]+defs+w[1:]
    return struct.pack('<%dI'%len(w),*w)

def make_pack(retail,collection):
    originals=retail_shaders(retail); mods=collection_shaders(collection); rows=[]; payload=[]; seen={}
    for name,(group,code) in sorted(mods.items()):
        if group not in ('environment_lightmap_normal','model_environment'): continue
        original_name=name.replace('PS_EnvironmentLightmapNormal','PS_LightmapNormal')
        matches=originals.get(original_name,[])
        if len(matches)!=1: raise ValueError('Missing or ambiguous retail shader: '+original_name)
        old=matches[0]
        if old[:4]!=b'\x00\x02\xff\xff' or old[-4:]!=b'\xff\xff\x00\x00': raise ValueError('Invalid retail shader')
        new=adapt(code,group=='model_environment','ComplexFog' in name)
        if old in seen:
            if seen[old]!=new: raise ValueError('Ambiguous identical original bytecode')
            continue
        seen[old]=new;payload.append(struct.pack('<II',len(old),len(new))+old+new)
        rows.append(dict(name=original_name,group=group,originalSHA256=hashlib.sha256(old).hexdigest(),replacementSHA256=hashlib.sha256(new).hexdigest(),originalBytes=len(old),replacementBytes=len(new)))
    if len(rows)!=13: raise ValueError('Expected the reviewed 13-program subset')
    return b'HVSHD001'+struct.pack('<II',len(rows),0)+b''.join(payload),rows

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('retail',type=Path);p.add_argument('collection',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    pack,rows=make_pack(a.retail.read_bytes(),a.collection.read_bytes());a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_bytes(pack)
    report=dict(format='HVSHD001',count=len(rows),sha256=hashlib.sha256(pack).hexdigest(),retailSHA256=hashlib.sha256(a.retail.read_bytes()).hexdigest(),collectionSHA256=hashlib.sha256(a.collection.read_bytes()).hexdigest(),entries=rows)
    a.output.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({k:v for k,v in report.items() if k!='entries'}))
if __name__=='__main__':main()
