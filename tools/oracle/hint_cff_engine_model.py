#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
"""The y-hinting rules of FreeType's Adobe engine in the integer arithmetic they are fitted in (plan C2).

    hint_cff_engine_model.py DUMP.jsonl[.gz] [--ppem-min N] [--all-masks]

The float prototype `hint_cff_model.py` says what the rules are; this is the same rule set in
16.16 fixed point, which is what the C engine is written from and what reproduces FreeType to the
last 1/64 pixel. A point's position is mapped through a piecewise-linear map of the stem edges of
the hint mask in force (interval scale by a rounded division, offsets by a rounded multiply), and
the 26.6 result is the 16.16 value shifted right by ten (truncated, not rounded).

On the corpus of `hint_cff_dump.py` (39 CFF fonts; ppem 9, 12, 17, 32, 48) it scores the
fraction of points that equal FreeType's exactly and the fraction of glyphs all of whose points do.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hint_cff_model import load, zones, active_sets
import math, collections
ONE=65536
GHOST='prov'      # a free ghost edge: provisional shift from the locked edges, then the nearest pixel
MINGAP=32768      # half a pixel: the least a free stem is left from the edge below it
OVERSHOOT=True    # at or above BlueScale an overshoot edge keeps its overshoot, rounded outward
OV_C=int(0.2*65536)
S0=1
def mulfix(a,b):
    p=a*b
    return (p+0x8000)>>16 if p>=0 else -((-p+0x8000)>>16)
def divfix(a,b):
    sign=1
    if a<0: a=-a; sign=-sign
    if b<0: b=-b; sign=-sign
    return sign*(((a<<16)+(b>>1))//b)
def fround(x): return (x+0x8000)&~0xFFFF   # to whole pixel, 16.16 (floor(x+.5))
def hint_map(g, only, S, boost_fn, tie=0.0):
    p=g['p']; u=g['u']; par=g['par']
    fuzz=par['blue_fuzz'][0] if par['blue_fuzz'] else ONE
    bs=par['blue_scale'][0]
    zs=[(int(a*ONE),int(b*ONE),int(c*ONE),k) for a,b,c,k in zones(par)]
    s_float=p/u
    boost=boost_fn(S,bs)
    edges={}   # cs16 -> ds16
    def zone_hit(cs,side):
        for lo,hi,flat,kind in zs:
            if kind==side and lo-fuzz<=cs<=hi+fuzz: return flat,kind
        return None
    items=[]
    for idx,(k,e,w) in enumerate(g['st']):
        if k!='h': continue
        if only is not None and idx not in only: continue
        e16=int(e*ONE); w16=int(w*ONE)
        if w==-20: items.append(('ghost',e16,'top'))
        elif w==-21: items.append(('ghost',e16+w16,'bot'))
        else: items.append(('stem',e16,e16+w16))
    pend=[]
    gpend=[]
    def locked(cs,side):
        h=zone_hit(cs,side)
        if not h: return None
        flat,kind=h
        v=mulfix(flat,S)
        r=fround(v+boost) if kind=='top' else fround(v-boost)
        if OVERSHOOT and boost==0 and cs!=flat:
            ov=mulfix(cs-flat,S)
            r=fround(v)+fround(ov+(OV_C if kind=='top' else -OV_C))
        return r
    for it in items:
        if it[0]=='ghost':
            _,cs,side=it
            d=locked(cs,side)
            if d is not None: edges[cs]=d
            else: gpend.append(cs)
            continue
        _,lo,hi=it
        a=locked(lo,'bot'); b=locked(hi,'top')
        if a is not None and b is not None: edges[lo]=a; edges[hi]=b
        elif a is not None: edges[lo]=a; edges[hi]=a+mulfix(hi-lo,S)
        elif b is not None: edges[hi]=b; edges[lo]=b-mulfix(hi-lo,S)
        else: pend.append((lo,hi))
    blue=sorted(edges.items())
    def delta(cs,ds): return ds-mulfix(cs,S)
    def above_only(a,lo,hi,c):
        # only locked edges above: a stem wholly below the baseline takes the nearest one's shift, fading to nothing
        # eight pixels away; any other is interpolated against an implicit anchor at y = 0 that does not move
        if hi<=0:
            fade=max(0,ONE-(mulfix(a[0]-hi,S)>>3))
            return mulfix(delta(*a),fade)
        return interp_delta([(0,0),a],c)
    def interp_delta(anchors,cs):
        if cs<=anchors[0][0]: return delta(*anchors[0])
        if cs>=anchors[-1][0]: return delta(*anchors[-1])
        for i in range(len(anchors)-1):
            a,b=anchors[i],anchors[i+1]
            if a[0]<=cs<=b[0]:
                da,db=delta(*a),delta(*b)
                if b[0]==a[0]: return da
                t=divfix(cs-a[0],b[0]-a[0])
                return da+mulfix(db-da,t)
    for lo,hi in sorted(pend):
        c=(lo+hi)//2
        below=[q for q in blue if q[0]<=lo]; above=[q for q in blue if q[0]>=hi]
        if below and above: d0=interp_delta([below[-1],above[0]],c)
        elif below: d0=delta(*below[-1])
        elif above:
            d0=above_only(above[0],lo,hi,c)
        else: d0=0
        ls=mulfix(lo,S)+d0; hs=mulfix(hi,S)+d0
        r0=fround(ls)-ls; r1=fround(hs)-hs
        dl=r0 if abs(r0)<abs(r1)-int(tie*ONE) else r1
        nl=ls+dl; width=mulfix(hi,S)-mulfix(lo,S)
        if MINGAP:
            # a stem is not left closer than MINGAP to the edge below it: its bottom goes to the first whole
            # pixel that clears it, unless that runs into the edge above, and then it stays where scaling puts it
            placed_below=[(cs,ds) for cs,ds in edges.items() if cs<=lo]
            if placed_below:
                tcs,tds=max(placed_below)
                if nl-tds<MINGAP:
                    cand=fround(tds+MINGAP+0x7FFF)   # the first whole pixel at or above
                    placed_above=[(cs,ds) for cs,ds in edges.items() if cs>=hi]
                    if placed_above and cand+width>min(placed_above)[1]-MINGAP: nl=ls
                    else: nl=cand
        edges[lo]=nl; edges[hi]=nl+width
    for cs in gpend:
        if GHOST=='round':
            edges[cs]=fround(mulfix(cs,S))
        elif GHOST=='prov':
            below=[q for q in blue if q[0]<=cs]; above=[q for q in blue if q[0]>=cs]
            if below and above: d0=interp_delta([below[-1],above[0]],cs)
            elif below: d0=delta(*below[-1])
            elif above: d0=above_only(above[0],cs,cs,cs)
            else: d0=0
            edges[cs]=fround(mulfix(cs,S)+d0)
    return sorted(edges.items())
def map_point(edges,cs,S):
    if not edges: return mulfix(cs,S)
    if cs<=edges[0][0]: return edges[0][1]+mulfix(cs-edges[0][0],S)
    if cs>=edges[-1][0]: return edges[-1][1]+mulfix(cs-edges[-1][0],S)
    for i in range(len(edges)-1):
        a,b=edges[i],edges[i+1]
        if a[0]<=cs<=b[0]:
            if b[0]==a[0] or cs==a[0]: return a[1]
            if cs==b[0]: return b[1]       # a point on an edge is on the edge's position exactly
            sc=divfix(b[1]-a[1],b[0]-a[0]); return a[1]+mulfix(cs-a[0],sc)
def evaluate(rows,S_fn,boost_fn,final=lambda ds:ds>>10,only_single=True,A=0.6):
    ok=tot=0; gl=gok=0
    for g in rows:
        if only_single and len(g['mk'])>1: continue
        S=S_fn(g); sets=active_sets(g)
        maps=[hint_map(g,a,S,boost_fn) for _,a in sets]
        allok=True
        for n,(fy,y1) in enumerate(zip(g['fy'],g['y1'])):
            i=g['fyi'][n] if g.get('fyi') else n     # the index in this library's outline, which a hint mask counts
            k=0
            for j,(f,_) in enumerate(sets):
                if f<=i: k=j
            ds=map_point(maps[k],fy*1024,S)
            e=final(ds)==y1; ok+=e; tot+=1; allok&=e
        gl+=1; gok+=allok
    return 100*ok/tot,100*gok/gl,gl
def score(rows, label, single_only):
    ok=tot=gl=gok=gle1=0
    for g in rows:
        if single_only and len(g['mk'])>1: continue
        S=round(g['p']*65536/g['u']); s=g['p']/g['u']; bs=g['par']['blue_scale'][0]/65536
        boost=min(round(0.6*max(0,1-s/bs)*ONE),32768)
        sets=active_sets(g)
        maps=[hint_map(g,a,S,lambda S_,b_,b=boost:b) for _,a in sets]
        errs=[]
        for n,(fy,y1) in enumerate(zip(g['fy'],g['y1'])):
            i=g['fyi'][n] if g.get('fyi') else n     # the index in this library's outline, which a hint mask counts
            k=0
            for j,(f,_) in enumerate(sets):
                if f<=i: k=j
            errs.append((map_point(maps[k],fy*1024,S)>>10)-y1)
        ok+=sum(1 for e in errs if e==0); tot+=len(errs); gl+=1
        gok+=all(e==0 for e in errs); gle1+=all(abs(e)<=1 for e in errs)
    if gl:
        print('%-34s %6d glyphs: points exact %.2f%%  glyphs exact %.2f%%  glyphs within 1/64 %.2f%%'%(
            label,gl,100*ok/tot,100*gok/gl,100*gle1/gl))
if __name__=='__main__':
    global_rows=[g for g in load(sys.argv[1]) if g.get('fy')]
    score([g for g in global_rows if g['p']<40],'below BlueScale, single mask',True)
    score([g for g in global_rows if g['p']<40],'below BlueScale, all glyphs',False)
    score([g for g in global_rows if g['p']>=40],'at or above BlueScale, all glyphs',False)
