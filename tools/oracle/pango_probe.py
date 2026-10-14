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
"""Pango's layout of a list of jobs, read back as the extent of every character.

    pango_probe.py <jobs.json> <results.json> <fonts-dir>

Runs **inside the pango image**. Each job names a family (found in <fonts-dir>, the only
font directory fontconfig is told about, so no font in the image or on the host can answer by
accident), a pixel size, a base gravity and gravity hint, optionally a width to wrap at, and a
text. The result is, per job, the rectangle (x, y, width, height) Pango gives each character
(`index_to_pos`), the logical extents and the line count, all in pixels and in Pango's own frame:
for a vertical gravity that is the frame in which the text is laid out horizontally, to be turned
a quarter to read as the page.
"""

import json
import os
import sys

import gi
gi.require_version('Pango', '1.0')
gi.require_version('PangoCairo', '1.0')
from gi.repository import Pango, PangoCairo

here = sys.argv[3]
conf = os.path.join(here, 'fonts.conf')
with open(conf, 'w') as h:
    h.write('<?xml version="1.0"?><fontconfig><dir>%s</dir><cachedir>%s/cache</cachedir>'
            '</fontconfig>' % (here, here))
os.environ['FONTCONFIG_FILE'] = conf

fm = PangoCairo.FontMap.get_default()
ctx = fm.create_context()
jobs = json.load(open(sys.argv[1]))
out = []
for j in jobs:
    ctx.set_base_gravity(getattr(Pango.Gravity, j['gravity']))
    ctx.set_gravity_hint(getattr(Pango.GravityHint, j['hint']))
    lay = Pango.Layout.new(ctx)
    desc = Pango.FontDescription()
    desc.set_family(j['family'])
    desc.set_absolute_size(j['size'] * Pango.SCALE)
    font = ctx.load_font(desc)
    if font is None:
        raise RuntimeError('could not load pinned oracle font: %s' % j['family'])
    metrics = font.get_metrics(ctx.get_language())
    lay.set_font_description(desc)
    lay.set_text(j['text'], -1)
    attrs = Pango.AttrList()
    if j.get('line_height') is not None:
        attrs.insert(Pango.attr_line_height_new(float(j['line_height'])))
    if j.get('rise') is not None:
        attrs.insert(Pango.attr_rise_new(int(round(j['rise'] * Pango.SCALE))))
    if j.get('line_height') is not None or j.get('rise') is not None:
        lay.set_attributes(attrs)
    if j.get('room'):
        lay.set_width(int(j['room'] * Pango.SCALE))
    lay.set_wrap(Pango.WrapMode.WORD)   # a word wider than the room overflows, as in a paragraph here
    chars = []
    pos = 0
    for ch in j['text']:
        r = lay.index_to_pos(pos)
        chars.append([r.x / Pango.SCALE, r.y / Pango.SCALE, r.width / Pango.SCALE, r.height / Pango.SCALE])
        pos += len(ch.encode('utf-8'))
    ink, logical = lay.get_extents()
    out.append({'name': j['name'], 'chars': chars,
                'ascent': metrics.get_ascent() / Pango.SCALE,
                'logical': [logical.x / Pango.SCALE, logical.y / Pango.SCALE, logical.width / Pango.SCALE, logical.height / Pango.SCALE],
                'lines': lay.get_line_count()})
json.dump(out, open(sys.argv[2], 'w'))
