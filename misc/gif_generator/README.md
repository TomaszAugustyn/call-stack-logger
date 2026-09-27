# README demo GIF generator

Regenerates `misc/call-stack-logger-capture-new.gif` (the GIF README embeds)
from the **current codebase** without any screen recording. Frames are VS Code
look-alikes rendered as HTML, screenshotted with headless Chrome, and encoded
into a GIF with ffmpeg. Everything inside the panes is real: the editor shows
the repo's `src/main.cpp` and the versions of it the story types, the terminal
shows captured `cmake`/`make run` output, and the trace.out views show real
trace files produced by the demo.

## Quick start

```bash
# 1. Recapture real outputs from the current codebase (rebuilds in ../../build)
./capture_inputs.sh

# 2. Render all frames and encode the GIF (~1 min)
python3 render.py
# -> out/call-stack-logger-capture-new.gif

# Optional: render only the landmark frames for a quick visual check
python3 render.py test
# -> out/testframes/f_*.png  (one frame per storyline landmark)
```

Review the GIF, then replace the published one:

```bash
cp out/call-stack-logger-capture-new.gif ../call-stack-logger-capture-new.gif
```

## Prerequisites

- `google-chrome` (headless mode renders the frames, several in parallel)
- `ffmpeg` (encodes the GIF) and ImageMagick 7 (`magick`, `identify`)
- `prlimit` (util-linux) and `stdbuf` (coreutils), used by `capture_inputs.sh`
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
character, red after a command that failed). A spacer line sits between the
prompt bar and the `❯` input line so the glow frames never overlap the bar.
The terminal shows at most `TERM_MAX_ROWS` (11) rows so the bottom edge keeps
breathing room.

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
   `src/main.cpp`.
5. Terminal: `cmake -DLOG_ELAPSED=ON .. && make run` is typed; before Enter,
   a purple frame glow-pulses three times around `-DLOG_ELAPSED=ON`.
6. `trace.out` tab: the full tree with the duration column; ~2 s after the
   view appears, a purple rectangle (`RECT_COLOR` / `RECT_BORDER`) glow-pulses
   three times around the column.
7. Back in the editor, the exception demo is typed above `main()`: a throw
   that leaves two frames (a destructor calls a traced helper on the way out)
   and is caught one level up, and a throw nobody catches (`EXC_FUNCS` in
   `demo_source.py`, names and shape as in README's "Exceptions in the trace
   tree" example). After `exc_mid()` a quick scroll (4 lines per frame)
   brings the rest of the typing to the middle of the screen. Then the view
   scrolls to `main()`, `fibonacci(6);` is commented out (a shorter trace),
   and calls to both are typed right below it (`EXC_CALLS`), before `print()`
   and `cube()`, which then never run: their missing trace lines show that
   the program really ended in `std::terminate`.
8. Terminal: `cmake -DLOG_EXCEPTIONS=ON -DLOG_ELAPSED=ON .. && make run`; a
   green (`GLOW_GREEN`) frame glow-pulses three times around the two options.
   The demo ends in `std::terminate`, so the real output ends in the
   terminate message and make's errors, and the next `❯` is red.
9. `trace.out` tab: the tree with the exception events. ~2 s in, the marks of
   the caught exception glow green three times (`!`-flagged durations, `!_`
   glyphs, `[  throw   ]`/`!! throw`, `[  catch   ]`/`!! catch`), then those of
   the uncaught one (the `[  pending ]` fields of `main` and the two frames,
   `[  throw   ]`/`!! throw`, `[ terminate]`/`!! terminate`), then the GIF
   loops. `special_spans()` in render.py decides what glows.

## The versions of main.cpp (`demo_source.py`)

The story types parts of the demo in live, so each build must see the file as
it looks at that moment. `demo_source.py` derives every version from the real
`src/main.cpp`: `initial` (the GIF's first frame), `intermediate` (first
build, no fibonacci yet), `full` (the real file) and `exceptions` (the real
file plus the exception demo). It finds its anchor lines by **content**
(`class A {`, `constexpr unsigned fibonacci`, `// Test logging constexpr`,
`cube(3);`, ...), so an edit elsewhere in `main.cpp` only shifts line numbers.
render.py types from these versions and asserts that each typed result equals
the version `capture_inputs.sh` built and ran, so every `main.cpp:NN` in the
GIF is the real line for the code on screen at that moment.

`python3 demo_source.py exceptions` prints a version, handy for a look.

## Inputs (`inputs/`, produced by `capture_inputs.sh`)

| File | Content |
|------|---------|
| `act1-cmake.txt`   | `cmake ..` configure output (default) |
| `act1-makerun.txt` | full-rebuild `make run` output incl. demo stdout |
| `act2-trace.txt`   | trace.out of the intermediate build (no fibonacci) |
| `act3-cmake.txt`   | `cmake -DLOG_ELAPSED=ON ..` configure output |
| `act3-makerun.txt` | LOG_ELAPSED rebuild + run output (real file) |
| `act4-trace.txt`   | trace.out with patched duration fields (real file) |
| `act5-cmake.txt`   | `cmake -DLOG_EXCEPTIONS=ON -DLOG_ELAPSED=ON ..` configure output |
| `act5-makerun.txt` | rebuild + run of the exception demo, ending in `std::terminate` |
| `act6-trace.txt`   | trace.out with exception events and marks |

The script runs the commands in `build/` exactly as the GIF types them, each
against its version of `main.cpp` (swapped in, restored afterwards). Details
that keep the output faithful:

- `make` runs under `stdbuf -oL`: the demo's stdout is line-buffered as on a
  terminal. The act5 demo aborts, and a fully buffered stdout would lose its
  lines.
- `make` runs under `prlimit --core=1:1`: the abort writes no core dump and
  wakes no desktop crash reporter (a limit of 0 does not stop systemd-coredump).
  make therefore prints `Aborted`, not `Aborted (core dumped)`.
- Everything runs with `LC_ALL=C.UTF-8`: make translates its error messages
  into the system language, and the rest of the GIF is English.
- On exit, also after an error or Ctrl+C, it restores the real `main.cpp`,
  the `LOG_*` options cached in `build/` and any `build/trace.out`, deletes
  the demo object compiled from a swapped-in file (its timestamp is newer
  than the restored file, so make would reuse it) and rebuilds `runDemo`.

## Things that need updating when the code changes

- **`src/main.cpp` restructured** → if an anchor line is gone or the sections
  are reordered, `demo_source.py` stops with a message naming what it could
  not find; update `Demo.__init__`. Shifted line numbers need nothing.
- **Trace format changed** → rerun `capture_inputs.sh`; if the timestamp or
  duration field widths changed, update `DUR_CHAR_START/END` (duration column
  = chars 26–38 of a trace line: after `"[DD-MM-YYYY HH:MM:SS.mmm] "`), and
  check what `special_spans()` highlights.
- **A longer trace** → the trace view shows 40 lines (`ROW16` = 16 px); an
  assert stops the render if the LOG_ELAPSED trace no longer fits.

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
view 40 lines x 16 px), panel header at 695 px, terminal text from (16, 728),
14 px rows. Editor font 15 px Ubuntu Mono (7.5 px advance), terminal 14 px
(7 px advance). Colors: stock VS Code Dark+ theme and default
integrated-terminal ANSI palette (see CSS in render.py).

## Output format facts

1200x912 (same as the original), ~620 frames / ~61 s, infinite loop,
per-frame delays in centiseconds (render.py checks every delay after
encoding). Size: ~3.5 MB. The encoder uses ONE 256-color palette for the
whole animation (`palettegen`) and no dithering (`paletteuse=dither=none`),
so a pixel that does not change keeps its exact color index and each frame
stores only the rectangle that changed. Converting each frame separately
with dithering (a plain ImageMagick conversion) made the same GIF 13 MB,
because the dither noise differs from frame to frame. The measured quality
against the rendered frames is 40–47 dB PSNR, with no visible banding in the
glows.

GitHub renders a GIF committed to the repository and referenced by a relative
path at any size; the 10 MB limit applies to images dragged into issues and
comments. A small file still matters: every visitor downloads it, and every
committed version stays in the history.

`inputs/` and `out/` are machine-generated (see .gitignore here); only the
scripts, this README and the .gitignore belong in version control.
