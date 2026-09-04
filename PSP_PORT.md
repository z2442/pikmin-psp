# Pikmin PSP port

This is the non-matching PSP port of the `GPIE01_01` (USA Rev 1) game. It is
kept separate from the original GameCube matching build.

## Current milestone

`make -f Makefile.psp psp-port` builds a PSP EBOOT that validates the PSP GU
framebuffer, alpha blending, system-font rendering, controller input, timing,
exit callbacks, executable-relative paths, and primitive submission through
the original GX-facing API. Audio is intentionally disabled.

The shared game code and the existing high-level `DGXGraphics` implementation
have also been compile-checked with `psp-g++`. The platform layer now contains
PSP replacements for the initial Dolphin OS, DVD/file, controller, and GX
calls. The current EBOOT is an interactive graphics bootstrap, not yet the full
game loop.

## Build

Install pspsdk/pspdev and ensure `psp-config`, `psp-g++`,
`psp-fixup-imports`, and `pack-pbp` are on `PATH`, then run:

```sh
make -f Makefile.psp psp-port
```

The result is `build/psp/GPIE01_01/EBOOT.PBP`.

An experimental whole-game build is also available:

```sh
make -f Makefile.psp psp-game
```

It compiles and links all 384 original high-level C++ files and writes
`build/psp/GPIE01_01-game/EBOOT.PBP`. It is an integration target, not yet a
playable release: indexed GX display-list decoding and tiled texture conversion
must be completed before the original game can render correctly.

## Game data

No copyrighted game data is committed by this project. Extract the filesystem
from a legally obtained `GPIE01_01` disc image, preserving names and directory
case. Copy its `dataDir` directory next to the EBOOT as:

```text
PSP/GAME/PIKMIN/
|-- EBOOT.PBP
`-- data/
    `-- dataDir/
```

The current EBOOT reports whether this directory is present. The supplied RVZ
must first be extracted by a GameCube-disc tool that supports RVZ; an RVZ file
cannot be used directly by the PSP runtime.

## Control mapping

| GameCube | PSP |
|---|---|
| Main stick | Analog nub |
| A | Cross |
| B | Circle |
| X | Square |
| Y | Triangle |
| L / R | L / R |
| D-pad | D-pad |
| Start | Start |
| Z | Select + R |
| C-stick | Select + D-pad |

The bootstrap currently uses the analog nub to move the red Pikmin and Circle
to exit.

## Porting stages

1. PSP platform bootstrap and portable compiler definitions (present).
2. Dolphin OS replacements: filesystem, timing, basic threads/messages/mutexes,
   and controller are present; save data still needs integration.
3. GX-to-GU renderer: immediate vertices, matrices, depth/blend/cull (present);
   texture decoding, indexed geometry, display lists, and the TEV combinations
   Pikmin uses remain.
4. Start the original `PlugPikiApp` loop with audio calls stubbed.
5. Profile memory and rendering on PSP-1000 and PSP-2000/3000 hardware.

The renderer should translate the existing GX-facing game code rather than
changing simulation or gameplay code. This keeps the port reviewable and lets
the original decompilation continue to match independently.
