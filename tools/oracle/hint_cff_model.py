#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
"""A prototype of FreeType's Adobe-engine y hinting, fitted from its answers (plan C2).

    hint_cff_model.py DUMP.jsonl[.gz] [N]

DUMP is what `hint_cff_dump.py` writes. The model below is the rule set recovered so far, in
Python floats so that a rule can be changed in one line and re-scored in seconds; the C
implementation is written from the rules that survive here, with integer arithmetic. It reports,
for glyphs whose hints do not change within the glyph and for those whose do, how many points
land within 2/64 pixel of FreeType's, against the unhinted outline as the baseline; N prints that
many glyphs it gets wrong.

Rules (all in y; x is not moved):
  * A stem edge is *locked* only by a zone of its own side: a bottom edge by a bottom zone
    (the baseline zone and OtherBlues), a top edge by a top zone. A locked edge lands on
        top zone:    round(flat * s + b)         bottom zone:  round(flat * s - b)
        b = 0.6 * max(0, 1 - s / BlueScale)       (s = ppem / upem)
    and the stem's other edge follows at its scaled width. A ghost hint (-20: top edge at y;
    -21: bottom edge at y + dy) is one edge.
  * A stem with no locked edge moves rigidly by a provisional shift taken from the locked
    edges only (never from other free stems): interpolated at its centre when there are locked
    edges on both sides; the nearest one's shift when only below; interpolated against an
    implicit anchor at y = 0 (shift 0) when only above; none when there are none. The edge
    then nearer a whole pixel is rounded to it (a tie goes to the top edge).
  * Every other point is interpolated linearly in y between the edges, and shifted rigidly
    above the highest and below the lowest.
"""
import gzip, json, math, sys, collections
def rnd(x): return math.floor(x+0.5)
def load(name, limit=None):
    out=[]
    opener=gzip.open if name.endswith('.gz') else open
    for i,l in enumerate(opener(name,'rt')):
        out.append(json.loads(l))
        if limit and i>=limit: break
    return out
def zones(par):
    zs=[]
    bv=[v/65536 for v in par['blue_values']]
    for i in range(0,len(bv),2):
        lo,hi=bv[i],bv[i+1]
        zs.append((lo,hi,hi if i==0 else lo,'bot' if i==0 else 'top'))
    ob=[v/65536 for v in par['other_blues']]
    for i in range(0,len(ob),2): zs.append((ob[i],ob[i+1],ob[i+1],'bot'))
    return zs
def blue_pos(flat,kind,s,bs,A=0.6):
    b=A*max(0,1-s/bs)
    return rnd(flat*s+b)  if kind=='top' else rnd(flat*s-b)
def edges_of(g, variant):
    par=g['par']; fuzz=par['blue_fuzz'][0]/65536 if par['blue_fuzz'] else 1.0
    zs=zones(par); s=g['p']/g['u']; bs=par['blue_scale'][0]/65536
    res=[]  # (font-unit edge, hinted px or None if not aligned)
    for k,e,w in g['st']:
        if k!='h': continue
        if w==-20.0: es=[e]
        elif w==-21.0: es=[e+w]
        else: es=[e,e+w]
        ent=[]
        for j,ed in enumerate(es):
            if len(es)==1: side='top' if w==-20.0 else 'bot'
            else: side='bot' if j==0 else 'top'
            hit=None
            for lo,hi,flat,kind in zs:
                if kind==side and lo-fuzz<=ed<=hi+fuzz: hit=(flat,kind); break
            ent.append((ed,hit))
        res.append((e,w,ent))
    return res
def interp(anchors, pos):
    """delta at font-unit-scaled position pos from sorted [(px, delta)]"""
    if not anchors: return None
    if pos<=anchors[0][0]: return anchors[0][1]
    if pos>=anchors[-1][0]: return anchors[-1][1]
    for i in range(len(anchors)-1):
        a0,d0=anchors[i]; a1,d1=anchors[i+1]
        if a0<=pos<=a1:
            t=(pos-a0)/(a1-a0) if a1>a0 else 0
            return d0*(1-t)+d1*t
def predict(g, variant='v1'):
    s=g['p']/g['u']; par=g['par']; bs=par['blue_scale'][0]/65536
    stems=edges_of(g,variant)
    anchors={}   # px(unhinted) -> delta
    pending=[]
    for e,w,ent in stems:
        if w in (-20.0,-21.0):
            ed,hit=ent[0]
            if hit: anchors[ed*s]=blue_pos(hit[0],hit[1],s,bs)-ed*s
            continue
        (e0,h0),(e1,h1)=ent
        if h0 and h1:
            anchors[e0*s]=blue_pos(h0[0],h0[1],s,bs)-e0*s; anchors[e1*s]=blue_pos(h1[0],h1[1],s,bs)-e1*s
        elif h0:
            n0=blue_pos(h0[0],h0[1],s,bs); anchors[e0*s]=n0-e0*s; anchors[e1*s]=n0+(e1-e0)*s-e1*s
        elif h1:
            n1=blue_pos(h1[0],h1[1],s,bs); anchors[e1*s]=n1-e1*s; anchors[e0*s]=n1-(e1-e0)*s-e0*s
        else:
            pending.append((e,w))
    blue=sorted(anchors.items())
    for e,w in pending:
        lo,hi=e*s,(e+w)*s; c=(lo+hi)/2
        below=[q for q in blue if q[0]<=lo+1e-9]; above=[q for q in blue if q[0]>=hi-1e-9]
        if below and above:
            ob,db=below[-1]; oa,da=above[0]; d0=interp([(ob,db),(oa,da)],c)
        elif below: d0=below[-1][1]
        elif above:
            oa,da=above[0]
            d0=interp([(0.0,0.0),(oa,da)],c) if lo>0 else da
        else: d0=0.0
        lo2,hi2=lo+d0,hi+d0
        r0=rnd(lo2)-lo2; r1=rnd(hi2)-hi2
        dl=r0 if abs(r0)<abs(r1)-0.006 else r1
        d=d0+dl
        anchors[lo]=d; anchors[hi]=d
    m=sorted(anchors.items())
    out=[]
    for y in g['y0']:
        yp=y/64
        d=interp(m,yp) if m else 0
        out.append(rnd((yp+d)*64))
    return out
def evaluate(rows, variant='v0', show=0):
    tot=collections.Counter(); per=collections.defaultdict(collections.Counter)
    shown=0
    for g in rows:
        multi=len(g['mk'])>1
        pr=predict(g,variant)
        diffs=[abs(a-b) for a,b in zip(pr,g['y1'])]
        k='multi' if multi else 'single'
        per[k]['glyphs']+=1
        per[k]['pts']+=len(diffs)
        per[k]['exact']+=sum(1 for d in diffs if d==0)
        per[k]['le2']+=sum(1 for d in diffs if d<=2)
        per[k]['le8']+=sum(1 for d in diffs if d<=8)
        per[k]['glyph_le2']+= all(d<=2 for d in diffs)
        per[k]['glyph_exact']+= all(d==0 for d in diffs)
        # baseline: unhinted
        base=[abs(a-b) for a,b in zip(g['y0'],g['y1'])]
        per[k]['base_le2']+=sum(1 for d in base if d<=2)
        per[k]['base_glyph_le2']+= all(d<=2 for d in base)
        if show and not all(d<=2 for d in diffs) and not multi and shown<show:
            shown+=1; print(g['f'],g['p'],g['g'],'stems',g['st'],'worst',max(diffs))
    for k,c in per.items():
        print(k,'glyphs',c['glyphs'],'points exact %.1f%% <=2 %.1f%% <=8 %.1f%% | glyphs exact %.1f%% <=2 %.1f%% | (unhinted baseline: pts<=2 %.1f%% glyphs<=2 %.1f%%)'%(
          100*c['exact']/c['pts'],100*c['le2']/c['pts'],100*c['le8']/c['pts'],100*c['glyph_exact']/c['glyphs'],100*c['glyph_le2']/c['glyphs'],100*c['base_le2']/c['pts'],100*c['base_glyph_le2']/c['glyphs']))
if __name__=='__main__':
    rows=[g for g in load(sys.argv[1]) if g['p']<40]
    print(len(rows),'glyph instances'); evaluate(rows,show=int(sys.argv[2]) if len(sys.argv)>2 else 0)
