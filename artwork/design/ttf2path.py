"""Minimal TrueType reader: turn a string into an SVG path using a font's glyph outlines.

Enough for simple 'glyf' fonts such as DejaVu Sans and Liberation Sans: reads
head, maxp, hhea, hmtx, cmap (format 4), loca, glyf and the legacy 'kern' table
if present. Composite glyphs are followed one level deep with x/y offsets only.
Outlines are quadratic, written as SVG Q commands, so letters need no font on the
machine that renders the SVG.
"""
import struct


class Font:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        num_tables = struct.unpack(">H", d[4:6])[0]
        self.tables = {}
        for i in range(num_tables):
            tag, _, offset, length = struct.unpack(">4sIII", d[12 + 16 * i:28 + 16 * i])
            self.tables[tag.decode("latin-1")] = (offset, length)
        head = self.tables["head"][0]
        self.units_per_em = struct.unpack(">H", d[head + 18:head + 20])[0]
        self.index_to_loc = struct.unpack(">h", d[head + 50:head + 52])[0]
        maxp = self.tables["maxp"][0]
        self.num_glyphs = struct.unpack(">H", d[maxp + 4:maxp + 6])[0]
        hhea = self.tables["hhea"][0]
        self.ascender, self.descender = struct.unpack(">hh", d[hhea + 4:hhea + 8])
        num_hmetrics = struct.unpack(">H", d[hhea + 34:hhea + 36])[0]
        hmtx = self.tables["hmtx"][0]
        self.advance = []
        for i in range(self.num_glyphs):
            j = min(i, num_hmetrics - 1)
            self.advance.append(struct.unpack(">H", d[hmtx + 4 * j:hmtx + 4 * j + 2])[0])
        self._read_loca()
        self._read_cmap()
        self._read_kern()

    def _read_loca(self):
        d, off = self.data, self.tables["loca"][0]
        if self.index_to_loc == 0:
            self.loca = [2 * struct.unpack(">H", d[off + 2 * i:off + 2 * i + 2])[0] for i in range(self.num_glyphs + 1)]
        else:
            self.loca = [struct.unpack(">I", d[off + 4 * i:off + 4 * i + 4])[0] for i in range(self.num_glyphs + 1)]

    def _read_cmap(self):
        d, cmap = self.data, self.tables["cmap"][0]
        n = struct.unpack(">H", d[cmap + 2:cmap + 4])[0]
        sub = None
        for i in range(n):
            pid, eid, off = struct.unpack(">HHI", d[cmap + 4 + 8 * i:cmap + 12 + 8 * i])
            if (pid, eid) in ((3, 1), (0, 3), (0, 4)) and struct.unpack(">H", d[cmap + off:cmap + off + 2])[0] == 4:
                sub = cmap + off
                break
        if sub is None:
            raise ValueError("no format 4 Unicode cmap")
        seg_x2 = struct.unpack(">H", d[sub + 6:sub + 8])[0]
        segs = seg_x2 // 2
        ends = struct.unpack(f">{segs}H", d[sub + 14:sub + 14 + seg_x2])
        starts = struct.unpack(f">{segs}H", d[sub + 16 + seg_x2:sub + 16 + 2 * seg_x2])
        deltas = struct.unpack(f">{segs}h", d[sub + 16 + 2 * seg_x2:sub + 16 + 3 * seg_x2])
        range_base = sub + 16 + 3 * seg_x2
        offsets = struct.unpack(f">{segs}H", d[range_base:range_base + seg_x2])
        self.cmap = {}
        for s in range(segs):
            for c in range(starts[s], ends[s] + 1):
                if c == 0xFFFF:
                    continue
                if offsets[s] == 0:
                    g = (c + deltas[s]) & 0xFFFF
                else:
                    p = range_base + 2 * s + offsets[s] + 2 * (c - starts[s])
                    g = struct.unpack(">H", d[p:p + 2])[0]
                    if g:
                        g = (g + deltas[s]) & 0xFFFF
                if g:
                    self.cmap[c] = g

    def _read_kern(self):
        self.kern = {}
        if "kern" not in self.tables:
            return
        d, off = self.data, self.tables["kern"][0]
        n = struct.unpack(">H", d[off + 2:off + 4])[0]
        p = off + 4
        for _ in range(n):
            length, coverage = struct.unpack(">HH", d[p + 2:p + 6])
            if coverage >> 8 == 0:
                pairs = struct.unpack(">H", d[p + 6:p + 8])[0]
                for k in range(pairs):
                    left, right, value = struct.unpack(">HHh", d[p + 14 + 6 * k:p + 20 + 6 * k])
                    self.kern[(left, right)] = value
            p += length

    def glyph_contours(self, gid):
        """List of contours, each a list of (x, y, on_curve)."""
        d = self.data
        start = self.tables["glyf"][0] + self.loca[gid]
        if self.loca[gid] == self.loca[gid + 1]:
            return []
        n_contours = struct.unpack(">h", d[start:start + 2])[0]
        p = start + 10
        if n_contours < 0:
            contours = []
            more = True
            while more:
                flags, glyph = struct.unpack(">HH", d[p:p + 4])
                p += 4
                if flags & 1:
                    dx, dy = struct.unpack(">hh", d[p:p + 4])
                    p += 4
                else:
                    dx, dy = struct.unpack(">bb", d[p:p + 2])
                    p += 2
                if flags & 0x8:
                    p += 2
                elif flags & 0x40:
                    p += 4
                elif flags & 0x80:
                    p += 8
                for c in self.glyph_contours(glyph):
                    contours.append([(x + dx, y + dy, on) for x, y, on in c])
                more = bool(flags & 0x20)
            return contours
        ends = struct.unpack(f">{n_contours}H", d[p:p + 2 * n_contours])
        p += 2 * n_contours
        ins_len = struct.unpack(">H", d[p:p + 2])[0]
        p += 2 + ins_len
        n_points = ends[-1] + 1
        flags = []
        while len(flags) < n_points:
            f = d[p]
            p += 1
            flags.append(f)
            if f & 8:
                repeat = d[p]
                p += 1
                flags.extend([f] * repeat)
        coords = []
        for axis, short_bit, same_bit in (("x", 2, 16), ("y", 4, 32)):
            value, out = 0, []
            for f in flags:
                if f & short_bit:
                    delta = d[p]
                    p += 1
                    value += delta if f & same_bit else -delta
                elif not f & same_bit:
                    value += struct.unpack(">h", d[p:p + 2])[0]
                    p += 2
                out.append(value)
            coords.append(out)
        points = [(coords[0][i], coords[1][i], bool(flags[i] & 1)) for i in range(n_points)]
        contours, s = [], 0
        for e in ends:
            contours.append(points[s:e + 1])
            s = e + 1
        return contours

    def text_width(self, text, size, tracking=0.0):
        scale = size / self.units_per_em
        gids = [self.cmap.get(ord(ch), 0) for ch in text]
        w = 0
        for i, g in enumerate(gids):
            w += self.advance[g]
            if i + 1 < len(gids):
                w += self.kern.get((g, gids[i + 1]), 0)
        return w * scale + tracking * size * max(len(text) - 1, 0)

    def text_path(self, text, x, y, size, anchor="start", tracking=0.0):
        """SVG path data for text with its baseline at y. anchor: start, middle or end."""
        scale = size / self.units_per_em
        width = self.text_width(text, size, tracking)
        pen = x - {"start": 0, "middle": width / 2, "end": width}[anchor]
        gids = [self.cmap.get(ord(ch), 0) for ch in text]
        parts = []
        for i, g in enumerate(gids):
            for contour in self.glyph_contours(g):
                parts.append(_contour_path(contour, pen, y, scale))
            pen += self.advance[g] * scale + tracking * size
            if i + 1 < len(gids):
                pen += self.kern.get((g, gids[i + 1]), 0) * scale
        return " ".join(parts)


def _contour_path(contour, ox, oy, scale):
    def pt(p):
        return ox + p[0] * scale, oy - p[1] * scale

    n = len(contour)
    if n == 0:
        return ""
    # start on an on-curve point; if none, start at the midpoint of the first two
    start = next((i for i, p in enumerate(contour) if p[2]), None)
    if start is None:
        a, b = contour[0], contour[1]
        pts = [((a[0] + b[0]) / 2, (a[1] + b[1]) / 2, True)] + contour[1:] + contour[:1]
    else:
        pts = contour[start:] + contour[:start]
    sx, sy = pt(pts[0])
    out = [f"M{sx:.1f},{sy:.1f}"]
    i, m = 1, len(pts)
    while i <= m:
        cur = pts[i % m]
        if cur[2]:
            x, y = pt(cur)
            out.append(f"L{x:.1f},{y:.1f}")
            i += 1
        else:
            nxt = pts[(i + 1) % m]
            if nxt[2]:
                end = nxt
                i += 2
            else:
                end = ((cur[0] + nxt[0]) / 2, (cur[1] + nxt[1]) / 2, True)
                i += 1
            cx, cy = pt(cur)
            ex, ey = pt(end)
            out.append(f"Q{cx:.1f},{cy:.1f} {ex:.1f},{ey:.1f}")
    out.append("Z")
    return " ".join(out)
