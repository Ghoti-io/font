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
"""Whether a lookup *would* substitute a glyph pair, against HarfBuzz.

The Indic shapers ask the font whether `pref`, `blwf` or `pstf` would act on a
halant and a consonant, and place the consonant by the answer. The question is
not a thing a shaped string shows directly, so this builds an old-spec Devanagari
font per seed whose only lookup is one random context (kind 5) or chained
context (kind 6) subtable under `pref`, and compares the order of the glyphs of
five strings with HarfBuzz's: a wrong answer moves a halant behind a Ra.

What it found when it was first run: format 3 chained rules compared the glyph
after the first against the first's coverage (an off-by-one), and formats 2 and 3
did not require the first glyph to be in the coverage, as HarfBuzz's lookup digest
does. 1,000 seeds of each kind agree now.

Usage: would_random_diff.py [N [KIND]]    N seeds (default 300), KIND 1-6 (default 6)
"""

import json
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env
import gsub_random_diff as G
from gsub_random_diff import u16, u32, base_font, lookup_table
W = os.path.join(oracle_env.ROOT, 'build', 'oracle', 'would-random')
os.makedirs(W, exist_ok=True)
DRV = G.DRIVER
kind = int(sys.argv[2]) if len(sys.argv) > 2 else 6   # 1-4 simple, 5 context, 6 chain
N = int(sys.argv[1]) if len(sys.argv) > 1 else 300
texts=['क्रक','क्क','र्क','क्र','क्खक']
open(W+'/t.txt','w',encoding='utf-8').write(''.join(t+'\n' for t in texts))
lines=[];fs=[]
for seed in range(N):
    g=G.Gen(seed,G.GLYPHS)
    sub=g.simple(kind) if kind<5 else g.context(kind,[],3)
    lk=lookup_table(kind,0,[sub])
    fl=u16(1)+b'pref'+u16(8)+u16(0,1,0)
    script=u16(4,0)+u16(0,0xffff,1,0)
    sl=u16(1)+b'deva'+u16(8)+script
    ll=u16(1,4)+lk
    gsub=u32(0x10000)+u16(10,10+len(sl),10+len(sl)+len(fl))+sl+fl+ll
    f=f'{W}/w{seed}.ttf'
    open(f,'wb').write(base_font(nglyphs=30,cmap_map=G.DEVA,extra={'GSUB':gsub}))
    lines.append(f"hb-shape --font-file={f} --output-format=json --no-glyph-names --script=Deva --text-file={W}/t.txt > {f}.hb"); fs.append((seed,f,struct.unpack('>H',sub[:2])[0]))
open(W+'/w.sh','w').write('\n'.join(lines)+'\n')
subprocess.run(oracle_env.command("harfbuzz",["sh",W+'/w.sh'],scratch=W),capture_output=True)
bad=0;byfmt={}
for seed,f,fmt in fs:
    hb=[[x['g'] for x in json.loads(l)] if l.startswith('[') else [] for l in open(f+'.hb').read().split('\n')[:-1]]
    ours=[[x['g'] for x in json.loads(l)] for l in subprocess.run([DRV,'--batch','--script','deva',f],input=''.join(t+'\n' for t in texts),capture_output=True,text=True).stdout.split('\n')[:-1]]
    if hb!=ours:
        bad+=1; byfmt[fmt]=byfmt.get(fmt,0)+1
        if bad<=3: print('seed',seed,'fmt',fmt,[ (a,b) for a,b in zip(hb,ours) if a!=b][:2])
print('would_random_diff: kind %d, %d seeds, %d differ' % (kind, N, bad))
sys.exit(1 if bad else 0)
