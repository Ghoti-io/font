#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
#
# Ghoti.io Font is free software: you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License version 3 as
# published by the Free Software Foundation.
"""Random GSUB lookups, shaped by this library and by HarfBuzz, and compared.

The corpus differential (`hb_diff.py`) holds the engine to HarfBuzz on real fonts,
whose lookups are sensible: they never put a ligature of two virama-less
consonants across a syllable, a context rule over a class that is empty, or a
reverse chain with a lookahead of three. This tool makes fonts whose lookups are
random in every format `GSUB` has (single, multiple, alternate, ligature, the
three context and three chain-context formats with nested lookups, and reverse
chain), over a small alphabet so that rules match often, and shapes a dozen
strings with each. Nothing is committed: every font is a function of its seed.

What it found when it was first run: Latin agreed on every seed (3,000 seeds,
36,000 strings), which is what `make check-oracle-gsub` holds. Devanagari showed
four differences in the feature plan, fixed, and some in how the Indic shaper
orders glyphs after `blwf`, `pstf`, `vatu` and `rphf`, which are not (about one
string in ten); `--script deva` shows them and is not part of the gate. See
notes/font/SHAPING.md, "Random lookups".

Usage: gsub_random_diff.py [--seeds N] [--first K] [--script latn|deva|deva-old]
                           [--reuse] [-v] [--driver PATH] [--scratch DIR]

`--reuse` takes HarfBuzz's answers from the `.hb` files a previous run left in the
scratch directory, which is how a mutation pass runs it on a machine with no
container; `--driver` and `--scratch` name the shaper and the directory.
"""

import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env

ROOT = oracle_env.ROOT
DRIVER = os.path.join(ROOT, "build", "linux", "release", "apps", "examples",
                      "font-shape")
SCRATCH = os.path.join(ROOT, "build", "oracle", "gsub-random")

import struct
def u16(*v): return b''.join(struct.pack('>H',x&0xffff) for x in v)
def u32(*v): return b''.join(struct.pack('>I',x&0xffffffff) for x in v)
def pad4(b): return b+b'\0'*((-len(b))%4)

def sfnt(tables):
    tags=sorted(tables)
    n=len(tags)
    out=struct.pack('>IHHHH',0x10000,n,0,0,0)
    off=12+16*n
    body=b''
    ents=b''
    for t in tags:
        d=tables[t]
        ents+=struct.pack('>4sIII',t.encode(),0,off+len(body),len(d))
        body+=pad4(d)
    return out+ents+body

def base_font(nglyphs=40, extra=None, cmap_map=None, adv=None):
    """glyph 0 notdef, glyph i+1 = chr(65+i) for i<26, then 'a'.. for the rest."""
    cm=cmap_map or {}
    if not cmap_map:
        for i in range(26): cm[65+i]=i+1
        for i in range(26): cm[97+i]=27+i if 27+i<nglyphs else 0
    segs=sorted((c,g) for c,g in cm.items() if g)
    # format 4, one segment per code point
    sc=len(segs)+1
    ends=u16(*[c for c,g in segs],0xffff)
    starts=u16(*[c for c,g in segs],0xffff)
    deltas=u16(*[(g-c) for c,g in segs],1)
    ro=u16(*([0]*sc))
    f4=u16(4,16+len(ends)*0+2*sc*4+2,0,sc*2,0,0,0)+ends+u16(0)+starts+deltas+ro
    f4=u16(4,len(f4),0)[0:6]+f4[6:]
    cmap=u16(0,1,3,1)+u32(12)+f4
    head=u32(0x10000,0x10000,0,0x5F0F3CF5)+u16(0,1000)+b'\0'*16+u16(0,0,1000,1000,0,8,2,0,0)
    hhea=u32(0x10000)+u16(800,-200&0xffff,0,1500,0,0,1250,1,0,0,0,0,0,0,0,nglyphs)
    maxp=u32(0x5000)+u16(nglyphs)
    adv=adv or (lambda g:500+10*g)
    hmtx=b''.join(u16(adv(g),0) for g in range(nglyphs))
    t={'head':head,'hhea':hhea,'maxp':maxp,'hmtx':hmtx,'cmap':cmap}
    if extra: t.update(extra)
    return sfnt(t)


def cov1(gl): gl=sorted(gl); return u16(1,len(gl),*gl)

def classdef2(m):
    if not m: return u16(2,0)
    gl=sorted(m); lo,hi=gl[0],gl[-1]
    return u16(2,lo*0+len(gl))[:0]+u16(1,lo,hi-lo+1,*[m.get(g,0) for g in range(lo,hi+1)])
def seq_records(r,nested,n):
    recs=[]
    for _ in range(r.randint(0,2)):
        if nested: recs.append((r.randrange(n),r.choice(nested)))
    return recs
def recs_b(recs): return b''.join(u16(a,b) for a,b in recs)
class Gen:
    def __init__(self,seed,gl):
        self.r=random.Random(seed); self.gl=gl   # gl: list of glyph ids used
    def g(self): return self.r.choice(self.gl)
    def gset(self,k=None):
        k=k or self.r.randint(1,4); return sorted(set(self.g() for _ in range(k)))
    def simple(self,t):
        r=self.r
        if t==1:
            cs=self.gset()
            if r.random()<.5: return u16(2,6+len(cov1(cs))*0,0)[:0]+self._single2(cs)
            return self._single1(cs)
        if t==2:
            cs=self.gset(); cov=cov1(cs)
            seqs=[u16(n:=r.randint(0,3),*[self.g()+0 for _ in range(n)]) for _ in cs]
            hdr=6+2*len(cs); offs=[];body=b''
            for s in seqs: offs.append(hdr+len(cov)+len(body)); body+=s
            return u16(1,hdr,len(cs),*offs)+cov+body
        if t==3:
            cs=self.gset(); cov=cov1(cs)
            sets=[u16(n:=r.randint(1,3),*[self.g() for _ in range(n)]) for _ in cs]
            hdr=6+2*len(cs); offs=[];body=b''
            for s in sets: offs.append(hdr+len(cov)+len(body)); body+=s
            return u16(1,hdr,len(cs),*offs)+cov+body
        if t==4:
            cs=self.gset(); cov=cov1(cs)
            sets=[]
            for _ in cs:
                ligs=[]
                for _ in range(r.randint(1,3)):
                    comps=[self.g() for _ in range(r.randint(0,3))]
                    ligs.append(u16(self.g()+0,len(comps)+1,*comps))
                hdr=2+2*len(ligs); offs=[];body=b''
                for l in ligs: offs.append(hdr+len(body)); body+=l
                sets.append(u16(len(ligs),*offs)+body)
            hdr=6+2*len(cs); offs=[];body=b''
            for s in sets: offs.append(hdr+len(cov)+len(body)); body+=s
            return u16(1,hdr,len(cs),*offs)+cov+body
    def _single1(self,cs): 
        cov=cov1(cs); return u16(1,6,self.r.randint(-3,3))+cov
    def _single2(self,cs):
        cov=cov1(cs); subs=[self.g() for _ in cs]
        return u16(2,6+2*len(cs),len(cs),*subs)+cov
    def context(self,t,nested,n):
        r=self.r; fmt=r.choice((1,2,3))
        if t==5:
            if fmt==1:
                cs=self.gset(); cov=cov1(cs); sets=[]
                for _ in cs:
                    rules=[]
                    for _ in range(r.randint(1,2)):
                        ins=[self.g() for _ in range(r.randint(0,2))]
                        recs=seq_records(r,nested,len(ins)+1)
                        rules.append(u16(len(ins)+1,len(recs),*ins)+recs_b(recs))
                    hdr=2+2*len(rules); offs=[];body=b''
                    for x in rules: offs.append(hdr+len(body)); body+=x
                    sets.append(u16(len(rules),*offs)+body)
                hdr=6+2*len(cs); offs=[];body=b''
                for s in sets: offs.append(hdr+len(cov)+len(body)); body+=s
                return u16(1,hdr,len(cs),*offs)+cov+body
            if fmt==2:
                cs=self.gset(); cov=cov1(cs)
                cd=classdef2({g:r.randint(1,2) for g in self.gl if r.random()<.7})
                sets=[]
                nclass=3
                for c in range(nclass):
                    if r.random()<.3: sets.append(None); continue
                    rules=[]
                    for _ in range(r.randint(1,2)):
                        ins=[r.randint(0,2) for _ in range(r.randint(0,2))]
                        recs=seq_records(r,nested,len(ins)+1)
                        rules.append(u16(len(ins)+1,len(recs),*ins)+recs_b(recs))
                    hdr=2+2*len(rules); offs=[];body=b''
                    for x in rules: offs.append(hdr+len(body)); body+=x
                    sets.append(u16(len(rules),*offs)+body)
                hdr=8+2*nclass; offs=[];body=b'';base=hdr+len(cov)+len(cd)
                for s in sets:
                    if s is None: offs.append(0)
                    else: offs.append(base+len(body)); body+=s
                return u16(2,hdr,hdr+len(cov),nclass,*offs)[:0]+u16(2,hdr,hdr+len(cov),nclass)+u16(*offs)+cov+cd+body
            # fmt 3
            k=r.randint(1,3); covs=[cov1(self.gset()) for _ in range(k)]
            recs=seq_records(r,nested,k)
            hdr=6+2*k+4*len(recs); offs=[];body=b''
            for c in covs: offs.append(hdr+len(body)); body+=c
            return u16(3,k,len(recs),*offs)+recs_b(recs)+body
        # chain
        if fmt==1:
            cs=self.gset(); cov=cov1(cs); sets=[]
            for _ in cs:
                rules=[]
                for _ in range(r.randint(1,2)):
                    bt=[self.g() for _ in range(r.randint(0,2))]; ins=[self.g() for _ in range(r.randint(0,2))]; la=[self.g() for _ in range(r.randint(0,2))]
                    recs=seq_records(r,nested,len(ins)+1)
                    rules.append(u16(len(bt),*bt)+u16(len(ins)+1,*ins)+u16(len(la),*la)+u16(len(recs))+recs_b(recs))
                hdr=2+2*len(rules); offs=[];body=b''
                for x in rules: offs.append(hdr+len(body)); body+=x
                sets.append(u16(len(rules),*offs)+body)
            hdr=6+2*len(cs); offs=[];body=b''
            for s in sets: offs.append(hdr+len(cov)+len(body)); body+=s
            return u16(1,hdr,len(cs),*offs)+cov+body
        if fmt==2:
            cs=self.gset(); cov=cov1(cs)
            cds=[classdef2({g:r.randint(1,2) for g in self.gl if r.random()<.7}) for _ in range(3)]
            nclass=3; sets=[]
            for c in range(nclass):
                if r.random()<.3: sets.append(None); continue
                rules=[]
                for _ in range(r.randint(1,2)):
                    bt=[r.randint(0,2) for _ in range(r.randint(0,2))]; ins=[r.randint(0,2) for _ in range(r.randint(0,2))]; la=[r.randint(0,2) for _ in range(r.randint(0,2))]
                    recs=seq_records(r,nested,len(ins)+1)
                    rules.append(u16(len(bt),*bt)+u16(len(ins)+1,*ins)+u16(len(la),*la)+u16(len(recs))+recs_b(recs))
                hdr=2+2*len(rules); offs=[];body=b''
                for x in rules: offs.append(hdr+len(body)); body+=x
                sets.append(u16(len(rules),*offs)+body)
            hdr=12+2*nclass; pos=hdr; covoff=pos; pos+=len(cov); cdo=[]
            for c in cds: cdo.append(pos); pos+=len(c)
            offs=[];body=b''
            for s in sets:
                if s is None: offs.append(0)
                else: offs.append(pos+len(body)); body+=s
            return u16(2,covoff,*cdo,nclass)+u16(*offs)+cov+b''.join(cds)+body
        bt=[cov1(self.gset()) for _ in range(r.randint(0,2))]; ins=[cov1(self.gset()) for _ in range(r.randint(1,3))]; la=[cov1(self.gset()) for _ in range(r.randint(0,2))]
        recs=seq_records(r,nested,len(ins))
        hdr=2+2+2*len(bt)+2+2*len(ins)+2+2*len(la)+2+4*len(recs)
        pos=hdr
        def place(L):
            nonlocal pos
            o=[]
            for c in L: o.append(pos); pos+=len(c)
            return o
        ob=place(bt); oi=place(ins); ol=place(la)
        return u16(3,len(bt),*ob,len(ins),*oi,len(la),*ol,len(recs))+recs_b(recs)+b''.join(bt+ins+la)
    def reverse(self):
        r=self.r; cs=self.gset(); cov=cov1(cs)
        bt=[cov1(self.gset()) for _ in range(r.randint(0,2))]; la=[cov1(self.gset()) for _ in range(r.randint(0,2))]
        subs=[self.g() for _ in cs]
        hdr=6+2*len(bt)+2+2*len(la)+2+2*len(subs)
        pos=hdr+len(cov); 
        ocov=hdr
        o=[];p=pos
        for c in bt+la: o.append(p); p+=len(c)
        ob=o[:len(bt)]; ol=o[len(bt):]
        return u16(1,ocov,len(bt),*ob,len(la),*ol,len(subs),*subs)+cov+b''.join(bt+la)

def lookup_table(typ,flag,subs):
    hdr=6+2*len(subs); offs=[];body=b''
    for x in subs: offs.append(hdr+len(body)); body+=x
    return u16(typ,flag,len(subs),*offs)+body
def build_gsub(seed,gl,tags,override=None,old_spec=False):
    gen=Gen(seed,gl); r=gen.r
    lookups=[]
    for _ in range(3):
        t=r.choice((1,1,2,3,4)); lookups.append(lookup_table(t,r.choice((0,0,0,4,8)),[gen.simple(t) for _ in range(r.randint(1,2))]))
    nested=[0,1,2]
    for _ in range(r.randint(2,4)):
        k=r.choice((5,6,6,8,4,1))
        if k in (1,2,3,4): subs=[gen.simple(k)]
        elif k==8: subs=[gen.reverse()]
        else: subs=[gen.context(k,nested,3) for _ in range(r.randint(1,2))]
        lookups.append(lookup_table(k,r.choice((0,0,0,4,8)),subs))
    n=len(lookups)
    feats=[]
    for t in tags:
        feats.append((t,sorted(set(r.randrange(n) for _ in range(r.randint(1,2))))))
    if override: feats=[(t,override.get(t,ls)) for t,ls in feats]
    feats.sort(key=lambda f:f[0])
    build_gsub.last=(lookups,feats)
    fb=b''; offs=[]
    flist_hdr=2+6*len(feats)
    fbodies=[]
    for t,ls in feats: fbodies.append(u16(0,len(ls),*ls))
    pos=flist_hdr; frec=b''
    for (t,ls),b in zip(feats,fbodies): frec+=t.encode()+u16(pos); pos+=len(b)
    flist=u16(len(feats))+frec+b''.join(fbodies)
    langsys=u16(0,0xffff,len(feats),*range(len(feats)))
    script=u16(4,0)+langsys
    # Sorted, because a reader bisects them. An old-spec font has no 'dev2'.
    scripts=['DFLT','deva','latn'] if old_spec else ['DFLT','dev2','deva','latn']
    slhdr=2+6*len(scripts); sl=u16(len(scripts)); body=b''
    for sc in scripts: sl+=sc.encode()+u16(slhdr+len(body)); body+=script
    sl+=body
    ll=u16(n); off=2+2*n; offs=[]
    for l in lookups: offs.append(off); off+=len(l)
    ll+=u16(*offs)+b''.join(lookups)
    h=10
    return u32(0x10000)+u16(h,h+len(sl),h+len(sl)+len(flist))+sl+flist+ll


LATIN = {65 + i: i + 1 for i in range(8)}                 # A..H -> 1..8
DEVA = {0x930: 9, 0x94D: 10, 0x915: 11, 0x916: 12, 0x93E: 13, 0x93F: 14}
GLYPHS = list(range(1, 15))
TAGS_LATIN = ['ccmp', 'liga', 'calt', 'locl', 'rlig', 'clig']
TAGS_DEVA = ['nukt', 'akhn', 'rphf', 'pref', 'blwf', 'half', 'pstf', 'vatu',
             'cjct', 'pres', 'abvs', 'blws', 'psts', 'haln']


def texts_for(script, seed):
    r = random.Random(seed * 7)
    if script == "latn":
        return [''.join(chr(65 + r.randrange(8)) for _ in range(r.randint(1, 6)))
                for _ in range(12)]
    alphabet = '\u0930\u094d\u0915\u0916\u093e\u093f'
    return [''.join(r.choice(alphabet) for _ in range(r.randint(2, 6)))
            for _ in range(12)]


def main(argv):
    global DRIVER, SCRATCH
    seeds, first, script, reuse = 200, 0, "latn", False
    for i, a in enumerate(argv):
        if a == "--seeds":
            seeds = int(argv[i + 1])
        elif a == "--first":
            first = int(argv[i + 1])
        elif a == "--script":
            script = argv[i + 1]
        elif a == "--reuse":
            reuse = True
        elif a == "--driver":
            DRIVER = argv[i + 1]
        elif a == "--scratch":
            SCRATCH = argv[i + 1]
    cmap = LATIN if script == "latn" else DEVA
    old = script == "deva-old"
    if old:
        script = "deva"
    iso, ot = (("Latn", "latn") if script == "latn"
               else ("Deva", "deva" if old else "dev2"))
    os.makedirs(SCRATCH, exist_ok=True)
    lines = []
    for seed in range(first, first + seeds):
        font = os.path.join(SCRATCH, "s%d.ttf" % seed)
        text = os.path.join(SCRATCH, "s%d.txt" % seed)
        with open(font, "wb") as h:
            h.write(base_font(nglyphs=30, cmap_map=cmap, extra={
                'GSUB': build_gsub(seed, GLYPHS, TAGS_LATIN + TAGS_DEVA,
                                       old_spec=old)}))
        with open(text, "w", encoding="utf-8") as h:
            h.write(''.join(t + "\n" for t in texts_for(script, seed)))
        lines.append("hb-shape --font-file='%s' --output-format=json "
                     "--no-glyph-names --script=%s --text-file='%s' > '%s.hb'"
                     % (font, iso, text, font))
    runner = os.path.join(SCRATCH, "run.sh")
    with open(runner, "w") as h:
        h.write("\n".join(lines) + "\n")
    if reuse:
        ref = subprocess.CompletedProcess([], 0, "", "")
    else:
        ref = subprocess.run(oracle_env.command("harfbuzz", ["sh", runner],
                                                scratch=SCRATCH),
                             capture_output=True, text=True)
    if ref.returncode not in (0, 1):
        sys.stderr.write("hb-shape failed: %s\n" % ref.stderr[-300:])
        return 2
    import json
    bad = 0
    compared = 0
    for seed in range(first, first + seeds):
        font = os.path.join(SCRATCH, "s%d.ttf" % seed)
        text = os.path.join(SCRATCH, "s%d.txt" % seed)
        with open(font + ".hb", encoding="utf-8") as h:
            # A line with no glyphs is an empty line in hb-shape's JSON.
            hb = [json.loads(l) if l.startswith("[") else []
                  for l in h.read().split("\n")[:-1]]
        with open(text, "rb") as h:
            ours = subprocess.run([DRIVER, "--batch", "--script", ot, font],
                                  stdin=h, capture_output=True, text=True)
        mine = [json.loads(l) for l in ours.stdout.split("\n")[:-1]]
        keys = ("g", "cl", "ax", "ay", "dx", "dy")
        norm = lambda g: [tuple(x.get(k) for k in keys) for x in g]
        for i, (a, b) in enumerate(zip(hb, mine)):
            compared += 1
            if norm(a) != norm(b):
                bad += 1
                if "-v" in argv:
                    print("seed", seed, ' '.join('%X' % ord(ch) for ch in
                          open(text, encoding="utf-8").read().split("\n")[i]),
                          "\n  hb ", [t[0] for t in norm(a)],
                          "\n  our", [t[0] for t in norm(b)])
        if len(hb) != len(mine):
            bad += 1
    print("gsub_random_diff: %s, %d seeds from %d, %d strings, %d differ"
          % (script, seeds, first, compared, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
