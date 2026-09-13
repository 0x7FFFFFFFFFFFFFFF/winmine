# Minesweeper

A C++/Win32 Minesweeper: three preset levels plus a custom field, question
marks, chording, a clock, best times, sound, a monochrome mode — and the
XYZZY peek.

![Expert board part way through a game](docs/expert.png)

*Expert, 30 × 16 with 99 mines, part way through.*

![Beginner board after stepping on a mine](docs/beginner.png)

*Beginner, after stepping on a mine: the one you hit turns red, the rest are
revealed, and a flag in the wrong place gets a red X.*

```bash
build.cmd
```

produces `winmine.exe` next to `build.cmd`.

---

## Layout

| path | contents |
|---|---|
| `build.cmd` | builds `winmine.exe` |
| `src/miner.cpp` | the whole game, one translation unit |
| `src/miner.rc`, `src/resource.h` | menu, dialogs, strings, accelerators, version |
| `src/miner.manifest` | side-by-side manifest for the v6 common controls |
| `res/*.bmp`, `res/miner.ico`, `res/*.wav` | artwork and sounds |
| `build/` | intermediate object/resource files |
| `docs/` | the screenshots above |
| `LICENSE` | WTFPL |

## Settings and scores

Everything the game remembers — level, custom field size, window position,
Marks/Sound/Colour, and the three best times with their names — is kept in
**`winmine.ini`, in the same folder as `winmine.exe`**. Nothing is written to
the registry. The file is created on first run, as UTF-16 with a byte-order
mark so that non-ASCII player names survive a round trip:

```ini
[Minesweeper]
Difficulty=1
Height=16
Width=16
Mines=40
Mark=1
Color=1
Sound=0
Xpos=80
Ypos=80
Time1=999
Name1=Anonymous
...
```

`Difficulty` is 0–3 (beginner, intermediate, expert, custom). `Time1..3` are
the best times in seconds for the three preset levels.

## How it is drawn

Three pairs of bitmaps live in the resource file — a 4bpp colour version and a
1bpp monochrome one, chosen by the **Colour** menu item:

| id | size | contents |
|---|---|---|
| 410 / 411 | 16 × 256 | 16 field tiles of 16 × 16 |
| 420 / 421 | 13 × 276 | 12 LED digits of 13 × 23 |
| 430 / 431 | 24 × 120 | 5 smileys of 24 × 24 |

They are bottom-up DIBs, so tile 0 is the *last* 16 rows of the strip. The
game blits with `SetDIBitsToDevice` at `header + palette + index *
bytesPerTile`, and pre-renders the 16 field tiles into memory DCs at start-up
so the common case is a plain `BitBlt`.

The client area is `width*16 + 24` by `height*16 + 67`. Cell *(x, y)* is
blitted at `(x*16 - 4, y*16 + 39)`, and a click at client *(px, py)* maps back
to `((px + 4) >> 4, (py - 0x27) >> 4)`. The 3D edges all come from one routine
that walks `cThick` nested rectangles, using `R2_WHITE` for the highlight and a
grey (black, in mono) pen with `R2_COPYPEN` for the shadow.

## Board encoding

One byte per cell in a fixed 864-byte array with a stride of 32 bytes — which
is what caps the width at 30 — surrounded by a ring of `0x10` sentinels so the
neighbour loops never need edge tests:

```
0x80  a mine is here
0x40  the cell has been uncovered
0x1F  tile index: 0-8 = the digits, 9 = '?' down, 10 = mine,
      11 = wrong flag, 12 = exploded, 13 = '?', 14 = flag,
      15 = untouched, 16 = off-board
```

## Rules worth spelling out

* the first cell uncovered is never a mine — if it is, the mine is relocated to
  the first free square found scanning rows/columns `1 .. n-1`;
* uncovering is a flood fill through a 100-entry ring buffer, not recursion;
* chording (both buttons, middle button, or shift + left) only fires when the
  flag count around the cell equals its number, and pressing in shows the whole
  3×3 block;
* right-click cycles blank → flag → `?` → blank, and skips `?` when **Marks**
  is off; only the flag transitions move the mine counter;
* placing the last flag when every safe cell is already open wins the game;
* on a win the remaining mines become flags and the counter is driven to 0; on
  a loss the mines are revealed and wrong flags get a red X;
* the clock starts on the first button release over the field and stops at 999;
* minimising pauses the clock and restores it on the way back.

F1 opens help, F2 starts a new game.

## The XYZZY peek

A hidden aid, off unless you go looking for it:

1. type `X`, `Y`, `Z`, `Z`, `Y` into the game window — a counter advances one
   step per matching key and resets on any other key;
2. press **Shift** — the counter (now 5) is XORed with `0x14`, becoming 17,
   which *arms* it. Pressing Shift again flips it back and disarms it. (While
   it sits at 5, holding **Ctrl** arms it for as long as Ctrl is down.)

Once armed, every mouse move over the field pokes a single pixel in the very
**top-left corner of the screen** — `SetPixel(GetDC(NULL), 0, 0, …)`, i.e.
straight onto the desktop, outside the game window: **black** when the cell
under the cursor hides a mine, **white** when it does not. Nothing else on
screen changes, which is what makes it discreet.

Because the pixel is painted directly on the screen, it is erased again by the
next thing that repaints that corner.

## Building

`build.cmd` holds `winmine.exe` to a **119,808 byte budget** and fails the
build if it goes over. Nearly all of that is artwork and sound, so the code has
to fit in what is left. Two things make that work:

* the program links **without the C run-time** — `MinerEntry()` in
  `miner.cpp` is the raw PE entry point, and the only two library routines it
  needs (`memset`, `rand`) are defined in `miner.cpp` itself;
* it is built **32-bit with a fixed image base**, so there is no `.reloc`
  section.

The result is about 118.8 KB, roughly 100 KB of which is the resource section.

`build.cmd` picks a toolchain in this order:

1. **32-bit MSVC**, located through `vswhere` — this is the combination that
   meets the budget;
2. `cl.exe` already on `PATH`;
3. **MinGW-w64** — builds and runs fine, but a 64-bit build cannot fit in the
   budget (64-bit code is simply bigger), so `build.cmd` reports that and
   fails. Use 32-bit MSVC or a 32-bit MinGW-w64.

Because the image has a fixed base it is not ASLR-randomised. If you would
rather keep ASLR than the size budget, drop `/FIXED /DYNAMICBASE:NO` from the
link line in `build.cmd`; the binary then comes out at 120,320 bytes.

The project directory may contain spaces and parentheses — `build.cmd` works on
relative paths throughout.

## License

[WTFPL](LICENSE) — do what the fuck you want to.
