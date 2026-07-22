#!/usr/bin/env python3
"""Regenerate the README demo GIF for call-stack-logger.

Renders VS Code Dark+ look-alike frames as HTML, screenshots them with
headless Chrome, and assembles an animated GIF with ImageMagick. All pane
content is real: the editor shows the repo's current src/main.cpp, the
terminal shows captured cmake/make output, and the trace.out views show
captured trace files (see inputs/ and capture_inputs.sh).

Storyline:
  1. main.cpp opens WITHOUT the fibonacci function; it is typed in live
     (function above the helpers, then the fibonacci(6); call in main).
     The typed result is byte-identical to the real src/main.cpp, so the
     line numbers in the trace stay truthful.
  2. Terminal: cmake .. && make run  (real captured output cascades).
  3. trace.out tab: the default call tree.
  4. Terminal: cmake -DLOG_ELAPSED=ON .. && make run.
  5. trace.out tab: same tree with the 12-byte duration column; a purple
     rectangle is drawn around the duration column at the end.

Usage:
  python3 render.py         # full render -> out/call-stack-logger-capture-new.gif
  python3 render.py test    # render a handful of landmark frames -> out/testframes/

If src/main.cpp is restructured, the STRUCTURE ASSERTS below fail with a
message telling you which constants to update.
"""
import html as html_mod
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
INPUTS = os.path.join(HERE, "inputs")
OUT = os.path.join(HERE, "out")
GIF_NAME = "call-stack-logger-capture-new.gif"

W, H = 1200, 912
TERM_COLS = 168          # terminal wrap width (14px Ubuntu Mono, 7px advance)
CHUNK = 8                # frames per Chrome screenshot page
ED_ADV = 7.5             # editor char advance (15px Ubuntu Mono)
ED_TOP = 46              # editor area top y
ROW17 = 17               # editor line height (main.cpp view)
ROW16 = 16.25            # editor line height (trace.out view, 40 lines fit)

# ---------------------------------------------------------------- CSS / chrome
CSS = """
* { margin:0; padding:0; box-sizing:border-box; }
body { background:#1e1e1e; }
.frame { position:relative; width:1200px; height:912px; overflow:hidden;
         background:#1e1e1e; font-family:'Ubuntu Mono','DejaVu Sans Mono',monospace; }
.tabbar { position:absolute; left:0; top:0; width:100%; height:27px;
          background:#252526; display:flex; }
.tab { height:27px; display:flex; align-items:center; justify-content:center;
       background:#2d2d2d; color:#969696; font:12px 'Noto Sans',sans-serif;
       margin-right:1px; gap:6px; }
.tab.active { background:#1e1e1e; color:#e7e7e7; }
.icpp { color:#519aba; font:bold 10px 'Noto Sans',sans-serif; }
.iout { color:#8a9199; font-size:11px; }
.close { color:#c5c5c5; font-size:12px; margin-left:2px; }
.acticons { position:absolute; right:10px; top:0; height:27px; color:#cccccc;
            opacity:.7; font-size:12px; display:flex; gap:12px; align-items:center; }
.breadcrumb { position:absolute; top:27px; left:0; width:100%; height:19px;
              color:#a0a0a0; font:11px 'Noto Sans',sans-serif; display:flex;
              align-items:center; padding-left:18px; gap:6px; background:#1e1e1e; }
.editor { position:absolute; top:46px; left:0; right:0; height:649px; overflow:hidden; }
.eline { position:relative; height:17px; line-height:17px; font-size:15px;
         white-space:pre; color:#d4d4d4; }
.eline16 { position:relative; height:16.25px; line-height:16.25px; font-size:15px;
           white-space:pre; color:#d4d4d4; }
.ln { position:absolute; left:0; width:41px; text-align:right; color:#858585;
      font-size:13px; }
.lc { position:absolute; left:60px; top:0; white-space:pre; }
.curline { outline:1px solid #303030; outline-offset:-1px; }
.blame { color:#6b6b6b; font-size:13px; }
.caret { position:absolute; top:1px; width:1.5px; height:15px; background:#aeafad; }
.kw{color:#569CD6} .ctl{color:#C586C0} .ty{color:#4EC9B0} .fn{color:#DCDCAA}
.st{color:#CE9178} .num{color:#B5CEA8} .cm{color:#6A9955} .var{color:#9CDCFE}
.panelhead { position:absolute; left:0; right:0; top:695px; height:26px;
             display:flex; align-items:center; padding-left:18px; gap:24px;
             font:11px 'Noto Sans',sans-serif; color:#969696; letter-spacing:.4px; }
.pha { color:#e7e7e7; border-bottom:1px solid #e7e7e7; padding-bottom:3px; }
.bashsel { position:absolute; right:76px; top:699px; width:108px; height:19px;
           background:#3c3c3c; color:#cccccc; font:10.5px 'Noto Sans',sans-serif;
           display:flex; align-items:center; padding:0 7px;
           justify-content:space-between; }
.panelicons { position:absolute; right:8px; top:699px; height:19px; color:#c5c5c5;
              opacity:.8; font-size:11px; display:flex; gap:8px; align-items:center; }
.term { position:absolute; left:16px; right:2px; top:728px; height:182px; overflow:hidden; }
.trow { height:14px; line-height:14px; font-size:14px; color:#cccccc; white-space:pre; }
.tg{color:#23d18b;font-weight:bold} .tb{color:#3b8eea;font-weight:bold}
.tyw{color:#f5f543}
.tcur { display:inline-block; width:7px; height:12px; background:#cccccc;
        vertical-align:-2px; }
.mouse { position:absolute; z-index:99; }
.durrect { position:absolute; border:3px solid; border-radius:4px; z-index:50;
           box-shadow:0 0 9px 2px rgba(177,94,222,.5); }
.cmdrect { position:absolute; border:2px solid #B15EDE; border-radius:3px;
           z-index:60; box-shadow:0 0 7px 1px rgba(177,94,222,.55); }
"""

ARROW_SVG = ("data:image/svg+xml;utf8,"
    "<svg xmlns='http://www.w3.org/2000/svg' width='12' height='19'>"
    "<path d='M1 1 L1 15 L4.6 11.6 L7.2 17.4 L9.4 16.4 L6.8 10.7 L11.6 10.7 Z' "
    "fill='white' stroke='black' stroke-width='1'/></svg>")
IBEAM_SVG = ("data:image/svg+xml;utf8,"
    "<svg xmlns='http://www.w3.org/2000/svg' width='9' height='17'>"
    "<path d='M1.5 1.5 H7.5 M4.5 1.5 V15.5 M1.5 15.5 H7.5' stroke='%23dcdcdc' "
    "stroke-width='1.5' fill='none'/></svg>")

# Purple frame drawn around the LOG_ELAPSED duration column at the end.
RECT_COLOR = "#B15EDE"
RECT_BORDER = 3          # px; "not too thick, not too thin"
DUR_CHAR_START = 26      # '[' of the 12-char duration field ("[ts] " = 26 chars)
DUR_CHAR_END = 38        # one past ']'
DUR_ROW_FIRST = 5        # first trace line (after blank + 3 separator rows)
DUR_ROW_LAST = 39        # last trace line with a duration

# ---------------------------------------------------------------- highlighter
KW = r"class|public|static|void|constexpr|unsigned|int|bool|auto|double|template|typename|inline|char|const"
CTL = r"if|return|else"
TY = r"A|B|std|vector|T|Types"

TOKEN_RE = re.compile(
    r'(?P<st>"(?:[^"\\]|\\.)*")'
    r'|(?P<num>\b\d+(?:\.\d+)?\b)'
    r'|(?P<kw>\b(?:' + KW + r')\b)'
    r'|(?P<ctl>\b(?:' + CTL + r')\b)'
    r'|(?P<ty>\b(?:' + TY + r')\b)'
    r'|(?P<fn>\b[A-Za-z_]\w*(?=\s*\())'
    r'|(?P<var>\b[A-Za-z_]\w*\b)'
)

def esc(s):
    return html_mod.escape(s, quote=False)

def cxx_hl(line):
    code, comment = line, None
    m = re.search(r'//.*$', line)
    if m:
        code, comment = line[:m.start()], line[m.start():]
    out, pos = [], 0
    for m in TOKEN_RE.finditer(code):
        out.append(esc(code[pos:m.start()]))
        out.append('<span class="%s">%s</span>' % (m.lastgroup, esc(m.group())))
        pos = m.end()
    out.append(esc(code[pos:]))
    if comment is not None:
        out.append('<span class="cm">%s</span>' % esc(comment))
    return "".join(out)

# ---------------------------------------------------------------- real content
MAIN_CPP = open(os.path.join(REPO, "src", "main.cpp")).read().rstrip("\n").split("\n")

# STRUCTURE ASSERTS — the typing scene inserts the fibonacci function at
# lines 29-34 and the call at lines 64-65. If main.cpp is restructured,
# update these line constants and the typing scene in build_frames().
FIB_BLANK = 29           # blank line above the function
FIB_FIRST, FIB_LAST = 30, 34
CALL_COMMENT, CALL_LINE = 64, 65
assert MAIN_CPP[FIB_BLANK - 1] == "", "expected blank line at %d" % FIB_BLANK
assert MAIN_CPP[FIB_FIRST - 1].startswith("constexpr unsigned fibonacci"), \
    "fibonacci signature moved — update FIB_* constants"
assert MAIN_CPP[FIB_LAST - 1] == "}", "fibonacci closing brace moved"
assert MAIN_CPP[CALL_COMMENT - 1].lstrip().startswith("// Test logging constexpr"), \
    "fibonacci call comment moved — update CALL_* constants"
assert MAIN_CPP[CALL_LINE - 1].strip() == "fibonacci(6);", "fibonacci call moved"

_INPUT_FILES = ("act1-cmake.txt", "act1-makerun.txt", "act3-cmake.txt",
                "act3-makerun.txt", "act2-trace.txt", "act4-trace.txt")
_missing = [f for f in _INPUT_FILES
            if not os.path.exists(os.path.join(INPUTS, f))]
if _missing:
    sys.exit("missing inputs %s — run ./capture_inputs.sh first" % ", ".join(_missing))

def read_lines(p):
    return open(os.path.join(INPUTS, p)).read().rstrip("\n").split("\n")

ACT1_CMAKE = read_lines("act1-cmake.txt")
ACT1_MAKERUN = read_lines("act1-makerun.txt")
ACT3_CMAKE = read_lines("act3-cmake.txt")
ACT3_MAKERUN = read_lines("act3-makerun.txt")
ACT2_TRACE = read_lines("act2-trace.txt")
ACT4_TRACE = read_lines("act4-trace.txt")

# ---------------------------------------------------------------- terminal model
def wrap_row(spans, cols=TERM_COLS):
    rows, cur, width = [], [], 0
    for cls, text in spans:
        if text == "":
            cur.append((cls, ""))
            continue
        while text:
            room = cols - width
            piece, text = text[:room], text[room:]
            cur.append((cls, piece))
            width += len(piece)
            if width >= cols:
                rows.append(cur)
                cur, width = [], 0
    rows.append(cur)
    return rows

def plain_rows(lines):
    rows = []
    for ln in lines:
        rows.extend(wrap_row([(None, ln)] if ln else [(None, "")]))
    return rows

PROMPT = [("tg", "fedora@fedora"), (None, ":"),
          ("tb", "~/projects/call-stack-logger/build"),
          ("tyw", " (master)"), (None, " $ ")]

def prompt_row(typed="", cursor=False):
    spans = PROMPT + [(None, typed)]
    if cursor:
        spans = spans + [("CURSOR", "")]
    return wrap_row(spans)

CMD1 = "cmake .. && make run"
CMD2 = "cmake -DLOG_ELAPSED=ON .. && make run"
OUT1 = plain_rows(ACT1_CMAKE + ACT1_MAKERUN)
OUT2 = plain_rows(ACT3_CMAKE + ACT3_MAKERUN)

# Geometry of the glow frame around "-DLOG_ELAPSED=ON" in the typed command.
CMD2_PARAM = "-DLOG_ELAPSED=ON"
PROMPT_LEN = sum(len(t) for _, t in PROMPT)
TERM_X0, TERM_ADV, TERM_TOP, TERM_ROW = 16, 7, 728, 14
CMDRECT_X = TERM_X0 + (PROMPT_LEN + CMD2.index(CMD2_PARAM)) * TERM_ADV - 3
CMDRECT_W = len(CMD2_PARAM) * TERM_ADV + 6
CMDRECT_Y = TERM_TOP + 12 * TERM_ROW - 2   # command row is the last visible row
CMDRECT_H = TERM_ROW + 4

# ---------------------------------------------------------------- components
TABS = [("trace.cpp", 104, "cpp"), ("callStack.cpp", 128, "cpp"),
        ("main.cpp", 116, "cpp"), ("trace.out", 106, "out")]

def tabbar(active):
    parts = ['<div class="tabbar">']
    for name, width, kind in TABS:
        cls = "tab active" if name == active else "tab"
        icon = '<span class="icpp">C</span>' if kind == "cpp" else '<span class="iout">&#8801;</span>'
        close = '<span class="close">&#215;</span>' if name == active else ""
        parts.append('<div class="%s" style="width:%dpx">%s%s%s</div>'
                     % (cls, width, icon, esc(name), close))
    parts.append('<div class="acticons"><span>&#9707;</span><span>&#8943;</span></div>')
    parts.append('</div>')
    return "".join(parts)

def breadcrumb(view):
    if view == "main":
        inner = ('src <span style="opacity:.6">&#8250;</span> '
                 '<span class="icpp">C</span> main.cpp '
                 '<span style="opacity:.6">&#8250;</span> &#8230;')
    else:
        inner = ('build <span style="opacity:.6">&#8250;</span> '
                 '<span class="iout">&#8801;</span> trace.out')
    return '<div class="breadcrumb">%s</div>' % inner

def editor_main(doc, first, caret=None, blame=None):
    """caret: (line, col) or None; blame text is shown on the caret line."""
    rows = []
    for i in range(38):
        n = first + i
        if n > len(doc):
            break
        text = doc[n - 1]
        content = cxx_hl(text)
        cls, extra = "eline", ""
        if caret and caret[0] == n:
            cls += " curline"
            # relative to .lc, which already sits at the 60px code column
            x = caret[1] * ED_ADV
            extra += '<span class="caret" style="left:%.1fpx"></span>' % x
            if blame:
                extra += ('<span class="blame">%s%s</span>'
                          % ("&nbsp;" * 8, esc(blame)))
        rows.append('<div class="%s"><span class="ln">%d</span>'
                    '<span class="lc">%s%s</span></div>' % (cls, n, content, extra))
    return '<div class="editor">%s</div>' % "".join(rows)

def editor_trace(lines, rect=False):
    rows = []
    if len(lines) < 40:
        lines = lines + [""] * (40 - len(lines))
    for i, text in enumerate(lines[:40]):
        rows.append('<div class="eline16"><span class="ln">%d</span>'
                    '<span class="lc">%s</span></div>' % (i + 1, esc(text)))
    rect_html = ""
    if rect:
        left = 60 + DUR_CHAR_START * ED_ADV - 4
        width = (DUR_CHAR_END - DUR_CHAR_START) * ED_ADV + 8
        top = (DUR_ROW_FIRST - 1) * ROW16 - 3
        height = (DUR_ROW_LAST - DUR_ROW_FIRST + 1) * ROW16 + 6
        opacity = rect if isinstance(rect, float) else 1.0
        rect_html = ('<div class="durrect" style="left:%.0fpx;top:%.0fpx;'
                     'width:%.0fpx;height:%.0fpx;border-color:%s;opacity:%.2f"></div>'
                     % (left, top, width, height, RECT_COLOR, opacity))
    return '<div class="editor">%s%s</div>' % ("".join(rows), rect_html)

def panel():
    return ('<div class="panelhead"><span class="pha">TERMINAL</span>'
            '<span>DEBUG CONSOLE</span><span>PROBLEMS</span><span>OUTPUT</span></div>'
            '<div class="bashsel"><span>1: bash</span><span>&#8964;</span></div>'
            '<div class="panelicons"><span>&#65291;</span><span>&#9636;</span>'
            '<span>&#8964;</span><span>&#10005;</span></div>')

def terminal(rows, max_rows=13):
    out = []
    for row in rows[-max_rows:]:
        spans = []
        for cls, text in row:
            if cls == "CURSOR":
                spans.append('<span class="tcur"></span>')
            elif cls:
                spans.append('<span class="%s">%s</span>' % (cls, esc(text)))
            else:
                spans.append(esc(text))
        out.append('<div class="trow">%s</div>' % "".join(spans))
    return '<div class="term">%s</div>' % "".join(out)

def mouse(x, y, kind):
    src = ARROW_SVG if kind == "arrow" else IBEAM_SVG
    return '<img class="mouse" style="left:%dpx;top:%dpx" src="%s">' % (x, y, src)

def frame_html(state):
    view = state["view"]
    active = "trace.out" if view.startswith("trace") else "main.cpp"
    if view == "main":
        ed = editor_main(state["doc"], state["first"],
                         state.get("caret"), state.get("blame"))
    elif view == "trace1":
        ed = editor_trace(ACT2_TRACE)
    else:
        ed = editor_trace(ACT4_TRACE, rect=state.get("rect", False))
    mx, my, mk = state["mouse"]
    cmdrect = ""
    if state.get("cmdrect") is not None:
        cmdrect = ('<div class="cmdrect" style="left:%dpx;top:%dpx;width:%dpx;'
                   'height:%dpx;opacity:%.2f"></div>'
                   % (CMDRECT_X, CMDRECT_Y, CMDRECT_W, CMDRECT_H,
                      state["cmdrect"]))
    return ('<div class="frame">' + tabbar(active) + breadcrumb(view) + ed +
            panel() + terminal(state["term"]) + cmdrect + mouse(mx, my, mk) +
            '</div>')

# ---------------------------------------------------------------- storyboard
def lerp(a, b, t):
    return (round(a[0] + (b[0] - a[0]) * t), round(a[1] + (b[1] - a[1]) * t))

def line_y(first, line, row=ROW17):
    return round(ED_TOP + (line - first) * row + row / 2)

def build_frames():
    """Returns (frames, landmarks): frames = [(state, delay_cs)], landmarks =
    {name: frame_index} for test rendering."""
    frames, landmarks = [], {}

    def F(delay, **state):
        frames.append((state, delay))

    def mark(name):
        landmarks[name] = len(frames)

    # Editable document: real main.cpp minus the fibonacci function (with its
    # leading blank line) and minus the call + its comment. The typing scene
    # re-inserts them; the result must equal the real file.
    doc = list(MAIN_CPP)
    del doc[CALL_COMMENT - 1:CALL_LINE]
    del doc[FIB_BLANK - 1:FIB_LAST]

    def snap():
        return list(doc)

    t0 = prompt_row("", cursor=True)
    t0_off = prompt_row("", cursor=False)
    typing_delays = [6, 5, 7, 5, 4, 6, 5, 8, 5, 4, 6, 5, 4, 7, 5, 6, 4, 5, 6, 5]

    def Fmain(delay, first, caret=None, blame=None, term=t0, mpos=(520, 340),
              mkind="ibeam", cmdrect=None):
        F(delay, view="main", doc=snap(), first=first, caret=caret, blame=blame,
          term=term, mouse=(mpos[0], mpos[1], mkind), cmdrect=cmdrect)

    # --- opening: main.cpp without fibonacci; mouse drifts to end of class B
    mark("open")
    click1 = (60 + 2 * ED_ADV + 4, line_y(14, 28))
    for p, d in [((520, 340), 65), ((300, 320), 35), (click1, 30)]:
        Fmain(d, 14, mpos=p)

    # --- type the fibonacci function (Enter Enter, then 5 lines, auto-indent)
    caret_line, caret_col = 28, len(doc[27])
    view = {"first": 14, "mpos": click1}   # helpers follow the current viewport
    Fmain(14, 14, caret=(caret_line, caret_col), mpos=click1)

    def enter(indent):
        nonlocal caret_line, caret_col
        doc.insert(caret_line, " " * indent)
        caret_line += 1
        caret_col = indent
        Fmain(7, view["first"], caret=(caret_line, caret_col), mpos=view["mpos"])

    def type_chars(text, chunk):
        nonlocal caret_col
        for i in range(0, len(text), chunk):
            piece = text[i:i + chunk]
            doc[caret_line - 1] += piece
            caret_col += len(piece)
            Fmain(typing_delays[(i // chunk) % len(typing_delays)], view["first"],
                  caret=(caret_line, caret_col), mpos=view["mpos"])

    enter(0)
    enter(0)
    mark("type_fib")
    type_chars("constexpr unsigned fibonacci(unsigned n) {", 2)
    enter(4);  type_chars("if (n <= 1)", 3)
    enter(8);  type_chars("return n;", 3)
    enter(4);  type_chars("return fibonacci(n - 1) + fibonacci(n - 2);", 3)
    enter(0);  type_chars("}", 1)
    mark("fib_done")
    blame_now = "You, seconds ago • Uncommitted changes"
    Fmain(45, 14, caret=(caret_line, caret_col), blame=blame_now, mpos=click1)
    Fmain(35, 14, caret=(caret_line, caret_col), blame=blame_now, term=t0_off,
          mpos=click1)

    # --- scroll calmly down to main() (3 lines per step, like a mouse wheel)
    mark("scroll")
    for f in (17, 20, 23, 26, 29, 32):
        Fmain(6, f, mpos=click1)
    Fmain(16, 35, mpos=click1)
    view["first"] = 35
    # --- move to the b.foo(); line, click, type the comment + call
    click2 = (60 + len(MAIN_CPP[CALL_COMMENT - 2]) * ED_ADV + 4,
              line_y(35, CALL_COMMENT - 1))
    for i in range(1, 4):
        Fmain(5, 35, mpos=lerp(click1, click2, i / 3))
    caret_line, caret_col = CALL_COMMENT - 1, len(doc[CALL_COMMENT - 2])
    view["mpos"] = click2
    Fmain(14, 35, caret=(caret_line, caret_col), mpos=click2)
    mark("type_call")
    enter(4);  type_chars("// Test logging constexpr function", 3)
    enter(4);  type_chars("fibonacci(6);", 2)
    mark("call_done")
    Fmain(45, 35, caret=(caret_line, caret_col), blame=blame_now, mpos=click2)
    Fmain(30, 35, caret=(caret_line, caret_col), blame=blame_now, term=t0_off,
          mpos=click2)

    assert doc == MAIN_CPP, "typed document does not match real src/main.cpp"

    # --- Act 1: type cmake/make in terminal, output cascades
    tpos = (430, 806)
    for i in range(1, 4):
        Fmain(5, 35, mpos=lerp(click2, tpos, i / 3),
              mkind="ibeam" if i < 3 else "ibeam")
    for i, _ in enumerate(CMD1):
        Fmain(typing_delays[i % len(typing_delays)], 35,
              term=prompt_row(CMD1[:i + 1], cursor=True), mpos=tpos)
    Fmain(12, 35, term=prompt_row(CMD1, cursor=False), mpos=tpos)
    mark("cascade1")
    hist1 = prompt_row(CMD1)
    steps = [(0.18, 16), (0.4, 20), (0.55, 26), (0.68, 30), (0.8, 22),
             (0.93, 16), (1.0, 10)]
    for fr, d in steps:
        n = max(1, int(len(OUT1) * fr))
        Fmain(d, 35, term=hist1 + OUT1[:n], mpos=tpos)
    after1 = hist1 + OUT1 + prompt_row("", cursor=True)
    after1_off = hist1 + OUT1 + prompt_row("", cursor=False)
    Fmain(70, 35, term=after1, mpos=tpos)
    Fmain(45, 35, term=after1_off, mpos=tpos)

    # --- Act 2: trace.out (default build)
    tab_trace = (400, 14)
    for i in range(1, 5):
        Fmain(5, 35, term=after1, mpos=lerp(tpos, tab_trace, i / 4), mkind="arrow")
    mark("trace1")
    F(30, view="trace1", term=after1, mouse=(tab_trace[0], tab_trace[1], "arrow"))
    F(170, view="trace1", term=after1_off, mouse=(tab_trace[0], tab_trace[1], "arrow"))
    drift = [(560, 300), (600, 420), (620, 520)]
    for p, d in zip(drift, (45, 45, 90)):
        F(d, view="trace1", term=after1, mouse=(p[0], p[1], "ibeam"))

    # --- Act 3: back to main.cpp, rebuild with LOG_ELAPSED
    tab_main = (290, 14)
    for i in range(1, 4):
        F(5, view="trace1", term=after1,
          mouse=lerp(drift[-1], tab_main, i / 3) + ("arrow",))
    Fmain(25, 35, term=after1, mpos=tab_main, mkind="arrow")
    for i in range(1, 3):
        Fmain(5, 35, term=after1, mpos=lerp(tab_main, tpos, i / 2),
              mkind="arrow" if i < 2 else "ibeam")
    base1 = hist1 + OUT1
    mark("type_cmd2")
    for i, _ in enumerate(CMD2):
        Fmain(max(3, typing_delays[(i * 3) % len(typing_delays)] - 1), 35,
              term=base1 + prompt_row(CMD2[:i + 1], cursor=True), mpos=tpos)
    # --- glow-pulse a purple frame around -DLOG_ELAPSED=ON before Enter
    # (three cycles: fade in, hold, fade out)
    mark("cmd2_glow")
    cmd_on = base1 + prompt_row(CMD2, cursor=True)
    cmd_off = base1 + prompt_row(CMD2, cursor=False)
    for _ in range(3):
        for op, d in ((0.3, 8), (0.65, 8), (1.0, 50)):
            Fmain(d, 35, term=cmd_on, mpos=tpos, cmdrect=op)
        for op, d in ((0.6, 8), (0.25, 8)):
            Fmain(d, 35, term=cmd_off, mpos=tpos, cmdrect=op)
        Fmain(8, 35, term=cmd_off, mpos=tpos)
    Fmain(12, 35, term=base1 + prompt_row(CMD2, cursor=False), mpos=tpos)
    hist2 = base1 + prompt_row(CMD2)
    for fr, d in steps:
        n = max(1, int(len(OUT2) * fr))
        Fmain(d, 35, term=hist2 + OUT2[:n], mpos=tpos)
    after2 = hist2 + OUT2 + prompt_row("", cursor=True)
    after2_off = hist2 + OUT2 + prompt_row("", cursor=False)
    Fmain(55, 35, term=after2, mpos=tpos)

    # --- Act 4: trace.out with durations, then the purple column frame
    for i in range(1, 5):
        Fmain(5, 35, term=after2, mpos=lerp(tpos, tab_trace, i / 4), mkind="arrow")
    mark("trace2")
    F(30, view="trace2", term=after2, mouse=(tab_trace[0], tab_trace[1], "arrow"))
    F(80, view="trace2", term=after2_off, mouse=(tab_trace[0], tab_trace[1], "arrow"))
    # quick glance down the duration column, then the glow starts ~2 s in
    col = [(330, 200), (330, 380), (330, 530)]
    for p, d in zip(col, (25, 25, 30)):
        F(d, view="trace2", term=after2, mouse=(p[0], p[1], "ibeam"))
    aside = (386, 400)
    F(6, view="trace2", term=after2, mouse=(360, 460, "arrow"))
    F(10, view="trace2", term=after2, mouse=aside + ("arrow",))
    # glow-pulse the purple frame around the duration column (three cycles)
    mark("rect")
    for terms in (after2, after2_off, after2):
        for op, d in ((0.3, 8), (0.65, 8), (1.0, 60)):
            F(d, view="trace2", term=terms, mouse=aside + ("arrow",), rect=op)
        for op, d in ((0.65, 8), (0.3, 8)):
            F(d, view="trace2", term=terms, mouse=aside + ("arrow",), rect=op)
        F(10, view="trace2", term=terms, mouse=aside + ("arrow",))
    F(140, view="trace2", term=after2, mouse=aside + ("arrow",))

    return frames, landmarks

# ---------------------------------------------------------------- rendering
def check_fonts():
    out = subprocess.run(["fc-list"], capture_output=True, text=True).stdout
    if "Ubuntu Mono" not in out:
        sys.exit("Ubuntu Mono not installed - see README.md (fonts section)")

def render_frames(frames, framedir):
    os.makedirs(framedir, exist_ok=True)
    total = len(frames)
    for c0 in range(0, total, CHUNK):
        chunk = frames[c0:c0 + CHUNK]
        page = ("<!doctype html><html><head><meta charset='utf-8'>"
                "<style>" + CSS + "</style></head><body>" +
                "".join(frame_html(s) for s, _ in chunk) + "</body></html>")
        hpath = os.path.join(framedir, "page.html")
        with open(hpath, "w") as f:
            f.write(page)
        shot = os.path.join(framedir, "page.png")
        subprocess.run(["google-chrome", "--headless", "--disable-gpu",
                        "--no-sandbox", "--hide-scrollbars",
                        "--force-device-scale-factor=1",
                        "--window-size=%d,%d" % (W, H * len(chunk)),
                        "--screenshot=" + shot, "file://" + hpath],
                       check=True, capture_output=True)
        subprocess.run(["magick", shot, "-crop", "%dx%d" % (W, H), "+repage",
                        os.path.join(framedir, "tmp_%d.png")], check=True)
        for i in range(len(chunk)):
            os.replace(os.path.join(framedir, "tmp_%d.png" % i),
                       os.path.join(framedir, "f_%03d.png" % (c0 + i)))
        print("chunk %d/%d" % (c0 // CHUNK + 1, (total + CHUNK - 1) // CHUNK),
              flush=True)
    for p in (os.path.join(framedir, "page.html"), os.path.join(framedir, "page.png")):
        if os.path.exists(p):
            os.remove(p)

def assemble(frames, framedir, gif_path):
    args = ["magick", "-loop", "0"]
    for i, (_, delay) in enumerate(frames):
        args += ["-delay", str(delay), os.path.join(framedir, "f_%03d.png" % i)]
    # keep a .gif extension — ImageMagick infers the format from it
    full = os.path.join(os.path.dirname(gif_path),
                        "_full_" + os.path.basename(gif_path))
    subprocess.run(args + [full], check=True)
    # fuzz: treat near-identical antialiased pixels as unchanged — roughly
    # halves the file size with no visible quality loss at 4%
    subprocess.run(["magick", full, "-fuzz", "4%", "-layers", "OptimizePlus",
                    gif_path], check=True)
    os.remove(full)

def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "all"
    check_fonts()
    frames, landmarks = build_frames()
    total_s = sum(d for _, d in frames) / 100.0
    print("frames: %d, duration: %.1f s" % (len(frames), total_s))
    if mode == "test":
        sel = [frames[min(i, len(frames) - 1)]
               for i in sorted(set(landmarks.values()))]
        names = dict(zip(sorted(set(landmarks.values())), sorted(landmarks,
                     key=landmarks.get)))
        print("landmarks:", {v: k for k, v in landmarks.items()})
        render_frames(sel, os.path.join(OUT, "testframes"))
        return
    framedir = os.path.join(OUT, "frames")
    if os.path.isdir(framedir):
        shutil.rmtree(framedir)
    render_frames(frames, framedir)
    gif = os.path.join(OUT, GIF_NAME)
    assemble(frames, framedir, gif)
    print("wrote %s (%.1f MB)" % (gif, os.path.getsize(gif) / 1e6))

if __name__ == "__main__":
    main()
