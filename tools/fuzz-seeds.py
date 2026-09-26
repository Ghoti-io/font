#!/usr/bin/env python3
"""Write the seed corpus for the fuzz harnesses.

The seeds are generated rather than hand-written, and generated here rather
than committed as opaque blobs, because a seed nobody can read is a seed
nobody can tell has rotted.  Each is a valid input - libFuzzer's job is to
break them, and it starts from something that parses.

The first byte of every seed is the harness's options byte
(documentation/design.md section 14.2), so a seed is not just a font: it is a
font plus the caps to read it under.

This duplicates a little of tests/sfnt_builder.h on purpose.  A seed is an
input, not an expectation: if this file and the builder ever disagree about
the format, the seeds are still valid inputs and the tests are still the
authority.

Usage: fuzz-seeds.py [corpus-directory]
"""

import struct
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
corpus = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "tests/fuzz/corpus"

TRUETYPE = 0x00010000


def tag(text):
    return struct.unpack(">I", text.encode("ascii"))[0]


def checksum(data, is_head):
    total = 0
    padded = data + b"\0" * (-len(data) % 4)
    if is_head:
        padded = padded[:8] + b"\0\0\0\0" + padded[12:]
    for i in range(0, len(padded), 4):
        total = (total + struct.unpack(">I", padded[i:i + 4])[0]) & 0xFFFFFFFF
    return total


def font(tables, flavour=TRUETYPE):
    """An sfnt with the given {tag: bytes}, in sorted tag order."""
    items = sorted(tables.items())
    count = len(items)
    entry_selector = 0
    while (1 << (entry_selector + 1)) <= count:
        entry_selector += 1
    search_range = 16 << entry_selector
    out = bytearray(struct.pack(">IHHHH", flavour, count, search_range,
                                entry_selector, count * 16 - search_range))
    directory = len(out)
    out += b"\0" * (count * 16)
    for index, (name, data) in enumerate(items):
        while len(out) % 4:
            out += b"\0"
        offset = len(out)
        out += data
        struct.pack_into(">IIII", out, directory + index * 16, tag(name),
                         checksum(data, name == "head"), offset, len(data))
    return bytes(out)


def collection(fonts):
    """A ttcf whose faces each carry their own tables."""
    out = bytearray(struct.pack(">IHHI", tag("ttcf"), 1, 0, len(fonts)))
    offsets = len(out)
    out += b"\0" * (len(fonts) * 4)
    for index, tables in enumerate(fonts):
        while len(out) % 4:
            out += b"\0"
        struct.pack_into(">I", out, offsets + index * 4, len(out))
        # The face's own offset table and directory, with absolute offsets.
        body = font(tables)
        start = len(out)
        out += body
        # Rewrite each table offset, which font() wrote relative to the face.
        items = sorted(tables.items())
        for i in range(len(items)):
            at = start + 12 + i * 16 + 8
            relative = struct.unpack_from(">I", out, at)[0]
            struct.pack_into(">I", out, at, relative + start)
    return bytes(out)


def head(units_per_em=1000):
    return struct.pack(">HHIIIHHqqhhhhHHhhh", 1, 0, 0x00010000, 0,
                       0x5F0F3CF5, 0x000B, units_per_em, 0, 0, -100, -250,
                       1200, 900, 0, 8, 2, 0, 0)


def maxp(num_glyphs):
    return struct.pack(">IH", 0x00010000, num_glyphs) + b"\0" * 26


def hhea(metrics):
    return struct.pack(">HHhhhHhhhhhh", 1, 0, 800, -200, 100, 1500, -30, -40,
                       1250, 1, 0, 0) + b"\0" * 8 + struct.pack(">hH", 0,
                                                                metrics)


def hmtx(entries, trailing=()):
    out = b"".join(struct.pack(">Hh", advance, bearing)
                   for advance, bearing in entries)
    return out + b"".join(struct.pack(">h", b) for b in trailing)


def delta16(value):
    """idDelta as the file spells it: the arithmetic is modulo 65536, so a
    delta that maps 0xF041 to glyph 1 is a small positive number rather than a
    large negative one."""
    value %= 65536
    return value - 65536 if value >= 32768 else value


def cmap_format4(segments):
    """segments: [(start, end, delta)], delta runs only."""
    count = len(segments)
    entry_selector = 0
    while (1 << (entry_selector + 1)) <= count:
        entry_selector += 1
    body = struct.pack(">HHH", count * 2, 2 << entry_selector, entry_selector)
    body += struct.pack(">H", count * 2 - (2 << entry_selector))
    body += b"".join(struct.pack(">H", end) for _, end, _ in segments)
    body += b"\0\0"
    body += b"".join(struct.pack(">H", start) for start, _, _ in segments)
    body += b"".join(struct.pack(">h", delta16(delta))
                     for _, _, delta in segments)
    body += b"\0" * (count * 2)
    return struct.pack(">HHH", 4, len(body) + 6, 0) + body


def cmap_format12(groups):
    body = b"".join(struct.pack(">III", *group) for group in groups)
    return struct.pack(">HHIII", 12, 0, 16 + len(body), 0, len(groups)) + body


def cmap_format0(mapping):
    array = bytearray(256)
    for code, glyph in mapping.items():
        array[code] = glyph
    return struct.pack(">HHH", 0, 262, 0) + bytes(array)


def cmap(records):
    """records: [(platform, encoding, subtable bytes)]."""
    out = bytearray(struct.pack(">HH", 0, len(records)))
    offsets = len(out)
    out += b"\0" * (len(records) * 8)
    for index, (platform, encoding, subtable) in enumerate(records):
        struct.pack_into(">HHI", out, offsets + index * 8, platform, encoding,
                         len(out))
        out += subtable
    return bytes(out)


def name(records):
    """records: [(platform, encoding, language, name_id, text bytes)]."""
    storage_offset = 6 + len(records) * 12
    out = bytearray(struct.pack(">HHH", 0, len(records), storage_offset))
    storage = bytearray()
    for platform, encoding, language, name_id, text in records:
        out += struct.pack(">HHHHHH", platform, encoding, language, name_id,
                           len(text), len(storage))
        storage += text
    return bytes(out + storage)


def utf16be(text):
    return text.encode("utf-16-be")


def os2(version=4):
    out = struct.pack(">HhHHH", version, 600, 400, 5, 0)
    out += struct.pack(">hhhhhhhhhh", 650, 600, 0, 75, 650, 600, 0, 350, 50,
                       250)
    out += struct.pack(">h", 0x0801)
    out += bytes(range(1, 11))
    out += struct.pack(">IIII", 0x10000000, 0x20000000, 0x30000000, 0x40000000)
    out += b"GHTI"
    out += struct.pack(">HHHhhhHH", 0x0080, 0x20, 0xFFFD, 800, -200, 100, 900,
                       250)
    if version >= 1:
        out += struct.pack(">II", 0x1F, 0)
    if version >= 2:
        out += struct.pack(">hhHHH", 500, 700, 0, 0x20, 3)
    if version >= 5:
        out += struct.pack(">HH", 80, 240)
    return out


def post(version=0x00030000):
    return struct.pack(">IIhhIIIII", version, 0xFFF40000, -75, 50, 1, 0, 0, 0,
                       0)


def write(path, options, payload):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes([options]) + payload)
    print("%s: %d bytes" % (path.relative_to(root), 1 + len(payload)))


def write_pair(path, options, second, payload):
    """A seed for a harness whose header is two bytes rather than one.

    `fuzz_glyf` reads a split point after its options byte and `fuzz_raster`
    reads a shape byte, so a seed written with one header byte would have its
    first payload byte eaten - and would still be a valid input, which is how a
    seed comes to exercise something other than what it was written for.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes([options, second]) + payload)
    print("%s: %d bytes" % (path.relative_to(root), 2 + len(payload)))


def simple_glyph(contours, instructions=b""):
    """A `glyf` simple glyph from [[(x, y, on_curve), ...], ...]."""
    points = [point for contour in contours for point in contour]
    ends = []
    total = 0
    for contour in contours:
        total += len(contour)
        ends.append(total - 1)
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    out = bytearray(struct.pack(">hhhhh", len(contours),
                                min(xs) if xs else 0, min(ys) if ys else 0,
                                max(xs) if xs else 0, max(ys) if ys else 0))
    for end in ends:
        out += struct.pack(">H", end)
    out += struct.pack(">H", len(instructions)) + instructions
    # One flag byte per point, never repeated: a seed is a starting point and
    # the fuzzer is what finds the REPEAT encoding.
    for point in points:
        out += bytes([1 if point[2] else 0])
    previous = 0
    for x in xs:
        out += struct.pack(">h", x - previous)
        previous = x
    previous = 0
    for y in ys:
        out += struct.pack(">h", y - previous)
        previous = y
    return bytes(out)


def composite_glyph(components):
    """A `glyf` composite from [(glyph, dx, dy), ...], words throughout."""
    out = bytearray(struct.pack(">hhhhh", -1, 0, 0, 1000, 1000))
    for index, (glyph, dx, dy) in enumerate(components):
        flags = 0x0001 | 0x0002  # ARG_1_AND_2_ARE_WORDS | ARGS_ARE_XY_VALUES
        if index + 1 < len(components):
            flags |= 0x0020      # MORE_COMPONENTS
        out += struct.pack(">HHhh", flags, glyph, dx, dy)
    return bytes(out)


def loca_short(offsets):
    return b"".join(struct.pack(">H", offset // 2) for offset in offsets)


def raster_points(contours):
    """The point records fuzz_raster reads: x, y, tag, five bytes each."""
    out = bytearray()
    for contour in contours:
        for index, (x, y, tag_value) in enumerate(contour):
            out += struct.pack(">hh", x, y)
            out += bytes([tag_value | (0x40 if index == 0 else 0)])
    return bytes(out)


def main():
    latin = {
        "head": head(),
        "maxp": maxp(5),
        "hhea": hhea(3),
        "hmtx": hmtx([(500, 10), (600, 20), (700, 30)], (40, 50)),
        "cmap": cmap([(3, 1, cmap_format4([(0x41, 0x45, -0x41 + 1),
                                           (0xFFFF, 0xFFFF, 1)]))]),
        "name": name([(3, 1, 0x409, 1, utf16be("Ghoti Seed")),
                      (3, 1, 0x409, 6, utf16be("GhotiSeed-Regular")),
                      (1, 0, 0, 1, b"Ghoti Seed")]),
        "OS/2": os2(4),
        "post": post(),
    }
    astral = dict(latin)
    astral["cmap"] = cmap([
        (1, 0, cmap_format0({0x41: 1})),
        (3, 1, cmap_format4([(0x41, 0x45, -0x41 + 1), (0xFFFF, 0xFFFF, 1)])),
        (3, 10, cmap_format12([(0x41, 0x45, 1), (0x1F600, 0x1F60F, 10)])),
    ])

    write(corpus / "sfnt/latin.seed", 0x00, font(latin))
    write(corpus / "sfnt/astral-dumped.seed", 0x40, font(astral))
    write(corpus / "sfnt/capped.seed", 0x0F, font(latin))
    write(corpus / "sfnt/collection.seed", 0x10,
          collection([latin, {"head": head(2048), "maxp": maxp(2)}]))
    write(corpus / "sfnt/cff-flavoured.seed", 0x00,
          font({"CFF ": b"\x01\x00\x04\x01", "maxp": maxp(1)},
               flavour=tag("OTTO")))

    write(corpus / "cmap/format4.seed", 0x00,
          cmap([(3, 1, cmap_format4([(0x41, 0x45, -0x41 + 1),
                                     (0xFFFF, 0xFFFF, 1)]))]))
    write(corpus / "cmap/format12.seed", 0x80,
          cmap([(3, 10, cmap_format12([(0x41, 0x45, 1),
                                       (0x1F600, 0x1F60F, 10)]))]))
    write(corpus / "cmap/format0.seed", 0x00,
          cmap([(1, 0, cmap_format0({0x41: 1, 0xFF: 2}))]))
    write(corpus / "cmap/symbol.seed", 0x41,
          cmap([(3, 0, cmap_format4([(0xF041, 0xF045, -0xF041 + 1),
                                     (0xFFFF, 0xFFFF, 1)]))]))

    # glyf and loca together, with the split byte placed so that the whole loca
    # and the whole glyf are where the harness looks for them. One seed per
    # construction the reader has a branch for.
    leaf = simple_glyph([[(100, 0, True), (100, 400, False),
                          (400, 400, True), (400, 0, False)]])
    implied = simple_glyph([[(100, 0, True), (200, 500, False),
                             (400, 500, False), (500, 0, True)]])
    hinted = simple_glyph([[(0, 0, True), (100, 0, True), (100, 100, True)]],
                          instructions=b"\x00\x01\x02")
    stack = composite_glyph([(1, 100, 50), (2, -200, 300)])
    for label, glyphs in (("simple", [leaf, implied]),
                          ("instructions", [hinted]),
                          ("composite", [leaf, implied, stack])):
        blob = b""
        offsets = [0]
        for glyph in glyphs:
            blob += glyph + (b"\0" * (-len(glyph) % 2))
            offsets.append(len(blob))
        table = loca_short(offsets)
        # The split byte is a fraction of the payload: 256 * len(loca) / total.
        total = len(table) + len(blob)
        split = max(1, min(255, (256 * len(table)) // total))
        write_pair(corpus / ("glyf/%s.seed" % label), 0x00, split,
                   table + blob)
    # And one with a backwards entry, which condemns a single glyph (M11).
    broken = loca_short([0, len(leaf) + 2, 2]) + leaf + b"\0\0"
    write_pair(corpus / "glyf/backwards-loca.seed", 0x10,
               max(1, (256 * 6) // len(broken)), broken)

    # Paths for the rasteriser, which reads no font at all.
    write_pair(corpus / "raster/square.seed", 0x00, 0x00, raster_points([
        [(0, 0, 0), (256, 0, 0), (256, 256, 0), (0, 256, 0)]]))
    write_pair(corpus / "raster/curve.seed", 0x40, 0x08, raster_points([
        [(0, 0, 0), (256, 640, 1), (512, 0, 0)]]))
    write_pair(corpus / "raster/cubic.seed", 0x08, 0x00, raster_points([
        [(0, 0, 0), (0, 640, 2), (512, 640, 2), (512, 0, 0)]]))
    write_pair(corpus / "raster/hole.seed", 0x10, 0x20, raster_points([
        [(0, 0, 0), (640, 0, 0), (640, 640, 0), (0, 640, 0)],
        [(128, 128, 0), (128, 512, 0), (512, 512, 0), (512, 128, 0)]]))
    write_pair(corpus / "raster/one-point.seed", 0x01, 0x10, raster_points([
        [(64, 64, 0)]]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
