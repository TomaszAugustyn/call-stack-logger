# README demo GIF generator

Regenerates `misc/call-stack-logger-capture.gif` from the **current codebase**
without any screen recording. Frames are VS Code Dark+ look-alikes rendered as
HTML, screenshotted with headless Chrome, and assembled into a GIF with
ImageMagick. Everything inside the panes is real: the editor shows the repo's
`src/main.cpp`, the terminal shows captured `cmake`/`make run` output, and the
trace.out views show real trace files produced by the demo.

These files are intentionally **not committed** — keep or commit them only by
explicit decision.

## Quick start

```bash
# 1. Recapture real outputs from the current codebase (rebuilds in ../../build)
./capture_inputs.sh

# 2. Render all frames and assemble the GIF (~2 min)
python3 render.py
# -> out/call-stack-logger-capture-new.gif

# Optional: render only the landmark frames for a quick visual check
python3 render.py test
# -> out/testframes/f_*.png  (one frame per storyline landmark)
```

Review the GIF, then replace the published one:

```bash
mv out/call-stack-logger-capture-new.gif ../call-stack-logger-capture.gif
```

## Prerequisites

- `google-chrome` (headless mode is used for screenshots)
- ImageMagick 7 (`magick`)
- **Ubuntu Mono** font — matches the font of the original 2021 capture:
  ```bash
  mkdir -p ~/.local/share/fonts/ubuntu-mono && cd ~/.local/share/fonts/ubuntu-mono
  curl -fsSLO https://raw.githubusercontent.com/google/fonts/main/ufl/ubuntumono/UbuntuMono-Regular.ttf
  curl -fsSLO https://raw.githubusercontent.com/google/fonts/main/ufl/ubuntumono/UbuntuMono-Bold.ttf
  fc-cache -f ~/.local/share/fonts/ubuntu-mono
  ```
- Noto Sans (UI labels; present by default on Fedora)
- A Nerd Font for the Starship prompt glyphs (Fedora logo, git branch,
  clock) — the renderer uses **FiraMono Nerd Font**
  (https://www.nerdfonts.com/, install under `~/.local/share/fonts/`)

## Storyline (edit `build_frames()` in render.py to change it)

The look is VS Code with a slightly darkened Dark+ palette (`BG*` constants)
and a Starship terminal prompt replicated from the author's `starship.toml`
(`SEG_*` constants — Pastel Powerline palette: purple os/user segment,
dark-blue path, peach git branch with a modified-flag, arrow cascade, time
segment with a rounded cap; clock parsed from the trace files; `❯` prompt
character). A spacer line sits between the prompt bar and the `❯` input line
so the glow frame never overlaps the bar. The terminal shows at most
`TERM_MAX_ROWS` (11) rows so the bottom edge keeps breathing room.

1. `main.cpp` opens **without** class A and **without** fibonacci. Class A
   and the `A::foo()` call in `B::foo()` are typed in live, quickly; the view
   then scrolls down until the whole `main()` body is visible and the
   `A::foo()` call is typed there too.
2. Terminal: `cmake .. && make run` — real output of the fibonacci-less
   intermediate build cascades.
3. `trace.out` tab: the intermediate build's call tree (short view, ~3.5 s).
4. Back in the editor, fibonacci is typed in, the view scrolls down (3 lines
   per frame, like a mouse wheel) and `fibonacci(6);` is typed with its
   comment in `main()`. The result is byte-identical to the real
   `src/main.cpp`, so `main.cpp:NN` line numbers in the final trace stay
   truthful.
5. Terminal: `cmake -DLOG_ELAPSED=ON .. && make run` is typed; before Enter,
   a purple frame glow-pulses three times around `-DLOG_ELAPSED=ON`.
6. `trace.out` tab: the full tree with the duration column; ~2 s after the
   view appears, a purple rectangle (`RECT_COLOR` / `RECT_BORDER`) glow-pulses
   three times around the column, then the GIF loops.

## Inputs (`inputs/`, produced by `capture_inputs.sh`)

| File | Content |
|------|---------|
| `act1-cmake.txt`   | `cmake ..` configure output (default) |
| `act1-makerun.txt` | full-rebuild `make run` output incl. demo stdout |
| `act2-trace.txt`   | trace.out of the INTERMEDIATE build (no fibonacci) |
| `act3-cmake.txt`   | `cmake -DLOG_ELAPSED=ON ..` configure output |
| `act3-makerun.txt` | LOG_ELAPSED rebuild + run output (full file) |
| `act4-trace.txt`   | trace.out with patched duration fields (full file) |

act1/act2 are captured from an intermediate `src/main.cpp` with the
fibonacci function and its call removed — in the story, the first build
happens before fibonacci exists. `capture_inputs.sh` swaps the file in
temporarily and restores it afterwards (also on interrupt).

## Things that need updating when the code changes

- **`src/main.cpp` restructured** → the typing scene inserts the fibonacci
  function at lines 29–34 and the call at 64–65. Structure asserts at the top
  of render.py fail loudly with the constant names to update (`FIB_*`,
  `CALL_*`). The purple-rectangle row range (`DUR_ROW_FIRST/LAST`) equals the
  trace.out line span of the trace entries; it is 5–39 for the current demo.
- **Trace format changed** → rerun `capture_inputs.sh`; if the timestamp or
  duration field widths changed, update `DUR_CHAR_START/END` (duration column
  = chars 26–38 of a trace line: after `"[DD-MM-YYYY HH:MM:SS.mmm] "`).

## How the original GIF was analyzed (recipe for matching any example GIF)

Useful when calibrating against a reference capture:

```bash
# frame count, dimensions, per-frame delays (units: 1/100 s)
identify -format "frame %s: %T cs (%wx%h)\n" example.gif | head

# extract frames as PNGs to look at
magick example.gif -coalesce frames/f_%03d.png

# measure text advance: crop one text line, trim background, divide width
# by the number of characters -> px/char -> font size (Ubuntu Mono: size/2)
magick frames/f_100.png -crop 1200x14+0+275 +repage -fuzz 25% -trim info:

# sample colors of a region (GIF dithering: read the bright outliers)
magick frames/f_100.png -crop 60x14+60+207 +repage -format %c histogram:info:- | sort -rn | head
```

Layout constants derived from the original 1200x912 capture: tab bar 27 px,
breadcrumb 19 px, editor top 46 px / height 649 px (38 lines x 17 px; trace
view 40 lines x 16.25 px), panel header at 695 px, terminal text from
(16, 728), 14 px rows, 13 visible. Editor font 15 px Ubuntu Mono (7.5 px
advance), terminal 14 px (7 px advance). Colors: stock VS Code Dark+ theme and
default integrated-terminal ANSI palette (see CSS in render.py).

## Output format facts

1200x912 (same as the original), ~290 frames / ~32 s, infinite loop (NETSCAPE
extension), per-frame delays in centiseconds. Size (~6 MB) comes from
`-fuzz 6% -layers OptimizePlus`; plain palette reduction (`-colors`) does NOT
help — the cost is antialiased-text diff area, not color count. GitHub renders
README GIFs up to ~10 MB.

`inputs/` and `out/` are machine-generated (see .gitignore here) — if this
directory is ever committed, only render.py, capture_inputs.sh, README.md and
.gitignore belong in version control.
