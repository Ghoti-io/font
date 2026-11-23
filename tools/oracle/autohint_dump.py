#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Font.
"""Dump the Latin letters of faces for autohint_model.py: outlines in font units, with their on-curve flags.

    autohint_dump.py OUT.json FONT...        (runs in the fontTools image; see autohint_zones_diff.py)
"""
import sys,json
from fontTools.ttLib import TTFont
from fontTools.pens.recordingPen import DecomposingRecordingPen
out={}
chars="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
for path in sys.argv[2:]:
    f=TTFont(path); gs=f.getGlyphSet(); cmap=f.getBestCmap(); order=f.getGlyphOrder()
    upem=f['head'].unitsPerEm
    d={"upem":upem,"glyphs":{}}
    for ch in chars:
        name=cmap.get(ord(ch))
        if not name: continue
        gid=order.index(name)
        if 'glyf' in f:
            g=f['glyf'][name]; coords,ends,flags=g.getCoordinates(f['glyf'])
            pts=[[int(x),int(y),int(fl&1)] for (x,y),fl in zip(coords,flags)]
            d["glyphs"][ch]={"gid":gid,"pts":pts,"ends":list(ends)}
        else:
            pen=DecomposingRecordingPen(gs); gs[name].draw(pen)
            pts=[];ends=[]
            for op,args in pen.value:
                if op=="moveTo": pts.append([*map(int,args[0]),1])
                elif op=="lineTo": pts.append([*map(int,args[0]),1])
                elif op=="curveTo":
                    for a in args[:-1]: pts.append([*map(int,a),0])
                    pts.append([*map(int,args[-1]),1])
                elif op=="closePath": ends.append(len(pts)-1)
            d["glyphs"][ch]={"gid":gid,"pts":pts,"ends":ends}
    out[path.split('/')[-1]]=d
json.dump(out,open(sys.argv[1],"w"))
print(len(out))
