"""Minimal AsciiDoc -> single HTML preview for the WS guide draft.

Only the constructs the draft uses: = headings, include::, image:: with
.Title, [cols] |=== tables, NOTE:/TIP:, *bold*, _italic_, `code`,
kbd:[], menu:A[B], URL[text], <<ref>>, // comments. Not a replacement
for asciidoctor; images are embedded so the page is one file.
usage: python preview.py en/ws-main.adoc out.html
"""
import base64
import html
import pathlib
import re
import sys

src = pathlib.Path(sys.argv[1])
root = src.parent


def expand(path):
    out = []
    for line in path.read_text(encoding="utf-8").splitlines():
        m = re.match(r"include::(.+?)\[\]", line)
        out += expand(path.parent / m.group(1)) if m else [line]
    return out


def inline(s):
    s = html.escape(s, quote=False)
    s = re.sub(r"(https?://[^\s\[]+)\[([^\]]*)\]", r'<a href="\1">\2</a>', s)
    s = re.sub(r"kbd:\[([^\]]+)\]", r"<kbd>\1</kbd>", s)
    s = re.sub(r"menu:([^\[]+)\[([^\]]*)\]",
               lambda m: '<span class="menu">' + " &rsaquo; ".join(x for x in [m.group(1)] + ([m.group(2)] if m.group(2) else [])) + "</span>", s)
    s = re.sub(r"&lt;&lt;(\w+)&gt;&gt;", r'<a href="#\1">\1</a>', s)
    s = re.sub(r"(?<![\w*])\*([^*]+)\*(?![\w*])", r"<b>\1</b>", s)
    s = re.sub(r"(?<![\w_])_([^_]+)_(?![\w_])", r"<i>\1</i>", s)
    s = re.sub(r"`([^`]+)`", r"<code>\1</code>", s)
    return s


def img(name, alt):
    p = root / "images" / name
    data = base64.b64encode(p.read_bytes()).decode()
    return f'<img alt="{html.escape(alt)}" src="data:image/png;base64,{data}">'


def inline_img(s):
    # inline image:name[alt] and " +" hard line breaks, around inline()
    raw = []

    def keep(m):
        raw.append(img(m.group(1), m.group(2)))
        return f"\x02{len(raw) - 1}\x02"
    s = re.sub(r"image:([^\[:\s]+)\[([^\]]*)\]", keep, s)
    s = inline(s.replace(" +\n", "\x01").replace("\n", " "))
    s = s.replace("\x01", "<br>")
    return re.sub(r"\x02(\d+)\x02", lambda m: raw[int(m.group(1))], s)


def block_html(ls):
    """Cell content of an a| cell: paragraphs, lists, block images, a nested !=== table."""
    out, para, items = [], [], []

    def fp():
        if para:
            out.append("<p>" + inline_img("\n".join(para)) + "</p>")
            para.clear()

    def fl():
        if items:
            out.append("<ul>" + "".join(f"<li>{inline_img(x)}</li>" for x in items) + "</ul>")
            items.clear()
    j = 0
    while j < len(ls):
        t = ls[j]
        if not t.strip():
            fp(); fl(); j += 1; continue
        if t.startswith("[cols"):
            j += 1; continue
        if t.startswith("!==="):
            fp(); fl(); j += 1
            cells, cur = [], None
            while j < len(ls) and not ls[j].startswith("!==="):
                s = ls[j]
                if s.startswith("!"):
                    cur = [s[1:]]; cells.append(cur)
                elif s.strip() and cur is not None:
                    cur.append(s)
                j += 1
            j += 1
            rows = [cells[k:k + 2] for k in range(0, len(cells), 2)]
            out.append('<table class="nested">' + "".join(
                "<tr>" + "".join(f"<td>{inline_img(chr(10).join(c))}</td>" for c in r) + "</tr>" for r in rows) + "</table>")
            continue
        m = re.match(r"image::(.+?)\[([^,\]]*)", t)
        if m:
            fp(); fl(); out.append(f"<figure>{img(m.group(1), m.group(2))}</figure>"); j += 1; continue
        m = re.match(r"[*.] (.*)", t)
        if m:
            fp(); items.append(m.group(1)); j += 1; continue
        if items and not para:
            items[-1] += "\n" + t
        else:
            para.append(t)
        j += 1
    fp(); fl()
    return "".join(out)


lines = [l for l in expand(src) if not l.startswith("//")]
body, toc, title = [], [], ""
i, para, caption, anchor, discrete = 0, [], None, None, False
notitle = ":notitle:" in lines


def flush():
    global para
    if para:
        body.append("<p>" + inline(" ".join(para)) + "</p>")
        para = []


while i < len(lines):
    l = lines[i]
    if not l.strip():
        flush(); i += 1; continue
    if l.startswith("[discrete]"):
        flush(); discrete = True; i += 1; continue
    if l.startswith(":") or l.startswith("[preface]") or re.match(r"\[cols=", l):
        flush(); i += 1; continue
    m = re.match(r"\[\[(\w+)\]\]", l)
    if m:
        flush(); anchor = m.group(1); i += 1; continue
    m = re.match(r"(=+) (.*)", l)
    if m:
        flush()
        level = len(m.group(1))
        if level == 1:
            title = m.group(2)
        else:
            hid = anchor or re.sub(r"\W+", "-", m.group(2).lower()).strip("-")
            body.append(f'<h{level} id="{hid}">{inline(m.group(2))}</h{level}>')
            if level <= 3 and not discrete:
                toc.append((level, hid, m.group(2)))
        anchor = None; discrete = False; i += 1; continue
    m = re.match(r"\.(\S.*)", l)
    if m:
        flush(); caption = m.group(1); i += 1; continue
    m = re.match(r"image::(.+?)\[([^,\]]*)", l)
    if m:
        flush()
        cap = f"<figcaption>{inline(caption)}</figcaption>" if caption else ""
        body.append(f"<figure>{img(m.group(1), m.group(2))}{cap}</figure>")
        caption = None; i += 1; continue
    m = re.match(r"(NOTE|TIP|IMPORTANT|WARNING): (.*)", l)
    if m:
        flush()
        text = [m.group(2)]
        i += 1
        while i < len(lines) and lines[i].strip():
            text.append(lines[i]); i += 1
        body.append(f'<div class="adm {m.group(1).lower()}"><span>{m.group(1).title()}</span><p>{inline(" ".join(text))}</p></div>')
        continue
    if l.startswith("|==="):
        flush()
        i += 1
        cells, cur, nested = [], None, False
        while not lines[i].startswith("|==="):
            t = lines[i]
            if t.startswith("!==="):
                nested = not nested
            if not nested and re.match(r"a?\|", t):
                parts = t[t.index("|") + 1:].split("|") if t.startswith("|") else [t[2:]]
                for p in parts:
                    cur = [p.strip()]
                    cells.append(cur)
            elif cur is not None:
                cur.append(t)
            i += 1
        i += 1
        rows = [cells[k:k + 2] for k in range(0, len(cells), 2)]
        out = ["<table>"]
        for n, r in enumerate(rows):
            tag = "th" if n == 0 else "td"
            out.append("<tr>" + "".join(f"<{tag}>{block_html(c)}</{tag}>" for c in r) + "</tr>")
        body.append("\n".join(out) + "</table>")
        continue
    m = re.match(r"[*.] (.*)", l)
    if m:
        flush()
        items = []
        while i < len(lines) and lines[i].strip():
            mm = re.match(r"[*.] (.*)", lines[i])
            if mm:
                items.append(mm.group(1))
            elif items:
                items[-1] += "\n" + lines[i]
            i += 1
        body.append("<ul>" + "".join(f"<li>{inline_img(x)}</li>" for x in items) + "</ul>")
        continue
    if "image:" in l:
        flush(); body.append("<p>" + inline_img(l) + "</p>"); i += 1; continue
    para.append(l)
    i += 1
flush()

toc_html = "".join(f'<li class="l{lv}"><a href="#{hid}">{inline(t)}</a></li>' for lv, hid, t in toc)
page = f"""<title>{html.escape(title)}</title>
<style>
:root {{ --ink:#1d232b; --muted:#5b6673; --rule:#d9dee4; --paper:#fbfbf9; --accent:#1f5fae; --th:#eef1f4; }}
body {{ background:var(--paper); color:var(--ink); font:15px/1.55 "Segoe UI",system-ui,sans-serif; margin:0; padding-inline:16px; }}
.wrap {{ max-width:1000px; margin:0 auto; padding-block:28px 60px; }}
h1 {{ font-size:2rem; margin:0 0 .3em; }}
h2 {{ font-size:1.5rem; border-bottom:2px solid var(--rule); padding-bottom:.2em; margin-top:2em; }}
h3 {{ font-size:1.25rem; margin-top:1.8em; }}
h4 {{ font-size:1.05rem; margin:1.6em 0 .5em; color:var(--accent); }}
nav ul {{ list-style:none; padding:0; columns:2; }} nav li.l3 {{ padding-left:1.2em; }}
a {{ color:var(--accent); }}
figure {{ margin:1.2em 0; text-align:center; }} figure img {{ max-width:100%; border:1px solid var(--rule); box-shadow:0 2px 10px #0001; }}
figcaption {{ color:var(--muted); font-size:.9em; margin-top:.4em; }}
.tablewrap, table {{ max-width:100%; }}
table {{ border-collapse:collapse; width:100%; margin:.6em 0 1.2em; display:block; overflow-x:auto; }}
th, td {{ border:1px solid var(--rule); padding:6px 10px; vertical-align:top; text-align:left; }}
th {{ background:var(--th); }} td:first-child {{ width:32%; }}
td p {{ margin:0 0 .5em; }} td p:last-child {{ margin-bottom:0; }} td figure {{ text-align:left; margin:.4em 0; }}
table.nested {{ display:table; margin:.5em 0 0; }} table.nested td {{ font-size:.95em; }}
td img, p img {{ vertical-align:top; margin:2px 6px 2px 0; border:1px solid var(--rule); }}
kbd {{ border:1px solid #b9c0c8; border-bottom-width:2px; border-radius:3px; padding:0 4px; font-size:.85em; background:#fff; }}
.menu {{ font-weight:600; white-space:nowrap; }}
code {{ background:#eef1f4; padding:0 3px; border-radius:3px; }}
.adm {{ display:flex; gap:12px; border-left:4px solid var(--accent); background:#eef3fa; padding:8px 12px; margin:1em 0; }}
.adm span {{ font-weight:700; color:var(--accent); min-width:3.5em; }} .adm p {{ margin:0; }}
.draft {{ background:#fff4cc; border:1px solid #e6cf7a; padding:6px 10px; font-size:.9em; }}
</style>
<div class="wrap">
{body.pop(0) if notitle and body and body[0].startswith("<figure>") else ""}
{"" if notitle else f"<h1>{html.escape(title)}</h1>"}
<nav><ul>{toc_html}</ul></nav>
{chr(10).join(body)}
</div>
"""
pathlib.Path(sys.argv[2]).write_text(page, encoding="utf-8")
print("wrote", sys.argv[2], len(page) // 1024, "KB")
