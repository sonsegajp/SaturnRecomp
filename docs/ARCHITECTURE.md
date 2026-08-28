# Architecture

SaturnRecomp separates copyrighted inputs from reusable platform code. A game declaration points at private local media; the public runtime reads that media at execution time. Nothing extracted from a game needs to be checked into the source tree.

## End-to-end flow

```text
user BIOS + user disc + local game.toml
                  |
                  v
       disc/IP.BIN/config parser
                  |
                  v
       BIOS reset -> IPL -> CD block
                  |
                  v
       dual SH-2 scheduler and buses
          |          |          |
          v          v          v
      SCU/SMPC    VDP1+VDP2   M68K/SCSP/CDDA
          \          |          /
           +---------+---------+
                     v
               SDL2 video/audio/input
```

## Configuration and disc layer

`recompiler/src/game_config.c` parses a deliberately small TOML subset. `[game]` selects the private disc and BIOS. Each `[[module]]` records an ISO path, CPU, compression, load address, entry point, and optional known function entries.

`recompiler/src/disc.c` opens CUE/BIN and ISO media, maps FAD/LBA values to the correct track, distinguishes data and audio sectors, parses IP.BIN, walks ISO9660, and extracts file data on request. The CD block uses the same mapped disc rather than a second ad-hoc reader.

## Boot paths

The normal path loads a user-supplied BIOS and resets the master SH-2 from the ROM vectors. The BIOS initializes the Saturn work area and service tables, runs its animation, reads IP.BIN through the emulated CD block, loads the first-read executable, and transfers control to the game. This is why the normal path auto-boots instead of dropping into an artificial BIOS-menu workflow.

The optional `nobios` path installs HLE service stubs and preloads the first-read file. It is faster for low-level diagnosis but cannot reproduce firmware animation, work-area state, service routines, or timing exactly.

## CPU and scheduler

The runner models two SH-2 CPUs with separate on-chip registers, FRTs, and 4 KiB four-way caches. Address normalization preserves Saturn cache-region semantics. The scheduler interleaves master and slave execution in short quanta so MINIT/SINIT, FRT capture, DMA, and shared-memory rendezvous are observable in the right order.

One frontend frame runs to V-Blank-IN, when visible scanout is complete, matching Ymir's frame boundary. The next call resumes the blanking scanlines and proceeds through the next visible field. That boundary matters: composing after the following V-Blank-OUT handler can pair a VDP1 framebuffer with the wrong VDP2 priority registers.

The SH-2 interpreter is the current execution engine. It has a hand-optimized fast dispatch path checked against the reference path by randomized differential tests. `external/sh2-recomp-core` contains the shared decoder/ISA representation needed by the future AOT emitter.

## Bus, SCU, and CD block

`runner/src/bus.c` maps low/high WRAM, BIOS, VDP1, VDP2, SCSP, SMPC, SCU, CD block, and per-core on-chip address spaces. It also coordinates scanline timing, H/V blank status, SCU interrupt delivery, DMA triggers, timers, and VDP1 field changes.

The SCU implements interrupt routing, DMA, and an instruction-level DSP core. The CD block implements the command/status protocol, filters/selectors, sector delivery, filesystem-backed reads, and CD-DA feed. The implementation is still being expanded against real title behavior.

## Video pipeline

VDP1 walks the command table in Saturn VRAM and rasterizes sprites, distorted sprites, polygons, polylines, and lines into double-buffered sprite and mesh framebuffers. It tracks local/system/user clipping, texture modes, color banks/LUTs, Gouraud data, mesh, shadow, and color-calculation modes.

VDP2 samples normal and rotation backgrounds from VRAM/CRAM, including cell and bitmap layouts, plane maps, scrolling/zoom, pattern-name data, transparency, windows, line/vertical scroll, color offsets, and special color/priority modes. The compositor resolves effective priority per pixel and then performs color calculation/shadow.

For equal numeric priority, VDP1 sprite pixels win ties. Foreground scenery that must cover a priority-2 sprite uses VDP2 special priority to replace the background priority's low bit, producing effective priority 3 only on marked characters/dots. The renderer tests pin both the marked and unmarked cases.

## Audio and input

The sound side contains an MC68000 interpreter, SCSP slot/envelope/LFO/mixer logic, the SCSP DSP, a stereo ring buffer, and CD-DA streaming. SDL's audio clock steers video pacing toward a small target buffer rather than letting two host clocks drift freely.

SMPC implements command/response behavior, region settings, clock/language persistence, and digital-pad reports. Keyboard and SDL GameController inputs are merged. Analog sticks currently act as a digital D-pad; full Saturn 3D Control Pad analog protocol is not implemented.

## Static recompilation status

The public tree has the game-agnostic module schema, disc extraction tools, exhaustive SH-2 decoder, semantic metadata, and runtime ABI. It does not yet contain a complete function-discovery and C-emission pipeline that turns an arbitrary retail title into a standalone native executable. Until that exists, `saturnwin` executes private game code from the user's disc through the SH-2 runtime.

Generated game C, if produced during future local experiments, belongs under an ignored personal game directory and must never be committed or distributed.
