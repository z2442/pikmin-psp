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

The regular game build enables main-CPU link-time optimization (LTO):

```sh
make -f Makefile.psp psp-game -j8
```

Its EBOOT and symbol ELF use `build/psp/GPIE01_01-game/`. Install that
EBOOT in the existing PSP game folder alongside `data/dataDir`. LTO allows
compiler inlining and optimization across main-CPU source files, using the existing `-O2`
arithmetic and aliasing settings. The ME mixer and stream remain ordinary
objects so their independent callbacks and cache exchange retain their code
boundaries. Gprof builds automatically disable LTO. The log identifies an LTO
build in its first line; measure its hardware FPS and thread CPU times in the
same scene. A smaller EBOOT does not establish an FPS gain. For a comparison
without LTO, use `make -f Makefile.psp psp-game PSP_ENABLE_LTO=0 -j8`; switching
the option automatically rebuilds the objects in the regular game folder.

PSP animation bundle loading replaces the model's extension in the destination
bundle path itself. The old expression wrote `"anm"` at
`finalAnimPath + strlen(finalBundlePath) + 253`, outside its 256-byte buffer.
With the opening model and the first LTO build's stack layout, that address
was precisely the saved `s2` slot: the resulting `0x006d6e61` value then caused
an Address Store when `CineShapeObject::init` assigned the manager's name.
The PSP path now derives `cinemas/opening/opening.anm` and
`cinemas/titles/logo.anm` with bounded copies, preserves explicit bundle names
literally, and rejects paths that do not fit. LTO remains enabled by default.
The regression exercises the production loading block with ASan/UBSan both
with and without LTO, and reproduces the original overwrite when given the
old `animMgr.cpp`:

```sh
python3 tools/check_psp_animation_path.py
```

Native renderer, audio and movie regressions also support LTO compilation:

```sh
PIKMIN_PSP_TEST_LTO=1 python3 tools/check_psp_gx.py
PIKMIN_PSP_TEST_LTO=1 python3 tools/check_psp_audio.py
PIKMIN_PSP_TEST_LTO=1 python3 tools/check_psp_movies.py \
  build/psp/GPIE01_01-game/data/dataDir/MovieData/*.h4m
```

The whole-game target requires `libme-core` and includes the original JAM
sequencer, bank loader, and scene control. It converts the big-endian bank
metadata once, then directly decodes the game's AFC/PCM `.aw` samples and
streamed `.stx` audio on the Media Engine. PCM is sent to the PSP hardware SRC
at 32 kHz; no GameCube DSP instruction emulation is involved.

Audio transport sends an 80-byte mixer channel snapshot in place of each
384-byte GameCube DSP block. JAudio's CPU-owned channel table no longer receives
redundant DSP cache flushes. The 16 KiB stream input is published only on refill
or reset; the 256-byte playback state still synchronizes for every 80-sample job.
This preserves the sequencer cadence, mixer fields and completion updates while
reducing interprocessor cache traffic. `audio.log` reports the transport sizes.
Native transfer-range, refill/reset, and channel-field checks run with:

```sh
python3 tools/check_psp_audio_transport.py
```

For a separate gprof build, use:

```sh
make -f Makefile.psp psp-game-gprof -j8
# Equivalent: make -f Makefile.psp psp-game PSP_ENABLE_GPROF=1 -j8
```

This writes `build/psp/GPIE01_01-game-gprof/EBOOT.PBP` and keeps its objects
and unstripped `pikmin-psp.elf` separate from the normal build. The bootstrap
equivalent is `psp-port-gprof`, with output in `build/psp/GPIE01_01-gprof`.
The profiling EBOOT packages a standard ELF payload so the SDK profiler gets
the required startup and text bounds; the normal EBOOT continues to use PRX.
Install the profiling EBOOT alongside the usual `data/dataDir`. Profiling
starts during platform initialization; exiting through HOME or normal shutdown
writes `gmon.out` beside the EBOOT. Forced termination cannot save a profile.
The ME audio mixer and stream code are built without instrumentation because
the profiler runs on the main CPU.

Copy `gmon.out` back and use the ELF from that exact build:

```sh
psp-gprof -b -p -q build/psp/GPIE01_01-game-gprof/pikmin-psp.elf gmon.out > gprof.txt
```

The first profile-driven renderer optimization skips the vertex tint pass for
ordinary materials and caches three 256-byte channel tables for tinted materials.
The tables preserve the existing signed TEV arithmetic and opacity; they are
rebuilt when the RGB tint or scale changes. Native GX checks compare all signed
tint values and scales against the previous arithmetic, alongside the existing
geometry checks. To measure the gain, repeat the same scene and duration on PSP
with the updated build. Keep each capture with its matching ELF before rebuilding;
profiling overhead and time spent waiting can distort the flat-profile rankings.

Mesh conversion has a bounded 4 MiB cache of native vertices and triangle
indices. Normal model draws carry a globally unique revision, assigned at model
construction/loading and replaced when source data changes. Array dirty flags
invalidate it at binding; the slime's position/normal deformation explicitly
invalidates it at the mutation sites. Future code that edits a model's arrays or
mesh display lists must call `BaseShape::invalidatePspMesh()` on PSP. Model
matrices and material state remain live; rigid animation needs no source scan.
Revision-tracked entries store no array or display-list snapshots. Compact
state keys retain every attribute source (which determines FIFO offsets), but
array metadata only for position, primary color/UV, and consumed normal/NBT inputs.
Cache hits skip vertex-layout reconstruction. Keys compare texture matrices only
when generating texture coordinates from positions or normals. Uniform texture
matrix selection compares only that matrix; per-vertex selectors retain all ten.
Unused normal arrays do not invalidate ordinary UV materials. Replacement uses
the matching state or the least recently used unpinned entry, preserving separate
material/projection variants of one display list.

Generic GX calls outside a revision-tracked model draw still compare source
bytes. Shared array validation is available for callers that can guarantee
read-only arrays within a draw scope, with a separate 2 MiB snapshot budget.
Snapshot comparisons use unrolled word equality checks, including unaligned
word loads. Skinned meshes cache their native vertices, fixed weights and index
buffers too; each submission uploads the current bone matrices in the original
slot order. CPU-deformed source arrays still require revision invalidation. Allocation
failure falls back to conversion. Cached buffers are submitted directly to the
GPU and remain protected from eviction until a GPU sync completes.

The native GX checks exercise reuse, source mutations, animated matrices,
reflection changes, and eviction protection. Replaying the opening and logo
models together reproduces the title's 71,010 decoded vertices per frame; after
warmup the eight-way cache avoids all 71,010 conversions in that replay. The reported native benchmark
ratio is not a PSP FPS measurement. In `performance.log`, `mesh_cache_hits` and
`cached_vertices` report reuse, while `cache_validate_ms` measures lookup and
state checks within `mesh_ms` (plus byte comparisons for unversioned calls).
`array_validate_ms` measures shared array comparisons inside `render_ms`, outside
`mesh_ms`; normal revision-tracked model draws do not perform these comparisons.

Gameplay cache diagnostics are averages per frame: `mesh_miss_absent` means no
entry for that list address/length, `mesh_miss_state` means its revision or render
state changed, and `mesh_miss_source` means unversioned source validation failed.
`mesh_store_failures` counts capacity, allocation, or GPU-pinning failures;
`mesh_evictions` counts replacements during insertion. These distinguish changing
inputs from cache pressure without per-vertex logging. The native Olimar
`pikis/nv3Model.mod` replay previously rebuilt 1,300 skinned vertices every frame;
after warmup it now rebuilds zero and reuses all 12 lists. This verifies conversion
reuse, not an on-device FPS gain. Repeat the same gameplay route on PSP and compare
`main_cpu_ms`, `mesh_ms`, `decoded_vertices`, and the miss counters.

The normal build also writes `performance.log` beside the EBOOT every 120
presented frames. It reports average FPS, frame time, final GPU wait, display-list
split wait, framebuffer-copy time (including its GPU wait), and draw/vertex/copy
counts. Each launch starts a new log at the first report. Use this log from the
slow scene to distinguish GPU/copy stalls from the remaining game, audio, CPU
rendering and pacing time without gprof instrumentation. A second line now
reports main/audio thread CPU time, render, mesh conversion, texture loading,
retrace waits, audio work, and audio-lock waits. Wall-time scopes overlap (for
example mesh work is part of rendering, and audio can run during either), so
these columns must not be added together. The submitted-index count includes
vertex reuse; `decoded_vertices` counts actual mesh vertex conversions.
`skinned_vertices`, `fallback_lists`, and `texture_misses` help identify expensive
paths. Counter units and wraparound can be checked with
`python3 tools/check_psp_performance.py`.

PSP frame pacing reads the hardware refresh counter, including refreshes that
pass during rendering. This avoids adding the full pacing delay to frames that
have already missed their target. Check the VI shim with:

```sh
python3 tools/check_psp_retrace.py
```

The opening planet/ship sequence uses the game's model-based cinematic player.
Demo audio loading now services pending completion callbacks while holding the
audio lock, preventing the opening's second segment from waiting indefinitely.

Generator IDs retain the original least-significant-byte-first disc format on
PSP. This lets the first area's `v0.1` header consume its starting direction
before reading the generator count, fixing an endian error during loading after
the ship crash. The native regression checks exercise ID strings, serialization,
generator helpers, and the first-area header. They also check route-name
conversion and the Onion's nearest-waypoint lookup: the route system has a
separate ID representation that must preserve names such as `test` on PSP.
Reversing that name makes the lookup return null during Onion initialization.

```sh
python3 tools/check_psp_generator_ids.py \
  build/psp/GPIE01_01-game/data/dataDir/stages/practice/default.gen
```

The asset argument is optional; a synthetic header is always checked.

PSP AI condition trees use named local operands so their pointers remain valid
through subsequent searches and interactions. The old temporary-address pattern
expired at the declaration's semicolon and could crash enemy startup. A native
sanitizer regression exercises the Palm startup construction and condition
short-circuiting:

```sh
python3 tools/check_psp_conditions.py
```

PSP collision impulses calculate the change in normal velocity directly. The
original restitution calculation divided by the incoming normal speed, causing
an FPU invalid-operation exception (`div.s`) on resting or tangential contacts
with zero normal speed. The simplified expression preserves ordinary collision
response and avoids that intermediate division. Check resting, tangential,
bouncing, and off-centre contacts with floating-point exception checks:

```sh
python3 tools/check_psp_impulse.py
```

Environment texture generation handles zero-scale model matrices without
dividing by zero. Collapsed models use the texture centre; ordinary and mirrored
scales retain their original mapping. Check the arithmetic with:

```sh
python3 tools/check_psp_texgen.py
```

The whole-game target also plays the six original `MovieData/*.h4m` tutorial
and credits movies. It uses the repository's HVQM4 decoder with big-endian
bitstream reads, two YUV reference buffers, and a 360×270 RGB565 image centered
on the PSP screen. Compressed AFC movie audio is buffered and mixed into the
32 kHz output on the CPU. Movie data needs no transcoding. Start, Cross, or
Circle skips tutorial movies; endings keep the original unskippable behavior.
Playback speed and audio/video timing still need validation on PSP hardware.

Movie RGB565 conversion now precomputes horizontal sample positions and uses
5 KiB of constant integer YUV coefficient tables. The 360 by 270 playback path
performs 360 horizontal scaling divisions per converted frame instead of
97,200; nearest-neighbour sampling, clipping, rounding and pixel packing remain
identical. The column map uses a bounded 2.5 KiB stack buffer, with a generic
fallback for output widths above 640. Native reference checks compare all chroma
pairs at ten luma values, up/downscaling, row padding and decoded movie frames.
A native 640 by 480 to 360 by 270 conversion benchmark measured approximately
0.67 ms before and 0.31 ms after; this is not a PSP frame-rate measurement.

The RGB allocation is fully written back once at initialization. Subsequent
frames publish only the 270 displayed rows, reducing that cache range from
524,288 to 276,480 bytes while retaining the initialized texture padding.
`performance.log` now reports `movie_decode_ms` (container reads, video decoding
and audio queuing), `movie_convert_ms` (YUV scaling/color conversion) and
`movie_publish_ms` (RGB cache writeback). These are averages per presented frame,
including frames without new movie video; they are separate from
`audio_movie_ms`, which measures audio mixing. Compare the same movie on PSP
to establish the actual gain and locate the remaining playback bottleneck.

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

Gameplay rendering on PSP bypasses the original two-texture feedback blur.
That pass sampled an unfilled framebuffer texture and could cover the wake-up
scene in black. The backend now captures tiled I8 shadow maps and RGB565 scene
copies, refreshes their cached uploads, and generates position-based shadow
coordinates. The experimental CPU
lighting/color-channel changes were rolled back after they regressed title
rendering following screen-mode selection. The prior material/vertex-color
approximation remains active. Native checks cover shadow coordinates,
framebuffer-copy tiling/endian conversion, and opening/title geometry.
Full GX lighting, multi-stage TEV, and feedback blur remain incomplete;
gameplay appearance and capture performance still need PSP hardware testing.

Title/options menu pictures now preserve their P2D opacity on PSP. The original
menus deliberately set their full-screen `back` pictures to alpha zero so the
title cinematic stays visible. P2D normally applies that opacity in a later
TEV stage; ignoring it made those pictures cover the scene after pressing
Start. PSP picture draws now carry the picture alpha in their white vertex
colors, which the GU modulates with the texture alpha. Fully transparent
pictures return before texture loading or drawing. The GameCube TEV path keeps
its original opacity behavior. Native GX checks run the real P2DPicture drawing
code, checking no submission for the hidden background, all 255 visible alpha
levels, UVs, geometry and restoring an opaque picture. Confirm title/options
backgrounds and transitions on hardware with the rebuilt EBOOT.

The opening ship's reflection maps now generate texture coordinates from
model-space normals using the game's texture matrices, including their
half-texture offset. Indexed normals, NBT entries, and immediate normals are
supported without changing the title's material-color path. The native GX
checks also accept `cinemas/demo01/ufo.mod` and compare reflection coordinates
between the batched and fallback decoders. Full multi-stage metallic/specular
material effects still remain approximate.

The Onion body now uses its animated signed TEV color register for the
`C0 * raster` material operation. This preserves its red/yellow/blue tint
without enabling the experimental CPU lighting path. Native GX checks cover
all three tints, opacity, signed clamping, indexed geometry, and restoring
ordinary title materials. Secondary specular lighting remains approximate.

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

## PSP boot and save storage

PSP builds skip the GameCube progressive-scan selection at boot. The PSP LCD
uses the port's fixed output mode, and boot proceeds to the title screen.
The original GameCube/PAL conditions remain for other builds.

The existing in-game memory-card menus use a virtual card stored beside the
game executable. Card A is created automatically under `saves/card_a/`;
Card B uses `saves/card_b/`. Direct launches store these on Memory Stick or
PSP Go internal storage. PSPLink `host0:` launches store them in the host's
mapped game directory. Detection checks/creates these save directories;
it does not require inspecting `ms0:/` or `ef0:/`, or redirect a host launch
to another filesystem. Synchronization targets the selected save filesystem.
Each card exposes 2 MiB with the original five
reserved sectors, 8 KiB sectors and up to 127 files. The game's single card file
contains its original three save slots, spare game block and options copies.
Numbered `.pcs` files contain a versioned header, display metadata and game
data. Back up the entire `saves` directory to preserve progress and options.
These containers are specific to the PSP port, rather than GameCube card images.

Full saves and partial block writes stage a complete file, verify transfer
results and close it, request filesystem synchronization, and read back its
header and payload CRCs before promotion. They preserve the previous file
as `.bak`, request synchronization for that backup, and promote the staged file. Mounting checks
header and payload CRCs and restores a valid backup if the current file is
missing or corrupt. Corrupt current files are retained as `.bad` when recovery
is possible; incomplete `.new` files are ignored. An unrecoverable file reports
a broken card through the game's existing error/format flow. Formatting only
removes the virtual card's numbered files and their transaction companions.
HostFS renames pass the destination basename, which its driver rebases onto
the source directory. The PSP kernel expands the source path but forwards
the destination verbatim; a full `host0:/...` destination produces an invalid
host path. This handling covers promotion, backups, rollback and recovery.

Game utility requests run on the existing card worker when it starts; if PSP
cannot allocate or start it, the same operations execute on the calling thread
so saving remains available. PSP waits
sleep while that worker is busy, protect shared completion state with its
mutex, and preserve a save error before unmounting. Save creation/replacement
uses one backend transaction instead of deleting the old save before renaming
a temporary GameCube file. Load sizes are checked against the game buffer.
PSP integer/short writes use unsigned endian conversion, and card checksums
read big-endian words with unsigned arithmetic, including with LTO enabled.

Memory Stick synchronization tries `ms0:`, the SDK-documented `msfat0:`, and
`fatms0:` endpoints. Unsupported device, operation or device-wide synchronization
permissions do not reject a successfully closed and verified save. Other sync
errors still fail the save. Both errno-family errors and native PSP kernel
device-control errors (`0x80020321`, `0x80020324`, `0x80020325`, and permission
error `0x800200d1`) are handled; treating the latter as errno values rejects
valid saves before the staged file can be promoted. Failed writes/reads/closes
and a full filesystem still fail the operation.
USB HostFS returns plain `-1` for an unsupported sync devctl. This result is
accepted only for `host*` save devices, after checked writes and closes; staged
header/payload verification still runs before promotion. The same `-1` remains
an error on Memory Stick/internal storage. A real PSPLink user-mode backend
test reproduced `sync=-1`, `save=-5` before this change and verified a complete
0x26000-byte save, byte-for-byte reads, cold remount/reload and deletion after it.
The native shim exercises each endpoint and unsupported sync, and corrupts a
staged payload to require verification to reject it while retaining the old save.
It also checks saving without a worker. Startup checks wait for queued card
utility work before mounting; the regression runs the game's actual empty-card
detection path before creating any save. It creates the default save with the
game's banner, icons and all seven blocks while sync returns the native PSP
unsupported-operation error, then reopens it and runs the game's corruption
checks after a cold card restart.
The PSPLink regression models the firmware's unexpanded rename destination
and USB HostFS path handling: the old host-relative backend fails immediately
after successfully reading an empty card. The regression also rejects storage
root stat calls to reproduce the subsequent detection regression caused by
redirecting saves. With the redirect removed and the HostFS rename corrected,
it detects an empty virtual card, creates and reloads all game blocks, replaces
files, restores backups and deletes files in the mapped host game directory.

Run `python3 tools/check_psp_card.py` for the native LTO sanitizer regression.
It exercises the production storage and card utility save/list/write helpers,
all seven game block writes, persistence across processes, metadata offsets,
short transfers, byte/file capacity, callbacks, channel isolation, injected I/O
failures and termination after each mutation in a save transaction. It also
checks backup recovery, broken-card formatting, actual primitive serialization,
a golden game checksum and the game's save-result/polling methods.
`tests/psp_card_hardware.cpp` is the isolated user-mode PSPLink backend test;
its 16-word `cardHardwareResults` array records completion, probe/mount, raw
sync, save, file index, read/compare, cold mount/open/read/compare and deletion
results for inspection with PSPLink. It uses only `build/psp/checks/saves` for
its temporary file, deletes the file on success, and leaves no game logger.
Full in-game save/load and Memory Stick durability still need hardware checks.

## Porting stages

1. PSP platform bootstrap and portable compiler definitions (present).
2. Dolphin OS replacements: filesystem, timing, basic threads/messages/mutexes,
   controller and Memory Stick-backed virtual cards are present.
3. GX-to-GU renderer: immediate vertices, matrices, depth/blend/cull, basic
   texture decoding, and indexed mesh lists (present); full TEV support remains.
4. Original `PlugPikiApp` loop and experimental ME-backed JAudio playback
   (present in the whole-game build).
5. Profile memory and rendering on PSP-1000 and PSP-2000/3000 hardware.

The renderer should translate the existing GX-facing game code rather than
changing simulation or gameplay code. This keeps the port reviewable and lets
the original decompilation continue to match independently.

### Active-voice ME transport

Mixer jobs now pack only enabled, unfinished voices and carry their original
channel IDs to preserve decoder history across packing changes. Inactive DSP
channels are untouched. The cache exchange is 448 bytes plus 80 bytes per active
voice, rounded to 64 bytes (576 bytes for one voice, versus 5,504 previously).
The 64-voice limit, 80-sample tick, sequence cadence, and output rate are unchanged.
Native transport tests cover sparse/full/empty jobs, completion, slot reuse, and
return-field ownership. Actual speedup and ME cache coherence need PSP testing.
Compare the same scene's `audio_cpu_ms`, `audio_mix_ms`, `cache_validate_ms`, and
`frame_ms`; audio totals are per rendered frame, so they also fall when FPS rises.

### GPU bandwidth and ME idle polling

Converted 4444 textures at least 8 by 8 pixels now use the GE's 16-byte by
8-row swizzled layout. Smaller textures and movie frames remain linear.
Framebuffer-copy refreshes use the same layout as their cached texture.
Texture dimensions, pixel values, filtering, and UV scales are preserved.
The renderer also tracks the model matrix actually loaded in GU, skipping
redundant uploads and invalidating that state on matrix edits or temporary
identity matrices used for skinning and pretransformed vertices.

The idle ME uses a bounded 256-iteration register-only loop between mailbox
polls instead of continuously reading shared uncached memory. This targets
shared-bus contention without changing audio tick or output timing; job pickup
can be delayed by a few microseconds. PSP measurements are still required to
verify the performance effect and audio/video behavior.

### Cached draw submission batches

The installed PSPSDK's `sceGuDrawArray` calls `sceGeListUpdateStallAddr` for
every draw in a direct list. Persistent cached meshes now emit the same GE
vertex-format/address/primitive commands with `sceGuSendCommandi`, using a
normal SDK draw for the first and each subsequent sixteenth cached draw to
publish preceding commands. Ordinary draws and list completion also publish
pending commands. Draw order, matrices, indices, and cache pinning are unchanged;
list splits, framebuffer copies, and presentation still finish/synchronize
before storage reuse. This bounds deferred work to 15 cached draws.

`gpu_cached_kicks` counts cached draws using the SDK submission path;
`gpu_deferred_draws` counts those using command-only emission (both per frame).
These are not total GPU syscall counts: transient draws and allocations can
also publish commands. Native tests decode the emitted command words, resolve
both addresses against pinned cache entries, and compare triangles and winding
through batch boundaries. Hardware testing must establish the FPS effect.

### Reusing raster state on PSP

A real-PSP title capture with 606 mesh hits and no vertex conversions still
reported approximately 12 FPS, 51 ms of main-thread CPU and 29 ms of audio-thread
CPU per presented frame. Rendering's roughly 79 ms wall time overlaps the audio
thread. Final GPU waits were below 0.5 ms in the steady title windows, and movie
timers were zero. This points the next pass at CPU rendering/submission and
audio work; increasing the mesh cache or optimizing movie conversion does not
address this particular capture.

The GX backend now reuses identical GU enables and blend, depth, alpha-test,
culling-direction and pixel-mask parameters. It tracks actual GU state during
movie overlays too, preserves it across direct-list restarts and conservatively
invalidates it after framebuffer clears. The independently owned bootstrap
renderer bypasses reuse. Animated material colors and matrices remain live.
`gpu_state_changes` counts these emitted SDK state updates per frame;
`gpu_state_reuses` counts identical updates skipped. These counters do not count
all GE command words or state categories.

The native recorder verifies state transitions, fixed blend colors, overlay
restoration, list restarts and invalidation. A synthetic sequence of 606
identical material setups drops from 6,666 SDK state updates to 11. That is a
repeated-state check, not the actual title's update count or a PSP FPS result.
Repeat the same title capture with the rebuilt EBOOT and compare `frame_ms`,
`main_cpu_ms`, `render_ms`, `mesh_ms`, and the new state counters. Audio CPU time
is per rendered frame, so compare `audio_cpu_ms * fps / 1000` for its CPU-time
share if the frame rate changes.

The follow-up hardware title log reports 3,891 skipped state updates per frame
and approximately 47 ms of main-thread CPU, versus 51 ms before reuse. Steady
FPS remains around 12; pacing waits increased from roughly 4 ms to 8–10 ms.
This confirms lower CPU cost but only a small observed frame-rate improvement
in those captures. Audio CPU remains approximately 28–30 ms per title frame.

### Lower-cost mesh timing and empty audio interrupt checks

The 606-list title previously made 2,424 kernel clock reads per frame just to
time mesh submission and cache lookup. Detailed mesh timing now samples one
in 16 list calls, reducing those reads to approximately 152. The sampling phase
rotates each frame so stable draw positions receive equal coverage over 16
frames. `mesh_ms` and `cache_validate_ms` are scaled estimates; the log header
labels them accordingly and reports `mesh_timing_samples` and
`mesh_timing_interval`. Frame time, render time, thread CPU time and all
draw/vertex/cache counters remain fully measured. Use these unsampled times
and FPS to assess hardware speedup, rather than treating a change in the mesh
estimates as proof. Short-lived or uneven workloads can skew sampled estimates.

On PSP, the sequencer also returns immediately when no enabled interrupt is
pending, avoiding its eight-slot scan on ordinary ticks. Priority, pending
bits, saved program counters, nesting and timers retain the original behavior.
The native audio checker compares all 65,536 enabled/pending combinations in
three active-interrupt states. Timing tests check disabled scopes, weighted
clock wraparound, actual mesh clock-read counts and phase rotation; renderer
checks still exercise real menu opacity and the extracted title models.

The latest menu-open capture preceding this pass has 740 draws per frame and
roughly 10 FPS, with approximately 56 ms main-thread and 31–33 ms audio-thread
CPU per frame. Compare the rebuilt EBOOT against the same menu-open scene,
as well as the title with its menu closed. Hardware testing is required to
establish the gain from this pass.

### Deferred model matrix uploads

GX matrix edits and selection now update the backend's CPU state. The GU model
matrix is uploaded when a draw actually consumes it. Cached rigid draws select
their own live matrix; skinned draws use identity and their live bone slots;
CPU-transformed fallback draws use identity; immediate/UI draws consume the
current GX matrix. The renderer no longer uploads/restores an unused current
matrix around each mesh. Edits to the matrix actually loaded in GU still
invalidate its reuse, including when GX currently selects a different slot.

`gpu_model_uploads` reports draw-related GU model matrix uploads per presented
frame. It excludes bone uploads and the initialization identity matrix. Native
checks verify one upload across 33 repeated rigid draws while an unused slot
changes, then verify skinned, fallback and immediate transitions. Extracted
opening/logo geometry and menu opacity pass with ordinary and LTO compilation.
This proves reduced uploads and preserved geometry; measure PSP frame time and
main-thread CPU to establish the performance gain.

### Compact mesh lookup and animated texture coordinates

The following hardware capture reached approximately 14 FPS in the steady
title, with 37 ms of main-thread CPU and 26 ms of audio-thread CPU per frame.
Later gameplay windows dropped to approximately 8 FPS with 12,000 vertices
converted per frame and approximately 54 material-state misses. GPU presentation
waits remained below 0.1 ms in those gameplay windows. This makes repeated mesh
conversion a useful target alongside title submission overhead.

Cache lookup now checks a separate, 64-byte-aligned table of display-list
addresses and lengths before accessing geometry and lifetime metadata. The
tags occupy 16 KiB on PSP; replacement, failed insertion and clearing remove
the corresponding tag. Exact material checks, source validation, model lifetime
revisions and GPU pinning remain in place.

Meshes using position- or normal-generated texture coordinates retain their
generation inputs alongside native geometry. When only texture matrices change,
the renderer copies native vertices into GU list memory and updates their UVs.
It reuses triangle indices and bone weights, avoids list decoding and persistent
buffer replacement, and pins the indices until GPU completion. Matching matrices
still draw the cached buffer directly. Geometry, tint, bindings, selectors,
generation mode and model revisions still participate in validation. Auxiliary
inputs stay within the existing 4 MiB cache budget. Large meshes that cannot fit
the additional inputs in GU list memory keep their original full conversion path.

`mesh_uv_refreshes` and `mesh_uv_vertices` report these refreshes per frame.
They count as cache hits and cached vertices, but do not increment
`decoded_vertices`; matrix-only reuse is distinct from complete geometry
conversion. Refresh draws use the SDK submission path because their vertices
are transient. They are excluded from the persistent cached-draw kick counters.

Native sanitizer tests with LTO check 16 rigid/skinned, position/normal,
uniform/per-vertex and affine/projective combinations across 20 animation frames,
including zero projective denominators and identity selectors. They compare UVs,
colors, positions and triangles to the reference, verify persistent buffers stay
unchanged and pinned, and require source edits to invalidate geometry. They also
check oversized meshes, tag consistency, eviction, menu opacity and both real
title models. Repeat the same PSP scenes to establish the FPS improvement;
native conversion measurements do not establish hardware frame rate.

### Direct validation of cached mesh state

The saved title capture reports 12.69 FPS, 38.65 ms of main-thread CPU and
57.30 ms of render wall time, despite reusing all 606 lists and converting no
vertices. Cache hits now compare their stored dependencies directly with live
GX state. They no longer clear and populate a temporary mesh key or copy
texture matrices before lookup. A 26-byte descriptor shadow, updated by the GX
descriptor setters, retains every attribute's contribution to FIFO offsets.
Keys are constructed only for conversion/cache insertion. Position-matrix
equality checks also use the existing wordwise comparator instead of libc's
byte-ordering comparison.

Model revisions, consumed array bindings/formats, material tint, constant
colors, matrix selectors and texture generation remain validated. Generic
unversioned calls still compare source snapshots, and animated UV refreshes
retain the existing GPU pinning rules. `mesh_key_builds` reports constructions
per presented frame; it should fall to zero after title-cache warmup. The log
header identifies live-state validation. `cache_validate_ms` now includes the
direct field comparisons; previously key construction was outside that scope.
Use FPS, `frame_ms`, `main_cpu_ms` and `render_ms` for hardware comparisons.

Native sanitizer checks compare direct validation with canonical key equality
across 21,312 state mutations, including ignored arrays, descriptor offsets,
signed tints, revisions, all texture-matrix elements and identity selectors.
Ordinary and LTO renderer checks preserve the extracted opening/logo triangles,
animated reflections, skinned geometry, cache eviction protection and menu fades.
These checks verify correctness and eliminated key construction; an FPS gain
still requires the rebuilt EBOOT to be measured on PSP.

### Reusing GE draw commands and sampling audio detail timers

The follow-up capture identifies live-state validation and reports no title
key builds after warmup. Its windows include transitions between scenes, so
their FPS cannot be treated as a like-for-like steady-title comparison. They
still show substantial main-thread rendering and audio CPU costs.

The previous build stored six immutable GE draw words and a RET in an
aligned 64-byte block alongside each mesh's vertices/indices. Deferred 3D submissions
emitted BASE/CALL instead of regenerating all six words. The first and every
sixteenth cached submission still use the SDK draw to publish pending work.
Matrices and raster state are applied in the parent list before the call.
The command block shares the geometry's lifetime, GPU pinning and 4 MiB budget;
all 606 title entries add 38,784 bytes. 2D mode changes and transient animated
UV vertices keep their dynamic-format submission paths.

`gpu_compiled_mesh_calls` counts these calls per presented frame. Native GU
checks resolve each called list against pinned storage, validate all six
opcodes and the terminating RET, then compare the resulting triangles and
winding. They cover batch boundaries, restored 3D state, source edits, eviction,
skinning and reflection refreshes. The recorder does extra address-resolution
work for these calls, so its native benchmark is unsuitable for comparing the
submission cost to the preceding build.

The audio worker samples its five detail timers once per eight 80-sample ticks,
rotating the sampled tick between output buffers. This reduces those timers
from 4,000 to 500 kernel clock reads per second at the existing 400 Hz cadence.
Every sequencer/channel/stream/mixer/movie callback still runs on every tick.
The log labels these five detailed times as estimates; audio work, lock/ME
waits and thread CPU measurements remain exact. Native timing checks verify
the clock-read reduction, weighted accounting, phase coverage and wraparound.
Renderer and audio checks pass with LTO; actual FPS and audio playback still
need a PSP capture using this EBOOT.

### Late-frame presentation and streaming UV refresh

The latest title window reports 13.36 FPS, 32.23 ms main-thread CPU, 25.92 ms
audio-thread CPU, 12.44 ms of final GPU wait and 8.14 ms in retrace pacing.
Later gameplay windows report roughly 35 ms of mesh work with 118 UV refresh
draws and 18,009 refreshed vertices per frame. Lower main-thread CPU time alone
has not established a comparable FPS improvement.

Cached draw words now emit inline through the original six-command path.
This retains precomputed formats/addresses while removing per-mesh GE CALL/RET
and its additional command-buffer fetch. `gpu_compiled_mesh_draws` replaces
the former `gpu_compiled_mesh_calls` counter; it counts reuse of precompiled
words, not GPU subroutine calls.

Presentation now completes the GPU list before deciding whether a refresh
wait remains necessary. Frames that have already passed their target refresh
interval still dispatch retrace callbacks and can queue their display swap
without waiting for another blank. Fast frames retain the requested 60/30 FPS
refresh interval. Three RGBA8888 color buffers and one 16-bit depth buffer use
1,949,696 of the PSP's 2,097,152 VRAM bytes. The spare buffer lets a new frame
start while the just-finished frame is pending NEXTFRAME scanout. At least one
refresh passes between swaps before a previously displayed buffer is recycled.
Color format, resolution, geometry and material behavior are preserved.

`pacing_bypass_frames` counts frames that needed no additional refresh wait
in each 120-frame window. Native checks run the production presentation path
with simulated CPU/GPU refreshes, checking completion/pacing/swap/clear order
and forbidding writes to both the visible and pending display buffers. Separate
retrace checks cover 60/30 FPS targets, missed targets, zero intervals, callbacks
and counter wraparound. A previous pacing wait could overlap GPU work, so its
elimination is not necessarily an equal reduction in total frame time.

Animated UV refresh now copies geometry/weights and calculates coordinates in
one streaming pass instead of copying the whole vertex buffer and revisiting
it to overwrite UVs. Uniform/per-vertex and rigid/skinned paths select their
matrix work once per draw. Reflection matrices with constant q=1 avoid two
unnecessary divisions per vertex; other projective matrices keep the original
division and rounding. Native checks compare all output bytes for 1,120 vertices
across projective thresholds, identity/invalid selectors, weights and constant
denominators, in addition to the existing animated-model tests. Ordinary and
LTO renderer checks preserve the real opening/logo, Olimar and ship geometry.
Measure the rebuilt EBOOT on PSP to establish FPS, scanout and playback behavior.

### Native PSP strips, byte weights and heap pressure

The latest steady title capture reports 14.23 FPS, 26.00 ms main-thread CPU,
37.13 ms render wall time and 25.61 ms final GPU wait. Pacing is already zero
and all 606 meshes are cached. This pass reduces native GE input work.

Lists containing only triangle strips now use one indexed GE triangle-strip
draw when it needs fewer indices than triangle expansion. Repeated endpoint
indices join strips without rasterized connecting triangles; an extra duplicate
after an odd prefix preserves the original winding. Mixed primitive lists and
short strips that would cost more keep indexed triangles. Model/bone matrices,
draw order, geometry, colors, UVs, cache validation and GPU pinning remain live.
The extracted opening/logo replay keeps every original triangle but submits
98,168 mesh indices instead of 134,652 per frame: 36,484 fewer (27.1%). This is
an input-count reduction, not a hardware FPS result. `mesh_strip_draws` and
`mesh_strip_indices_saved` report the selected path and savings per frame.

Weighted vertices now use `GU_WEIGHT_8BIT`. The backend's matrix selectors
need only zero/unit weights, encoded exactly as 0/128. This matches the PSP
byte-weight normalization implemented by
[PPSSPP's vertex decoder](https://github.com/hrydgard/ppsspp/blob/master/GPU/Common/VertexDecoderCommon.cpp#L258).
Each weighted vertex shrinks from 56 to 32 bytes, retaining float positions/UVs
and RGBA8888 colors. Bone uploads and their slot ordering remain unchanged.
The native recorder decodes byte weights using that normalization and compares
transformed triangles to the original per-vertex reference.

Later gameplay in the same capture reports approximately 62 mesh-store failures
per frame with no evictions. Native allocation can fail before the mesh cache
reaches its own 4 MiB budget. On allocation failure the renderer now reclaims
the oldest GPU-idle entry and retries, allowing unused title geometry to leave
the heap. It never evicts an entry still used by queued GE commands; exhaustion
with all entries pinned keeps the uncached fallback. `mesh_memory_evictions`
distinguishes this reclamation from byte-budget/set replacement.

Ordinary and LTO sanitizer checks exercise 432 rigid/skinned strip combinations
with odd/even joins, exact triangle order/winding, animation, source edits,
cache reuse and the 16-bit draw-count boundary. They replay the real opening,
logo, Olimar and ship models, check packed-weight UV refresh and force allocation
failures to verify both idle reclamation and pinned-buffer protection. A native
30,000-index strip now remains batched instead of overflowing its triangle
expansion. The large auxiliary-UV fallback fixture uses 45,000 vertices because
the previous 30,000-vertex case now fits after weight packing. Build with the
usual `make -f Makefile.psp psp-game -j8` and compare the same scenes on PSP.
