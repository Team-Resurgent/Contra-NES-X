# Contra NES-X

A native original-Xbox port of *Contra* (Konami, NES, 1988 — the US
release), built from
[vermiceli/nes-contra-us](https://github.com/vermiceli/nes-contra-us), the
annotated disassembly of the cartridge.

The disassembly is **6502 assembly**, the NES's own machine code, so the game
runs as 6502 code on the Xbox, on a small NES built in `src/core/` for this
game:

* a 6502 (`cpu6502.c`), the 2A03's (no decimal mode), which passes every
  binary test of Klaus Dormann's 6502 functional test
* the 2C02 PPU (`ppu.c`): the scroll registers, CHR-RAM — Contra
  decompresses its own tiles into it — the nametables, eight-by-sixteen
  sprites, sprite 0 and the eight-a-line limit, NTSC timing and its NMI
* the 2A03's APU (`apu.c`): both pulses, the triangle, the noise and the
  DMC (Contra's drums), the frame counter, mixed as the hardware's resistor
  ladders mix them and filtered as the console's output stage filters them
* the machine (`nes.c`): the UxROM board (mapper 2) with its bus conflict,
  the two pads, OAM DMA, and save states of the whole machine

The colours are worked out from the 2C02's signal (`tools/palette.py`), not
copied from an emulator's palette.

The Xbox side (`src/xbox/`) puts the picture on the TV with Direct3D, the
sound out through DirectSound, reads the controllers, and keeps a save
state on the hard disk.

**No game data is in this repository.** You put your own cartridge in the
folder as `baserom.nes`; `setup.py` fetches the disassembly and cc65 at
pinned revisions, takes the graphics and music the disassembly does not
carry from your cartridge (at the offsets its `assets.txt` lists), and
assembles the cartridge — the disassembly builds it **byte for byte**, and
the build checks that it does.

| Cartridge | Size | MD5 | CRC32 |
| --- | --- | --- | --- |
| Contra (US) | 131,088 | `7BDAD8B4A7A56A634C9649D20BD3011B` | `C50A8304` |

A dump that differs only in the iNES header's unused bytes is taken too.
*Probotector*, the PAL version, is not: this port runs the NES at NTSC's
speed.

## Features

* The whole game: the eight stages, the 3-D bases, the ending — and the
  30-lives code at the title
* Two players: the first two controllers plugged in
* Rapid fire on Y
* Save states: the game's state at any pause, kept on the hard disk
  (dashboard > Memory > Contra NES X), loaded while paused or at the title
* 720p and 480p (progressive or interlaced), following the dashboard; the
  224 lines a television shows scaled by a whole number (×3 at 720p, ×2 at
  480) so every pixel is the same size, in the NES's backdrop colour
* PAL consoles: 60 Hz when the dashboard allows PAL60, else 50 Hz with the
  game still running at its own 60
* Optional: no sprite flicker (right-stick click), smoothed picture
  (left-stick click)

## Controls

| Xbox | NES | In the game |
| --- | --- | --- |
| D-pad / left stick | the cross | move and aim |
| A | A | jump |
| B or X | B | fire |
| Y | B, fifteen times a second | rapid fire |
| Back | SELECT | 1 or 2 players at the title |
| Start | START | start; pause |
| White | | save state, while paused |
| Black | | load state, while paused or at the title |
| Back + Start + both triggers | | back to the dashboard |

To save: **Start** to pause, **White** (STATE SAVED), **Start** to play on.
Saving again replaces the state. To load: **Start** to pause (or be at the
title), **Black** (STATE LOADED), **Start**. If the hard disk did not take
the state the port says SAVE FAILED, and the last one is still there.

## Requirements

* Windows 10 or 11
* your own Contra (US) cartridge, as `baserom.nes`
* the RXDK toolchain: in Visual Studio, **RXDK > RXDK Window > Install
  Prerequisites** (or RXDK's own installer), in `C:\ProgramData\RXDK`
* [Python](https://www.python.org/downloads/) 3.8 or later
* [Git](https://git-scm.com/download/win)
* an internet connection the first time: the disassembly and cc65 are
  fetched from GitHub

Nothing else — no CMake, Make, MSYS or a cc65 install: ca65, ld65 and the
PC-side test harness are compiled with the RXDK's own clang (or Visual
Studio's `cl`, or a `gcc` on the `PATH`).

`python setup.py --check` lists what is there and what is missing.

## Building

1. Download this repository (**Code > Download ZIP**, or `git clone`).
2. Put your cartridge in the folder as **`baserom.nes`**.
3. Double-click **`build.bat`**, or in a console:

```bash
python setup.py
```

The result is **`out\Release\XISO\Contra-NES-X.iso`** (about 1.2 MB).

The build runs, in order:

1. checks `baserom.nes`; fetches [the disassembly](https://github.com/vermiceli/nes-contra-us)
   and [cc65 2.19](https://github.com/cc65/cc65) at the revisions `setup.py`
   pins into `extern/`, builds ca65 and ld65, takes the graphics and music
   from your cartridge into a copy of the disassembly (`build/decomp`), and
   assembles and links it as its own build script does — checked against
   the cartridge: `build/Contra.nes`;
2. builds `tools/hosttest` (the port's NES on the PC) and has it run the
   game to its title, for the dashboard icons — the CONTRA logo
   (`tools/xbox/make_icons.py`);
3. puts the cartridge in `data/` (the disc's `D:\data\Contra.nes`);
4. writes `rxdk.project.json` and the Visual Studio project
   (`Contra-NES-X.sln`);
5. builds with `Rxdk.Cli`: the XBE, with its title and save icons, and the
   ISO.

`python setup.py --no-assemble` skips fetching and assembling, and puts
your (checked) cartridge on the disc as it is: the same bytes, offline.

To build from Visual Studio instead: run `python setup.py --skip-build`
once, then open `Contra-NES-X.sln` and build; F5 deploys to a devkit and
starts the Xbox Debugger.

Title ID `0x54520016`.

## On the PC: tools/hosttest

The port's core runs on Windows too, with no Xbox, to see that the game
works: `build/hosttest.exe` plays it headless with scripted input and writes
PNG screenshots and a WAV of the sound.

```bash
build/hosttest.exe build/Contra.nes --frames 2400 --press 420-423:START --press 520-2400:RIGHT+B --every 200 --out shots --wav shots/stage1.wav
```

`--press2` is player 2's pad; `--state-save F:FILE` and `--state-load F:FILE`
save and load states; `--poke` / `--peek` set and read the game's RAM
(with START pressed at frames 420-423, `--poke 600:30=1` starts on stage 2). `tools/hosttest/cputest.c` runs Klaus
Dormann's [6502 functional test](https://github.com/Klaus2m5/6502_65C02_functional_tests)
(`bin_files/6502_functional_test.bin`, not included) against the CPU.

## Checking a tree before publishing

```bash
python tools/xbox/release_check.py
```
