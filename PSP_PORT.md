# Pikmin PSP port

This is the non-matching PSP port of the `GPIE01_01` (USA Rev 1) game. It is
kept separate from the original GameCube matching build.

## Current milestone

`make -f Makefile.psp psp-port` builds a PSP EBOOT that validates the PSP GU
framebuffer, alpha blending, system-font rendering, controller input, timing,
exit callbacks, executable-relative paths, and primitive submission through
the original GX-facing API. Audio remains disabled in this small bootstrap.

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
playable release: indexed GX mesh lists and basic tiled textures are supported,
while multi-stage TEV combinations and other GX features remain incomplete.

The whole-game target requires `libme-core` and includes the original JAM
sequencer, bank loader, and scene control. It converts the big-endian bank
metadata once, then directly decodes the game's AFC/PCM `.aw` samples and
streamed `.stx` audio on the Media Engine. PCM is sent to the PSP hardware SRC
at 32 kHz; no GameCube DSP instruction emulation is involved.

The opening planet/ship sequence uses the game's model-based cinematic player.
Demo audio loading now services pending completion callbacks while holding the
audio lock, preventing the opening's second segment from waiting indefinitely.

The whole-game target also plays the six original `MovieData/*.h4m` tutorial
and credits movies. It uses the repository's HVQM4 decoder with big-endian
bitstream reads, two YUV reference buffers, and a 360×270 RGB565 image centered
on the PSP screen. Compressed AFC movie audio is buffered and mixed into the
32 kHz output on the CPU. Movie data needs no transcoding. Start, Cross, or
Circle skips tutorial movies; endings keep the original unskippable behavior.
Playback speed and audio/video timing still need validation on PSP hardware.

The title renderer batches indexed triangle strips by model display list and
keeps positions in model space. The PSP GE applies rigid model matrices (or
bone matrices for batches using up to eight different GX matrices) and performs
hardware clipping. The general decoder remains available for other list types.
The native GU command checks compare transformed triangles and winding against
the original decoder, and can also check extracted title assets:

```sh
python3 tools/check_psp_gx.py \
  build/psp/GPIE01_01-game/data/dataDir/cinemas/opening/opening.mod \
  build/psp/GPIE01_01-game/data/dataDir/cinemas/titles/logo.mod
```

These checks need PSPSDK headers and a native C++ compiler; they do not measure
hardware frame rate or verify the final image on a PSP.

The native decoder and metadata checks can be run against the extracted bank:

```sh
python3 tools/check_psp_audio.py \
  build/psp/GPIE01_01-game/data/dataDir/SndData/Banks/pikibank.bx
```

Movie checks decode video and audio with address/undefined-behavior sanitizers,
then exercise timing, skip, restart, EOF, and invalid-file handling. They check
90 frames per movie by default; use `PIKMIN_MOVIE_TEST_FRAMES=0` for full files:

```sh
PIKMIN_MOVIE_TEST_FRAMES=0 python3 tools/check_psp_movies.py \
  build/psp/GPIE01_01-game/data/dataDir/MovieData/*.h4m
```

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
Include `data/dataDir/MovieData` with its original `.h4m` files for movie playback.

The game EBOOT writes `audio.log` beside itself with ME startup, sequence-load,
voice, PCM, movie start/end/errors, and PSP output milestones. This file is only updated when a new
milestone is reached, so it does not add continuous Memory Stick traffic.

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
3. GX-to-GU renderer: immediate vertices, matrices, depth/blend/cull, basic
   texture decoding, and indexed mesh lists (present); full TEV support remains.
4. Original `PlugPikiApp` loop and experimental ME-backed JAudio playback
   (present in the whole-game build).
5. Profile memory and rendering on PSP-1000 and PSP-2000/3000 hardware.

The renderer should translate the existing GX-facing game code rather than
changing simulation or gameplay code. This keeps the port reviewable and lets
the original decompilation continue to match independently.
