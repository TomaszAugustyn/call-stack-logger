#!/usr/bin/env python3
"""Regenerate the README demo GIF for call-stack-logger.

Renders VS Code look-alike frames as HTML, screenshots them with headless
Chrome, and assembles an animated GIF with ImageMagick. All pane content is
real: the editor shows the repo's current src/main.cpp, the terminal shows
captured cmake/make output, and the trace.out views show captured trace
files (see inputs/ and capture_inputs.sh).

Storyline:
  1. main.cpp opens WITHOUT class A and WITHOUT the fibonacci function.
     Class A and both A::foo() calls (inside B::foo() and in main()) are
     typed in live, quickly.
  2. Terminal (Starship-style prompt): cmake .. && make run — real captured
     output of the fibonacci-less intermediate build cascades.
  3. trace.out tab: the call tree of that intermediate build (short view).
  4. Back in the editor the fibonacci function and its fibonacci(6); call
     are typed in; the result is byte-identical to the real src/main.cpp,
     so the main.cpp:NN line numbers in the final trace stay truthful.
  5. Terminal: cmake -DLOG_ELAPSED=ON .. && make run is typed; before
     Enter a purple frame glow-pulses three times around -DLOG_ELAPSED=ON.
  6. trace.out tab: the full tree with the duration column; ~2 s in, a
     purple rectangle glow-pulses three times around the column, then the
     GIF loops.

Usage:
  python3 render.py         # full render -> out/call-stack-logger-capture-new.gif
  python3 render.py test    # render landmark frames only -> out/testframes/

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
TERM_MAX_ROWS = 11       # fewer than fit: leaves breathing room at the bottom
CHUNK = 8                # frames per Chrome screenshot page
ED_ADV = 7.5             # editor char advance (15px Ubuntu Mono)
ED_TOP = 46              # editor area top y
ROW17 = 17               # editor line height (main.cpp view)
ROW16 = 16.25            # editor line height (trace.out view, 40 lines fit)

# ---------------------------------------------------------------- palette
# Slightly darker than stock VS Code Dark+ (user preference).
BG = "#161616"           # editor / terminal / active tab background
BG_TABBAR = "#1d1d1e"
BG_TAB = "#242425"       # inactive tab

# Starship prompt segments — colors and glyphs from the user's starship.toml
# (Pastel Powerline palette + gruvbox-rainbow structure).
SEG_FG = "#fbf1c7"        # color_fg0 — cream text on every segment
SEG_OS_BG = "#9A348E"     # color_purple1 — os + username
SEG_DIR_BG = "#33658A"    # color_dark_blue — directory
SEG_GIT_BG = "#FCA17D"    # color_bright_orange2 — git branch/status
SEG_GIT_FG = "#3c3836"    # dark text on the light peach segment (readability)
SEG_LANG_BG = "#458588"   # color_blue — language modules (empty, arrow only)
SEG_BG3 = "#665c54"       # color_bg3 — docker/conda (empty, arrow only)
SEG_TIME_BG = "#3c3836"   # color_bg1 — time
CHAR_GREEN = "#98971a"    # color_green — the ❯ prompt character

CSS = """
* { margin:0; padding:0; box-sizing:border-box; }
body { background:""" + BG + """; }
.frame { position:relative; width:1200px; height:912px; overflow:hidden;
         background:""" + BG + """; font-family:'Ubuntu Mono','DejaVu Sans Mono',monospace; }
.tabbar { position:absolute; left:0; top:0; width:100%; height:27px;
          background:""" + BG_TABBAR + """; display:flex; }
.tab { height:27px; display:flex; align-items:center; justify-content:center;
       background:""" + BG_TAB + """; color:#969696; font:12px 'Noto Sans',sans-serif;
       margin-right:1px; gap:6px; }
.tab.active { background:""" + BG + """; color:#e7e7e7; }
.icpp { color:#519aba; font:bold 10px 'Noto Sans',sans-serif; }
.iout { color:#8a9199; font-size:11px; }
.close { color:#c5c5c5; font-size:12px; margin-left:2px; }
.acticons { position:absolute; right:10px; top:0; height:27px; color:#cccccc;
            opacity:.7; font-size:12px; display:flex; gap:12px; align-items:center; }
.breadcrumb { position:absolute; top:27px; left:0; width:100%; height:19px;
              color:#a0a0a0; font:11px 'Noto Sans',sans-serif; display:flex;
              align-items:center; padding-left:18px; gap:6px; background:""" + BG + """; }
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
.parrow { color:""" + CHAR_GREEN + """; font-weight:bold; }
.sg { display:inline-block; height:14px; line-height:14px; padding:0 7px;
      font-size:12.5px; vertical-align:top; }
.nf { font-family:'FiraMono Nerd Font','Ubuntu Mono',monospace; font-size:11.5px; }
.tri { display:inline-block; width:0; height:0; border-top:7px solid transparent;
       border-bottom:7px solid transparent; border-left:7px solid transparent;
       vertical-align:top; }
.tcur { display:inline-block; width:7px; height:12px; background:#cccccc;
        vertical-align:-2px; }
.mouse { position:absolute; z-index:99; }
.durrect { position:absolute; border:3px solid; border-radius:4px; z-index:50;
           box-shadow:0 0 9px 2px rgba(177,94,222,.5); }
.cmdrect { position:absolute; border:3px solid #B15EDE; border-radius:4px;
           z-index:60; box-shadow:0 0 8px 1px rgba(177,94,222,.55); }
"""

ARROW_SVG = ("data:image/svg+xml;utf8,"
    "<svg xmlns='http://www.w3.org/2000/svg' width='12' height='19'>"
    "<path d='M1 1 L1 15 L4.6 11.6 L7.2 17.4 L9.4 16.4 L6.8 10.7 L11.6 10.7 Z' "
    "fill='white' stroke='black' stroke-width='1'/></svg>")
IBEAM_SVG = ("data:image/svg+xml;utf8,"
    "<svg xmlns='http://www.w3.org/2000/svg' width='9' height='17'>"
    "<path d='M1.5 1.5 H7.5 M4.5 1.5 V15.5 M1.5 15.5 H7.5' stroke='%23dcdcdc' "
    "stroke-width='1.5' fill='none'/></svg>")

# Purple frames (glow highlights).
RECT_COLOR = "#B15EDE"
RECT_BORDER = 3          # px; duration-column frame
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

# STRUCTURE ASSERTS — the typing scenes insert class A (lines 13-17 incl. the
# leading blank), the A::foo() call in B::foo() (line 26), the A::foo() call
# in main() (lines 51-52 incl. comment), the fibonacci function (lines 29-34
# incl. the leading blank) and its call (lines 64-65 incl. comment). If
# main.cpp is restructured, update these constants, the typing scenes in
# build_frames(), and the line deletions in capture_inputs.sh.
A_BLANK, A_FIRST, A_LAST = 13, 14, 17
A_CALL_B = 26
A_CALL_COMMENT, A_CALL_MAIN = 51, 52
FIB_BLANK, FIB_FIRST, FIB_LAST = 29, 30, 34
CALL_COMMENT, CALL_LINE = 64, 65
assert MAIN_CPP[A_BLANK - 1] == "" and MAIN_CPP[A_FIRST - 1] == "class A {", \
    "class A moved — update A_* constants"
assert MAIN_CPP[A_LAST - 1] == "};", "class A closing moved"
assert MAIN_CPP[A_CALL_B - 1].strip() == "A::foo();", "A::foo() call in B::foo() moved"
assert MAIN_CPP[A_CALL_COMMENT - 1].lstrip().startswith("// Test logging static"), \
    "A::foo() call comment in main() moved"
assert MAIN_CPP[A_CALL_MAIN - 1].strip() == "A::foo();", "A::foo() call in main() moved"
assert MAIN_CPP[FIB_BLANK - 1] == "", "expected blank line above fibonacci"
assert MAIN_CPP[FIB_FIRST - 1].startswith("constexpr unsigned fibonacci"), \
    "fibonacci signature moved — update FIB_* constants"
assert MAIN_CPP[FIB_LAST - 1] == "}", "fibonacci closing brace moved"
assert MAIN_CPP[CALL_COMMENT - 1].lstrip().startswith("// Test logging constexpr"), \
    "fibonacci call comment moved — update CALL_* constants"
assert MAIN_CPP[CALL_LINE - 1].strip() == "fibonacci(6);", "fibonacci call moved"

# The file state after the class-A act but before the fibonacci act — must
# match what capture_inputs.sh built for act1/act2.
INTERMEDIATE = list(MAIN_CPP)
del INTERMEDIATE[CALL_COMMENT - 1:CALL_LINE]
del INTERMEDIATE[FIB_BLANK - 1:FIB_LAST]

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

def parse_clock(trace_lines):
    """HH:MM from the trace run separator — feeds the prompt's time segment."""
    m = re.search(r"(\d{2}:\d{2}):\d{2}", trace_lines[2])
    return m.group(1) if m else "12:00"

CLOCK1 = parse_clock(ACT2_TRACE)
CLOCK2 = parse_clock(ACT4_TRACE)

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

def prompt_block(modified, clock, typed="", cursor=False):
    """Starship-style prompt: blank line, segment bar, a spacer line (extra
    readability so the glow frame never overlaps the bar), then the ❯ line."""
    spans = [("parrow", "❯"), (None, " " + typed)]
    if cursor:
        spans.append(("CURSOR", ""))
    return ([[(None, "")], [("BAR", (modified, clock))], [(None, "")]]
            + wrap_row(spans))

def bar_html(modified, clock):
    tri = ('<span class="tri" style="border-left-color:%s;background:%s"></span>')
    seg = ('<span class="sg" style="background:%s;color:' + SEG_FG +
           '%s">%s</span>')
    git_status = " !" if modified else ""
    parts = [
        # os + username segment with the rounded left cap
        seg % (SEG_OS_BG, ";border-radius:7px 0 0 7px",
               '<span class="nf">&#xf08db;</span> fedora'),
        tri % (SEG_OS_BG, SEG_DIR_BG),
        seg % (SEG_DIR_BG, "", "&#8230;/call-stack-logger/build"),
        tri % (SEG_DIR_BG, SEG_GIT_BG),
        seg % (SEG_GIT_BG, ";color:" + SEG_GIT_FG,
               '<span class="nf">&#xf418;</span> master' + git_status),
        # empty language / docker segments leave their arrow cascade behind
        tri % (SEG_GIT_BG, SEG_LANG_BG),
        tri % (SEG_LANG_BG, SEG_BG3),
        tri % (SEG_BG3, SEG_TIME_BG),
        seg % (SEG_TIME_BG, ";border-radius:0 7px 7px 0",
               '<span class="nf">&#xf017;</span> ' + clock),
    ]
    return "".join(parts)

CMD1 = "cmake .. && make run"
CMD2 = "cmake -DLOG_ELAPSED=ON .. && make run"
OUT1 = plain_rows(ACT1_CMAKE + ACT1_MAKERUN)
OUT2 = plain_rows(ACT3_CMAKE + ACT3_MAKERUN)

# Geometry of the glow frame around "-DLOG_ELAPSED=ON" in the typed command.
# The input line is "<arrow><space><command>", so 2 prefix characters.
CMD2_PARAM = "-DLOG_ELAPSED=ON"
TERM_X0, TERM_ADV, TERM_TOP, TERM_ROW = 16, 7, 728, 14
CMDRECT_X = TERM_X0 + (2 + CMD2.index(CMD2_PARAM)) * TERM_ADV - 4
CMDRECT_W = len(CMD2_PARAM) * TERM_ADV + 8
CMDRECT_Y = TERM_TOP + (TERM_MAX_ROWS - 1) * TERM_ROW - 3
CMDRECT_H = TERM_ROW + 6

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
            x = caret[1] * ED_ADV
            extra += '<span class="caret" style="left:%.1fpx"></span>' % x
            if blame:
                extra += ('<span class="blame">%s%s</span>'
                          % ("&nbsp;" * 8, esc(blame)))
        rows.append('<div class="%s"><span class="ln">%d</span>'
                    '<span class="lc">%s%s</span></div>' % (cls, n, content, extra))
    return '<div class="editor">%s</div>' % "".join(rows)

def editor_trace(lines, rect=False):
    n_real = len(lines)
    disp = lines + [""] * max(0, 40 - n_real)
    rows = []
    for i, text in enumerate(disp[:40]):
        num = str(i + 1) if i < n_real + 1 else ""
        rows.append('<div class="eline16"><span class="ln">%s</span>'
                    '<span class="lc">%s</span></div>' % (num, esc(text)))
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

def terminal(rows, max_rows=TERM_MAX_ROWS):
    out = []
    for row in rows[-max_rows:]:
        if row and row[0][0] == "BAR":
            modified, clock = row[0][1]
            out.append('<div class="trow">%s</div>' % bar_html(modified, clock))
            continue
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

def click_xy(first, line, col):
    return (round(60 + col * ED_ADV + 4), line_y(first, line))

def build_frames():
    frames, landmarks = [], {}

    def F(delay, **state):
        frames.append((state, delay))

    def mark(name):
        landmarks[name] = len(frames)

    # Editable document: real main.cpp minus class A (with its blank line),
    # minus both A::foo() calls, minus the fibonacci function and its call.
    # The typing scenes re-insert everything; intermediate and final states
    # are asserted against INTERMEDIATE and MAIN_CPP.
    doc = list(MAIN_CPP)
    del doc[CALL_COMMENT - 1:CALL_LINE]
    del doc[A_CALL_COMMENT - 1:A_CALL_MAIN]
    del doc[FIB_BLANK - 1:FIB_LAST]
    del doc[A_CALL_B - 1:A_CALL_B]
    del doc[A_BLANK - 1:A_LAST]

    def snap():
        return list(doc)

    t0 = prompt_block(False, CLOCK1, "", cursor=True)
    t0_off = prompt_block(False, CLOCK1, "", cursor=False)
    typing_delays = [6, 5, 7, 5, 4, 6, 5, 8, 5, 4, 6, 5, 4, 7, 5, 6, 4, 5, 6, 5]
    blame_now = "You, seconds ago • Uncommitted changes"

    caret_line, caret_col = 1, 0
    view = {"first": 10, "mpos": (520, 340), "term": t0}

    def Fmain(delay, first=None, caret=None, blame=None, term=None, mpos=None,
              mkind="ibeam", cmdrect=None):
        F(delay, view="main", doc=snap(),
          first=view["first"] if first is None else first,
          caret=caret, blame=blame,
          term=view["term"] if term is None else term,
          mouse=(view["mpos"] if mpos is None else mpos) + (mkind,),
          cmdrect=cmdrect)

    def click(line, col, moves=2, delay=5):
        nonlocal caret_line, caret_col
        target = click_xy(view["first"], line, col)
        start = view["mpos"]
        for i in range(1, moves + 1):
            Fmain(delay, mpos=lerp(start, target, i / moves))
        view["mpos"] = target
        caret_line, caret_col = line, col
        Fmain(12, caret=(caret_line, caret_col))

    def enter(indent, d=5):
        nonlocal caret_line, caret_col
        doc.insert(caret_line, " " * indent)
        caret_line += 1
        caret_col = indent
        Fmain(d, caret=(caret_line, caret_col))

    def type_chars(text, chunk, dscale=1.0):
        nonlocal caret_col
        for i in range(0, len(text), chunk):
            piece = text[i:i + chunk]
            doc[caret_line - 1] += piece
            caret_col += len(piece)
            Fmain(max(3, round(typing_delays[(i // chunk) % len(typing_delays)]
                               * dscale)),
                  caret=(caret_line, caret_col))

    # ---- opening: no class A, no fibonacci; mouse drifts in
    mark("open")
    for p, d in [((520, 340), 60), ((330, 200), 30)]:
        Fmain(d, mpos=p)

    # ---- act A: type class A, then A::foo() in B::foo() and in main()
    mark("type_A")
    click(12, len(doc[11]))                      # end of "#include <vector>"
    enter(0, 5); enter(0, 5)
    type_chars("class A {", 4, 0.8)
    enter(0); type_chars("public:", 4, 0.8)
    enter(4); type_chars('static void foo() { std::cout << "static foo \\n"; }',
                         4, 0.8)
    enter(0); type_chars("};", 2, 0.8)
    Fmain(28, caret=(caret_line, caret_col), blame=blame_now)
    click(25, len(MAIN_CPP[A_CALL_B - 2]))       # end of the std::sort(...) line
    enter(8); type_chars("A::foo();", 4, 0.8)
    Fmain(22, caret=(caret_line, caret_col), blame=blame_now)
    # scroll down until the whole main() body is on screen, then edit it
    for f in (13, 16, 19, 22, 25):
        Fmain(6, first=f)
    view["first"] = 27
    Fmain(16)
    click(44, len("int main() {"))               # main() { in the fib-less file
    enter(4); type_chars("// Test logging static member methods.", 4, 0.8)
    enter(4); type_chars("A::foo();", 4, 0.8)
    mark("a_done")
    Fmain(35, caret=(caret_line, caret_col), blame=blame_now)
    Fmain(25, caret=(caret_line, caret_col), blame=blame_now, term=t0_off)

    assert doc == INTERMEDIATE, \
        "typed class-A document does not match the intermediate main.cpp"

    # ---- act 1: cmake .. && make run (intermediate build)
    tpos = (430, 806)
    for i in range(1, 4):
        Fmain(5, mpos=lerp(view["mpos"], tpos, i / 3))
    view["mpos"] = tpos
    for i, _ in enumerate(CMD1):
        Fmain(typing_delays[i % len(typing_delays)],
              term=prompt_block(False, CLOCK1, CMD1[:i + 1], cursor=True))
    Fmain(12, term=prompt_block(False, CLOCK1, CMD1))
    mark("cascade1")
    hist1 = prompt_block(False, CLOCK1, CMD1)
    steps = [(0.18, 16), (0.4, 20), (0.55, 26), (0.68, 30), (0.8, 22),
             (0.93, 16), (1.0, 10)]
    for fr, d in steps:
        n = max(1, int(len(OUT1) * fr))
        Fmain(d, term=hist1 + OUT1[:n])
    after1 = hist1 + OUT1 + prompt_block(True, CLOCK1, "", cursor=True)
    after1_off = hist1 + OUT1 + prompt_block(True, CLOCK1, "", cursor=False)
    view["term"] = after1
    Fmain(60)
    Fmain(40, term=after1_off)

    # ---- act 2: trace.out of the intermediate build (short look, ~3.5 s)
    tab_trace = (400, 14)
    for i in range(1, 5):
        Fmain(5, term=after1, mpos=lerp(tpos, tab_trace, i / 4), mkind="arrow")
    mark("trace1")
    F(25, view="trace1", term=after1, mouse=tab_trace + ("arrow",))
    F(150, view="trace1", term=after1_off, mouse=tab_trace + ("arrow",))
    drift = [(560, 150), (600, 250)]
    for p, d in zip(drift, (60, 90)):
        F(d, view="trace1", term=after1, mouse=p + ("ibeam",))

    # ---- act 3: back to main.cpp, type fibonacci + its call
    tab_main = (290, 14)
    for i in range(1, 4):
        F(5, view="trace1", term=after1,
          mouse=lerp(drift[-1], tab_main, i / 3) + ("arrow",))
    view["mpos"] = tab_main
    Fmain(25, term=after1, mpos=tab_main, mkind="arrow")
    mark("type_fib")
    click(28, len(doc[27]), moves=3)             # end of class B's "};"
    enter(0); enter(0)
    type_chars("constexpr unsigned fibonacci(unsigned n) {", 2)
    enter(4); type_chars("if (n <= 1)", 3)
    enter(8); type_chars("return n;", 3)
    enter(4); type_chars("return fibonacci(n - 1) + fibonacci(n - 2);", 3)
    enter(0); type_chars("}", 1)
    mark("fib_done")
    Fmain(45, caret=(caret_line, caret_col), blame=blame_now, term=after1)
    Fmain(35, caret=(caret_line, caret_col), blame=blame_now, term=after1_off)

    # ---- scroll calmly down to main() (3 lines per step, like a mouse wheel)
    mark("scroll")
    for f in (30, 33):
        Fmain(6, first=f, term=after1)
    view["first"] = 35
    Fmain(16, term=after1)
    mark("type_call")
    click(63, len(MAIN_CPP[CALL_COMMENT - 2]), moves=3)  # end of "b.foo();"
    enter(4); type_chars("// Test logging constexpr function", 3)
    enter(4); type_chars("fibonacci(6);", 2)
    mark("call_done")
    Fmain(45, caret=(caret_line, caret_col), blame=blame_now, term=after1)
    Fmain(30, caret=(caret_line, caret_col), blame=blame_now, term=after1_off)

    assert doc == MAIN_CPP, "typed document does not match real src/main.cpp"

    # ---- act 4: cmake -DLOG_ELAPSED=ON .. && make run, with the glow frame
    base1 = hist1 + OUT1
    for i in range(1, 4):
        Fmain(5, term=after1, mpos=lerp(view["mpos"], tpos, i / 3))
    view["mpos"] = tpos
    for i, _ in enumerate(CMD2):
        Fmain(max(3, typing_delays[(i * 3) % len(typing_delays)] - 1),
              term=base1 + prompt_block(True, CLOCK1, CMD2[:i + 1], cursor=True))
    mark("cmd2_glow")
    cmd_on = base1 + prompt_block(True, CLOCK1, CMD2, cursor=True)
    cmd_off = base1 + prompt_block(True, CLOCK1, CMD2, cursor=False)
    for _ in range(3):
        for op, d in ((0.3, 8), (0.65, 8), (1.0, 50)):
            Fmain(d, term=cmd_on, cmdrect=op)
        for op, d in ((0.6, 8), (0.25, 8)):
            Fmain(d, term=cmd_off, cmdrect=op)
        Fmain(8, term=cmd_off)
    Fmain(12, term=cmd_off)
    hist2 = base1 + prompt_block(True, CLOCK1, CMD2)
    for fr, d in steps:
        n = max(1, int(len(OUT2) * fr))
        Fmain(d, term=hist2 + OUT2[:n])
    after2 = hist2 + OUT2 + prompt_block(True, CLOCK2, "", cursor=True)
    after2_off = hist2 + OUT2 + prompt_block(True, CLOCK2, "", cursor=False)
    Fmain(55, term=after2)

    # ---- act 5: trace.out with durations, pulse the column frame
    for i in range(1, 5):
        Fmain(5, term=after2, mpos=lerp(tpos, tab_trace, i / 4), mkind="arrow")
    mark("trace2")
    F(30, view="trace2", term=after2, mouse=tab_trace + ("arrow",))
    F(80, view="trace2", term=after2_off, mouse=tab_trace + ("arrow",))
    col = [(330, 200), (330, 380), (330, 530)]
    for p, d in zip(col, (25, 25, 30)):
        F(d, view="trace2", term=after2, mouse=p + ("ibeam",))
    aside = (386, 400)
    F(6, view="trace2", term=after2, mouse=(360, 460, "arrow"))
    F(10, view="trace2", term=after2, mouse=aside + ("arrow",))
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
    for fam in ("Ubuntu Mono", "FiraMono Nerd Font"):
        if fam not in out:
            sys.exit("%s not installed — see README.md (fonts section)" % fam)

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
    # fuzz: treat near-identical antialiased pixels as unchanged — cuts the
    # file size to a fraction with no visible quality loss (checked at 6%)
    subprocess.run(["magick", full, "-fuzz", "6%", "-layers", "OptimizePlus",
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
