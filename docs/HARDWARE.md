# Sega Saturn — Complete Hardware Reference

Target platform reference for **SaturnRecomp**, a static recompiler for Saturn titles
(base game: *NiGHTS into Dreams…*, MK-81020 V1.000, 1996-06-19).

Every figure below is tagged with a confidence marker:

- **[DOC]** — stated in official Sega documentation (Overview / VDP1 / VDP2 / SCU / SMPC / SCSP manuals).
- **[RE]** — established reverse-engineering consensus (Yabause/Mednafen/Kronos, CyberWarriorX, C. MacDonald).
- **[DISC]** — measured directly from our NiGHTS disc image.
- **[PIN]** — needs pinning to the register-level manual or a mednafen cross-check before the
  runner depends on it. **Do not implement a [PIN] item from this document alone.**

---

## 1. System overview

The Saturn is not one machine. It is **eight processors on four isolated buses**, and almost
every hard problem in emulating or recompiling it comes from that topology rather than from
any single chip.

| Role | Part | Clock | Notes |
|---|---|---|---|
| Main CPU ×2 | Hitachi **SH7604** (SH-2), master + slave | 26.8741 MHz (320 mode) / 28.6364 MHz (352 mode) **[DOC/RE]** | 32-bit RISC, 4 KB cache each, big-endian on Saturn |
| System controller | **SCU** | 14.3 MHz DSP (½ CPU) **[DOC]** | DMA ×3 + DSP DMA, interrupt controller, bus bridge |
| Sound CPU | **MC68EC000** | 11.3 MHz **[DOC]** | 16-bit CISC, runs the game's sound driver out of sound RAM |
| Sound chip | **SCSP** (Yamaha YMF292) | DSP 22.6 MHz **[DOC]** | 32 slots PCM/FM @ 44.1 kHz + effects DSP |
| Sprite/polygon GPU | **VDP1** | system clock | Draws textured quads into a framebuffer |
| Background GPU | **VDP2** | system clock | Tile/rotation planes, composites VDP1 output, drives video out |
| System manager | **SMPC** | 4-bit MCU, 1 MHz RTC **[DOC]** | Reset, clock switch, slave enable, controllers, RTC |
| CD subsystem | **SH-1** | 20.0 MHz **[DOC]** | 512 KB buffer RAM + 64 KB BIOS ROM, own world |

> **Clock trap.** The universally quoted "28.6 MHz Saturn" is only true in **352-dot mode**.
> The machine boots in **320-dot mode at 26.8741 MHz**, and `SMPC CKCHG352`/`CKCHG320`
> switch between them at runtime — which *re-clocks the SH-2s, VDP1 and VDP2 together* and
> resets the whole video subsystem. Any cycle model that hardcodes 28.6 MHz is wrong for most
> games most of the time. **[DOC/RE]**

> **Source conflict, resolved.** Some secondary sources (e.g. retroreversing) list the 68EC000
> at 22.6 MHz. That is the **SCSP DSP** clock; the official Overview Manual gives
> "MC68EC000 16-bit CISC 11.3MHz" and "SCSP … DSP22.6MHz". Use **11.3 MHz** for the 68k. **[DOC]**

### Bus topology

Four separated domains — this is the single most important accuracy fact about the machine:

- **CPU bus** — the two SH-2s, WRAM-H (1 MB SDRAM), WRAM-L (1 MB DRAM), boot ROM, SMPC, backup RAM.
- **A-Bus** — external devices: cartridge slot (CS1), CD block (CS2).
- **B-Bus** — internal peripherals: VDP1, VDP2, SCSP. **16-bit wide.**
- **SCU bus / DSP** — arbitration and DMA between all of the above.

The SCU exists so these can run in parallel: "the CPU can access the work area using the
CPU-bus while data is being transferred between the A-bus and B-bus" **[DOC]**. Games are
written to exploit this, so a runner that serializes everything will produce the right pixels
with the wrong timing — and Saturn games are full of code that races DMA against the beam.

**B-Bus is a bottleneck by design.** It is 16-bit. Longword CPU writes to VDP1/VDP2/SCSP are
split into two bus cycles, and VDP2 steals bus slots from the CPU during active display
according to the programmed **cycle patterns** (§6.5). Getting this wrong is the classic cause
of "runs at 2× speed" in naive Saturn emulators. **[RE]**

---

## 2. Memory map

The SH-2 has a 32-bit virtual space but only **A26–A0 reach the bus** — 128 MB physical.
Bits **31–29 select cache behaviour**, not a different device: **[RE]**

| Bits 31–29 | Region | Meaning |
|---|---|---|
| `000` | `0x00000000` | Cached (if CE set in CCR) |
| `001` | `0x20000000` | **Cache-through** |
| `010` | `0x40000000` | Associative purge |
| `011` | `0x60000000` | Direct cache address array access |
| `110` | `0xC0000000` | Direct cache data array access |
| `111` | `0xE0000000` | On-chip peripheral registers |

So `0x06004000` and `0x26004000` are **the same byte of WRAM-H**, differing only in whether the
cache is consulted. Real Saturn code uses both, constantly and deliberately.

> **Recompiler consequence.** Every address the recompiler sees must be normalised to
> `addr & 0x07FFFFFF` for *identity*, while the top bits are preserved for *cache semantics*.
> A static recompiler that treats `0x06xxxxxx` and `0x26xxxxxx` as distinct will fail to
> recognise that a jump table and its consumer refer to the same memory.

### Physical map (bus addresses; add `0x20000000` for the cache-through view)

| Range | Size | Device | CS |
|---|---|---|---|
| `0x00000000–0x000FFFFF` | 512 KB | Boot ROM (IPL) | CS0 |
| `0x00100000–0x0017FFFF` | 128 B (mirrored) | **SMPC** registers | CS0 |
| `0x00180000–0x001FFFFF` | 32 KB (mirrored) | Battery-backed **backup RAM** | CS0 |
| `0x00200000–0x002FFFFF` | 1 MB | **WRAM-L** (DRAM, low work RAM) | CS0 |
| `0x01000000` | — | MINIT (master → slave FRT input capture) | — |
| `0x01800000` | — | SINIT (slave → master FRT input capture) | — |
| `0x02000000–0x03FFFFFF` | 32 MB | **A-Bus CS0/CS1** — cartridge | CS1 |
| `0x04000000–0x04FFFFFF` | — | A-Bus CS2 | CS2 |
| `0x05800000–0x058FFFFF` | — | **CD block** registers (`0x25890000` region) | CS2 |
| `0x05A00000–0x05AFFFFF` | 512 KB | **Sound RAM** (68EC000 + SCSP waveforms) | CS2/B |
| `0x05B00000–0x05BFFFFF` | 4 KB (mirrored) | **SCSP** registers | CS2/B |
| `0x05C00000–0x05C7FFFF` | 512 KB | **VDP1 VRAM** | CS2/B |
| `0x05C80000–0x05CFFFFF` | 256 KB | **VDP1 framebuffer** (the *draw* buffer, as addressed) | CS2/B |
| `0x05D00000–0x05D7FFFF` | 24 B | **VDP1 registers** | CS2/B |
| `0x05E00000–0x05EFFFFF` | 512 KB | **VDP2 VRAM** (banks A0/A1/B0/B1, 128 KB each) | CS2/B |
| `0x05F00000–0x05F7FFFF` | 4 KB | **VDP2 CRAM** (colour RAM) | CS2/B |
| `0x05F80000–0x05FBFFFF` | 512 B | **VDP2 registers** | CS2/B |
| `0x05FE0000–0x05FEFFFF` | 256 B | **SCU** registers | CS2 |
| `0x06000000–0x07FFFFFF` | 1 MB (mirrored ×32) | **WRAM-H** (SDRAM, high work RAM) | CS3 |
| `0xFFFFFE00–0xFFFFFFFF` | 512 B | SH-2 **on-chip peripherals** | — |

**VDP2 internal address map** (as VDP2 itself sees it) **[DOC]**:
`000000–0FFFFF` VRAM (1 MB space, 512 KB populated) · `100000–17FFFF` colour RAM · `180000–1BFFFF` registers.
CRAM and registers are **word/longword access only — byte access is prohibited**.

---

## 3. SH7604 (SH-2)

### 3.1 Core

- 32-bit RISC, **fixed 16-bit instructions**, 16 general registers `R0–R15` (`R15` = stack pointer by convention).
- Control regs: `SR` (T bit, S bit, I3–I0 interrupt mask, M/Q for division), `GBR`, `VBR`.
- System regs: `MACH`, `MACL`, `PR` (procedure return).
- **Big-endian on Saturn.** (Contrast: the Dreamcast's SH-4 is little-endian — our existing
  `crazytaxirecomp` decoder tables port over, but every byte-order assumption inverts.) **[DISC]**
- Hardware **32×32→64 multiplier** and a separate **division unit (DIVU)**, 5-stage pipeline.
- **Delayed branches**: `bra`, `bsr`, `braf`, `bsrf`, `jmp`, `jsr`, `rts`, `bt/s`, `bf/s` all
  execute the following instruction before transferring control.

> **Recompiler consequence.** The delay slot is not an optimisation detail — it is semantics.
> The slot instruction observes pre-branch `PC`/`PR` state, certain instructions are illegal in a
> slot, and PC-relative loads (`mov.w @(d,PC)` / `mov.l @(d,PC)`) inside a slot resolve against
> the **branch's** PC, not the slot's. Our emitter must sequence `<slot effect>` then `<transfer>`.

### 3.2 Cache

4 KB, 4-way set associative, 16-byte lines, unified instruction+data, controlled by `CCR`.
A two-way mode halves the cache and frees 2 KB as directly-addressable fast scratch — some games
use it. **[RE]**

> **This is why `0x20200000`/`0x26000000` exist.** Anything shared between master and slave SH-2
> **must** be accessed cache-through, because the two caches are not coherent. Yabause's docs state
> it flatly: shared data "should be read/written using cache-through addresses … and not cache."
> A runner that ignores the cache will *work* for most code and then break precisely on the
> inter-CPU handshakes. **[RE]**

### 3.3 On-chip peripherals (`0xFFFFFE00–0xFFFFFFFF`) **[PIN]**

Present and used by real games: **INTC** (interrupt controller — `IPRA`/`IPRB`, `VCRxx` vector
registers), **DMAC** (2 channels, independent of SCU DMA), **FRT** (16-bit free-running timer with
input capture — see §3.4), **WDT** (watchdog), **BSC** (bus state controller — wait states, refresh),
**DIVU** (division unit, `DVSR` at `0xFFFFFF00`), **SCI** (serial, unused on Saturn), and the
standby controller.

The **DIVU is asynchronous**: you write divisor and dividend, and the result appears ~39 cycles
later (32÷32) — games start a division and do useful work while it completes. A recompiler that
models division as instantaneous is *functionally* right and *timing* wrong. **[PIN]**

### 3.4 Master / slave

The slave SH-2 is **held disabled at boot** and started via `SMPC SSHON`. Both share the CPU bus,
master has priority. They synchronise through:

1. **Cache-through shared memory** in WRAM-L/WRAM-H, and
2. **FRT input capture**: a write to `0x01000000` (MINIT) pulses the *slave's* FRT capture line,
   and `0x01800000` (SINIT) does the reverse — the standard "poke the other CPU" IPC. **[RE]**

NiGHTS uses the slave heavily (geometry/transform work alongside the SCU DSP), so **the slave is
not optional** for this project.

---

## 4. SCU (System Control Unit)

### 4.1 Interrupt controller — complete table **[DOC]**

From the SCU User's Manual. This table is the backbone of the runner's scheduler.

| Mask bit | Source | Vector | SH-2 level |
|---|---|---|---|
| 0 | V-Blank-IN (VDP2) | `0x40` | 15 (F) |
| 1 | V-Blank-OUT (VDP2) | `0x41` | 14 (E) |
| 2 | H-Blank-IN (VDP2) | `0x42` | 13 (D) |
| 3 | Timer 0 (SCU) | `0x43` | 12 (C) |
| 4 | Timer 1 (SCU) | `0x44` | 11 (B) |
| 5 | DSP End (SCU) | `0x45` | 10 (A) |
| 6 | Sound Request (SCSP) | `0x46` | 9 |
| 7 | System Manager (SMPC) | `0x47` | 8 |
| 8 | PAD Interrupt | `0x48` | 8 |
| 9 | Level-2 DMA End | `0x49` | 6 |
| 10 | Level-1 DMA End | `0x4A` | 6 |
| 11 | Level-0 DMA End | `0x4B` | 5 |
| 12 | DMA-Illegal (SCU) | `0x4C` | 3 |
| 13 | Sprite Draw End (VDP1) | `0x4D` | 2 |
| 16–31 | External interrupts 00–15 (A-Bus, incl. CD block) | `0x50`–`0x5F` | 7, 4, 1 |

Interrupts reach the SH-2s over the **IRL** lines; `SR.I3–I0` masks by level.
SCU-side masking is the **Interrupt Mask register at `0x25FE00A0`**; pending status at `0x25FE00A4`.

### 4.2 DMA **[DOC]**

**Three CPU-usable channels** plus one dedicated to the DSP. (The Overview Manual's "DMA 2ch" is
an early-documentation error; the SCU manual documents levels 0/1/2.)

Priority: **level 2 > level 1 > level 0**. Each channel is a 24-byte register block:

| Offset | Reg | Meaning |
|---|---|---|
| `+00` | `DxR` | Read address, bits [26:0] |
| `+04` | `DxW` | Write address, bits [26:0] |
| `+08` | `DxC` | Transfer byte count |
| `+0C` | `DxAD` | Address add values (read/write stride) |
| `+10` | `DxEN` | Enable / start |
| `+14` | `DxMD` | Mode, update, start factor |

Blocks at `0x25FE0000` (L0), `0x25FE0020` (L1), `0x25FE0040` (L2).
Force-stop `0x25FE0060`; status `0x25FE007C`.

- **Capacity**: level 0 up to **1 MB** (`D0C[19:0]`); levels 1–2 up to **4 KB** (`DxC[11:0]`).
- **Read stride**: 0 (hold) or +4 bytes (the +4 form is CS2 A-Bus space only).
- **Write stride**: 0, 2, 4, 8, 16, 32, 64, 128 bytes (`000b`–`111b`).
  Always effective on B-Bus writes; restricted to `000b`/`010b` for A-Bus CS2.
- **Direct mode**: one transfer per trigger.
  **Indirect mode**: a table of (count, write addr, read addr) triplets in RAM, walked until an
  end-code bit is hit. Indirect DMA is how games push VDP1 command lists and VDP2 tables. **[DOC]**
- **Start factors** include VBlank-IN/OUT, HBlank-IN, timers, sprite-draw-end — i.e. **DMA is
  beam-synchronised**, and games rely on transfers landing in specific scanline windows.

> **Known hardware quirks [RE].** The SCU has documented DMA restrictions (notably around
> WRAM-H/CS3 access and DSP-region transfers, which must be DSP-initiated). Sega's technical
> bulletins list them and games code *around* them. The runner must reproduce the restrictions,
> not "helpfully" make the broken cases work — otherwise workaround code misbehaves.

### 4.3 SCU DSP **[DOC + PIN]**

A genuine second programmable processor, and NiGHTS uses it for geometry.

- **32×32 → 48-bit** multiply-accumulate.
- **Program RAM: 256 words × 32-bit.** **Data RAM: 4 banks × 64 words × 32-bit.**
- Runs at ½ CPU clock — "one step takes about 70 nsec" **[DOC]** (≈14.3 MHz).
- Has its **own DMA instruction** — the DSP can pull from WRAM and the B-Bus itself.
- Cannot address memory directly from instructions: it selects a memory bank + index, and the
  selection takes effect on the **following** instruction (a one-instruction load delay that
  hand-written DSP programs exploit).
- Instruction categories: **Operation, Load-Immediate, DMA, Jump, Loop-Bottom, END.**
- Host ports: program control `0x25FE0080`, program RAM data `0x25FE0084`,
  data RAM address `0x25FE0088`, data RAM data `0x25FE008C`.
- Raises **DSP End** (vector `0x45`).
- Quirk **[RE]**: DSP DMA to program RAM starts on instruction execution but the first write takes
  several cycles; on completion `PC` is set to `TOP` and the prefetched instruction is flushed —
  so `MVI` to `PC` must immediately follow the DMA instruction.

The register file (`RA0`, `WA0`, `PC`, `TOP`, `LOP`, `CT0–CT3`, `RX`, `RY`, `P`, `ACL`, `ALU`,
`MC0–MC3`) is **[RE]** — the official manual documents commands but not the register file by name.
**Full instruction encoding must be pinned from mednafen's `scu_dsp_*.inc` before implementation.**

### 4.4 Timers

Two channels, **screen-synchronised**: Timer 0 fires at a programmable H-position on a programmable
line; Timer 1 offsets from it. Registers `0x25FE0090` (T0 compare), `0x25FE0094` (T1 set data),
`0x25FE0098` (T1 mode). Raster effects hang off these. **[DOC]**

### 4.5 Other SCU registers **[DOC]**

A-Bus interrupt acknowledge `0x25FE00A8`; A-Bus set `0x25FE00B0`; A-Bus refresh `0x25FE00B8`;
SDRAM select `0x25FE00C4`; **SCU version `0x25FE00C8`**.
All SCU register access must use **cache-through** addresses. **[RE]**

---

## 5. VDP1 — sprite / polygon processor

VDP1 has no "3D mode". It draws **textured, arbitrarily-distorted quadrilaterals** into a
framebuffer. All Saturn 3D is quads pushed through this. **[DOC]**

### 5.1 Memory

- **512 KB VRAM** — holds the *command list* and all texture/character data.
- **Two 256 KB framebuffers**, double-buffered. Only the draw buffer is CPU-addressable at
  `0x05C80000`; the other is being scanned out by VDP2.
- Framebuffer pixel formats: **16-bit RGB (RGB555 + transparency bit)** or **8-bit indexed**,
  trading colour for buffer dimensions. **[DOC]**

### 5.2 Registers (`0x05D00000`)

| Off | Reg | R/W | Purpose |
|---|---|---|---|
| `0x00` | `TVMR` | W | TV mode: 8/16bpp, rotation, HDTV/interlace |
| `0x02` | `FBCR` | W | Frame buffer change / erase control |
| `0x04` | `PTMR` | W | **Plot trigger** — kicks off command-list execution |
| `0x06` | `EWDR` | W | Erase/write colour data |
| `0x08` | `EWLR` | W | Erase/write upper-left coordinate |
| `0x0A` | `EWRR` | W | Erase/write lower-right coordinate |
| `0x0C` | `ENDR` | W | Force draw termination |
| `0x10` | `EDSR` | R | Transfer / draw end status |
| `0x12` | `LOPR` | R | Last operation command address |
| `0x14` | `COPR` | R | Current operation command address |
| `0x16` | `MODR` | R | Mode / version status |

> The Yabause wiki lists these shifted by one slot (EDSR at `0x0E`). The layout above is the
> official one and is corroborated by the well-known VDP1 version-ID read at `0x05D00016`
> (= `MODR`). **Use this table.** **[DOC/RE]**

### 5.3 Command table — 32 bytes per command **[DOC]**

Commands live in VRAM as a **linked list**. Each entry uses 30 bytes, **32-byte aligned**:

| Off | Field | Purpose |
|---|---|---|
| `0x00` | `CMDCTRL` | End bit, jump mode, zoom point, character read direction, **command type** |
| `0x02` | `CMDLINK` | Address of next command (÷8) |
| `0x04` | `CMDPMOD` | Draw mode: colour mode, mesh, gouraud, half-transparency, clipping, end-code, MSB-on |
| `0x06` | `CMDCOLR` | Colour bank / lookup-table address |
| `0x08` | `CMDSRCA` | Character (texture) address ÷8 |
| `0x0A` | `CMDSIZE` | Character size (width ÷8, height) |
| `0x0C` | `CMDXA` / `CMDYA` | Vertex A |
| `0x10` | `CMDXB` / `CMDYB` | Vertex B |
| `0x14` | `CMDXC` / `CMDYC` | Vertex C |
| `0x18` | `CMDXD` / `CMDYD` | Vertex D |
| `0x1C` | `CMDGRDA` | Gouraud shading table address ÷8 |

`CMDCTRL` low nibble selects the command:

| Code | Command |
|---|---|
| `0x0` | Normal sprite draw |
| `0x1` | Scaled sprite draw |
| `0x2` / `0x3` | **Distorted sprite draw** (arbitrary quad — this is "3D") |
| `0x4` | Polygon draw |
| `0x5` / `0x7` | Polyline draw |
| `0x6` | Line draw |
| `0x8` | User clipping coordinates |
| `0x9` | System clipping coordinates |
| `0xA` | Local coordinates |

`CMDCTRL` bit 15 = **END** (terminate list); bits 14–12 = jump mode
(JUMP NEXT / ASSIGN / CALL / RETURN, plus SKIP variants) — the command list is effectively a small
program with subroutine calls. **[DOC]**

### 5.4 Colour and shading **[DOC]**

Three texture colour methods:
1. **Colour bank** — 16 / 64 / 128 / 256 colours indexing VDP2's CRAM.
2. **RGB code** — direct 5:5:5, 32,768 colours.
3. **Colour lookup table** — 16 entries, each RGB or bank format.

Effects: **Gouraud shading** (per-vertex colour interpolation via `CMDGRDA`), **half-brightness**,
**half-transparency**, **shadow**, and **mesh** — the Saturn's infamous dither transparency:
*"if the X coordinate value + Y coordinate value is an even number, it will be drawn."*
Mesh is a checkerboard stipple, not a blend, and it composites against VDP2 layers differently from
real translucency. Reproducing NiGHTS' look requires mesh to be exact.

### 5.5 Clipping and performance

- **System clipping**: origin fixed at (0,0), bottom-right programmable.
- **User clipping**: programmable region, inside/outside select, per-command via `CMDPMOD`.
- **Local coordinates**: a command-list-settable origin offset.
- Rated **up to 400,000 pixels per 1/60 s** **[DOC]**; real throughput depends on quad size,
  colour mode and VRAM contention.
- Completion raises **Sprite Draw End** (vector `0x4D`). Games that overrun a frame see the
  interrupt land late — and *that* is their frame-pacing signal.

---

## 6. VDP2 — background processor and video output

VDP2 is tile-based with **no framebuffer of its own** — it generates pixels on the fly, per dot,
compositing its own planes with the VDP1 framebuffer, then drives the video DAC. **[DOC]**

### 6.1 Layers

- **NBG0–NBG3** — four normal scroll planes: independent scaling (1/4× to 256×), line scroll,
  vertical cell scroll, mosaic.
- **RBG0, RBG1** — two rotation planes: full X/Y/Z rotation plus screen-perpendicular rotation,
  driven by rotation parameter tables. Enabling rotation planes costs normal planes.
- **Back screen** and **line colour screen**.

Plane sizes up to **4096×4096**. Formats: **cell** (8×8 cells → character patterns → pages →
planes) or **bitmap** (512/1024 × 256/512). **[DOC]**

### 6.2 Colour

Per-plane: **16, 256, 2048, 32768, or 16.77M** colours. CRAM is 4 KB with three modes selected by
`RAMCTL` bits 13–12: 1024 words 5:5:5, 2048 words 5:5:5, or 1024 words 8:8:8 (modes 2–3 shuffle
address bits). **[RE]**

### 6.3 Compositing

- **Priority**: 3-bit (8 levels) per plane, with per-character and per-dot priority override.
- **Colour calculation**: blending across **up to 4 screens**, 32 ratio steps.
- **Windows**: 2 rectangular + 1 sprite window + line windows.
- **Shadow**, **colour offset** (fades), **mosaic**, **special colour calculation**.

The **sprite priority / colour-calculation lookup** is how VDP1's flat framebuffer gets sorted
*between* VDP2 planes — VDP1 output is not simply "on top". This is the mechanism NiGHTS uses to
place NiGHTS and rings inside a layered sky. **[DOC]**

### 6.4 Resolutions and timing

| | Values |
|---|---|
| Horizontal | 320, 352, 640, 704 |
| Vertical (non-interlaced) | 224, 240, 256 |
| Vertical (interlaced) | 448, 480, 512 |

NTSC ≈ 263 lines/frame @ 60 Hz; PAL ≈ 313 lines @ 50 Hz. **[RE]**
Exact dots-per-line and blanking windows are **[PIN]** — pin against mednafen `vdp2.cpp`.

Status registers: `TVMD` `0x25F80000` (display enable, border mode, interlace, H/V res),
`TVSTAT` `0x25F80004` (VBlank/HBlank/field flags), `VRSIZE` `0x25F80006` (VRAM size + **VDP2
version** in bits 3–0), `HCNT` `0x25F80008`, `VCNT` `0x25F8000A`, `RAMCTL` `0x25F8000E`. **[RE]**
The **full 512-byte VDP2 register map is [PIN]** — transcribe from ST-058 rather than from any
secondary source.

### 6.5 VRAM cycle patterns — the accuracy landmine

VRAM splits into banks **A0, A1, B0, B1** (`RAMCTL` bits 8–9), and registers
**`CYCA0L/U`, `CYCA1L/U`, `CYCB0L/U`, `CYCB1L/U`** (`0x25F80010`–`0x25F8001E`) program, for each
bank, **which access occurs in each of 8 timeslots per cycle** — pattern-name fetch,
character-pattern fetch, vertical cell scroll, CPU access, and so on. **[DOC]**

Two consequences the runner must honour:

1. **A plane can only display if its required fetches were allocated slots.** Games set these to
   match their layer configuration; ignore cycle patterns and layers that should be impossible to
   display will render, while layers that should degrade won't.
2. **Unallocated slots are what the CPU and DMA get.** This is a primary source of B-Bus stall
   timing. Ignoring it makes the emulated machine faster than real hardware.

---

## 7. SCSP (Yamaha YMF292) + MC68EC000

### 7.1 SCSP **[DOC + PIN]**

- **32 slots**, each independently PCM or FM, 44.1 kHz, **8-bit or 16-bit two's-complement** samples.
- Per slot: envelope generator (ADSR), loop (normal / reverse / ping-pong) via `LSA`/`LEA`/`LPCTL`,
  LFO (pitch + amplitude), pitch via `OCT`/`FNS`, level via `TL` (0.3762 dB steps), pan,
  `KYONEX`/`KYONB` key on/off, `SSCTL` source select (sound RAM / noise / silence).
- **Effects DSP** — reverb (hall / stage / room / plate), chorus, delay, filtering, with multiple
  simultaneous sends (e.g. hall on BGM while SFX get tunnel).
- **DMAC ×1**, **timers ×3** (free-running `0x00`–`0xFF`, 7 clock divisions from 1/44100 to
  128/44100 s), **MIDI in/out**, one stereo external digital audio input (the CD-DA path).
- Register space: slots at `0x100000`–`0x1003FF` (32 bytes/slot), common control
  `0x100400`–`0x10042E` (`MVOL`, `RBL`/`RBP` ring buffer, timers, `SCILV`/`MCIPD` interrupts). **[RE]**
- Interrupts to both the 68k (`SCILV`) and the SH-2 side (**Sound Request**, vector `0x46`).

**Full per-slot bit layout and the effects-DSP microcode format are [PIN]** — the Yabause wiki is
incomplete here; pin from the SCSP manual plus mednafen `scsp.inc`.

### 7.2 Sound RAM and the 68EC000

**512 KB** shared between 68k code, sequence data, and PCM waveforms. The 68k runs the game's own
sound driver — there is no sound BIOS doing the work.

> **[DISC] Confirmed for NiGHTS.** `SDDRVS.TSK` (24,544 bytes) begins
> `00 00 A0 00 | 00 00 10 00 | 00 00 19 D6 …` — an M68000 reset vector pair
> (**initial SSP `0x0000A000`, initial PC `0x00001000`**) followed by an exception vector table.
> This is a **raw 68EC000 binary** loaded straight into sound RAM. It is a *second static
> recompilation target*, and the 68K core from `segagenesisrecomp`'s `m68k-recomp-core` applies
> to it directly.

### 7.3 CD-DA

The CD block feeds decoded Redbook audio into the SCSP's external digital input, where it is mixed
and can be routed through the effects DSP. **[DOC]** NiGHTS' 19 CDDA tracks go this way.

---

## 8. SMPC (System Manager & Peripheral Control)

A 4-bit MCU owning everything the SH-2s can't do for themselves. Registers based at `0x00100001`,
byte-wide, on **odd addresses only**: **[RE]**

| Address | Reg |
|---|---|
| `0x20100001–0x2010000D` | `IREG0–IREG6` (command input) |
| `0x2010001F` | `COMREG` (command) |
| `0x20100021–0x2010005F` | `OREG0–OREG31` (output) |
| `0x20100061` | `SR` (status) |
| `0x20100063` | `SF` (status flag — set to 1 when issuing a command) |
| `0x20100075` / `0x20100077` | `PDR1` / `PDR2` (port data) |
| `0x20100079` / `0x2010007B` | `DDR1` / `DDR2` (data direction) |
| `0x2010007D` | `IOSEL` |
| `0x2010007F` | `EXLE` |

**Commands:** `MSHON 0x00`, `SSHON 0x02`, `SSHOFF 0x03`, `SNDON 0x06`, `SNDOFF 0x07`,
`CDON 0x08`, `CDOFF 0x09`, `NETLINKON 0x0A`, `NETLINKOFF 0x0B`, `SYSRES 0x0D`,
**`CKCHG352 0x0E`**, **`CKCHG320 0x0F`**, `INTBACK 0x10`, `SETTIME 0x16`, `SETSMEM 0x17`,
`NMIREQ 0x18`, `RESENAB 0x19`, `RESDISA 0x1A`. **[RE]**

- `SSHON` starts the **slave SH-2** — until then it is halted.
- `CKCHG320/352` re-clocks the system (§1) and **resets VDP1/VDP2 and the SH-2s**.
- `INTBACK` is the workhorse: returns RTC, system status and **all peripheral data**, raising the
  System Manager interrupt (vector `0x47`).
- RTC is battery-backed, 1 MHz, tracking year / month / day / weekday / hour / minute / second. **[DOC]**

**Controller data** arrives from `INTBACK` as a peripheral-ID byte followed by data bytes. The
standard digital pad is **13 buttons** (D-pad ×4, A/B/C, X/Y/Z, L/R, Start) in 2 bytes. The
**3D Control Pad** (bundled with NiGHTS) reports an analogue ID with extra nibbles: X axis, Y axis
and L/R analogue triggers, each `0x00`–`0xFF`, centre `0x80`. Also supported: Mega Drive 3- and
6-button pads, the 4-player adapter, and mice. **[RE]**

> **[DISC]** Our disc's IP.BIN peripheral field reads **`EJ`**. `J` is the standard digital control
> pad. `E` is *most likely* the analogue/3D pad, consistent with the NiGHTS bundle, but I have not
> confirmed `E` against an authoritative peripheral-code list. **[PIN]**

---

## 9. CD block

An **SH-1 at 20 MHz** with **512 KB buffer RAM** and a **64 KB BIOS ROM**, behind the A-Bus. The
SH-2 never touches the drive; it sends commands through a small register window. **[DOC]**

Registers at `0x25890000`: **[RE]**

| Address | Reg |
|---|---|
| `0x25890008` | `DTR` (data transfer) / `HIRQ` (interrupt status) |
| `0x2589000C` | `HIRQ` mask |
| `0x25890018` | `CR1` |
| `0x2589001C` | `CR2` |
| `0x25890020` | `CR3` |
| `0x25890024` | `CR4` |
| `0x25890028` | `MPEGRGB` |

On reset `CR1–CR4` spell **"CDBLOCK"** in ASCII. Command status in the `CR1` high byte:
`0x80` = wait, `0xFF` = reject.

**Model:** raw 2352-byte sectors land in a buffer pool (**202 sectors internally, 200 visible to
the SH-2**) and are routed by a **filter → selector → partition** chain keyed on file number,
channel number, submode and coding info. Games configure filters and then consume partitions.

- ~41 CD commands (`0x00`–`0x75`), 18 MPEG commands (`0x90`–`0xAF`), authentication `0xE0`–`0xE2`.
- Standard init: abort transfer `0x75` → CD init `0x04` → end transfer `0x06` → reset selectors `0x48`.
- **Only the first 16 sectors are readable before authentication.** `0xE0` performs the ring check;
  disc type returns `0x02` (data), `0x04` (original Saturn disc), `0x01` (audio).
- Transfer 150 KB/s single / **300 KB/s double speed**; access time ≤ 500 ms at 2×. **[DOC]**

> **Runner note.** We authenticate unconditionally (we are not emulating the physical ring) but must
> still return `0x04` and honour the full sequence, because game code *checks*.

---

## 10. Boot process and disc format

1. SMPC releases reset; the master SH-2 begins at the boot ROM reset vector, slave stays halted.
2. IPL initialises hardware, then reads **sector 0** of the disc — the **IP.BIN** header.
3. IPL validates the security code, loads the **Initial Program**, and the IP loads the
   **1st-read file** to its declared address and jumps there.

### IP.BIN layout (2048-byte user data of sector 0)

| Off | Size | Field | **NiGHTS value [DISC]** |
|---|---|---|---|
| `0x00` | 16 | Hardware ID | `SEGA SEGASATURN ` |
| `0x10` | 16 | Maker ID | `SEGA ENTERPRISES` |
| `0x20` | 10 | Product number | `MK-81020  ` |
| `0x2A` | 6 | Version | `V1.000` |
| `0x30` | 8 | Release date | `19960619` |
| `0x38` | 8 | Device info | `CD-1/1  ` |
| `0x40` | 10 | Area symbols | `JTU` (Japan / Asia / North America) |
| `0x50` | 16 | Peripherals | `EJ` |
| `0x60` | 112 | Title | `NiGHTS` |
| `0xE0` | 4 | IP size | `0x00001800` (6 KB) |
| `0xE8` | 4 | Master stack | `0x00000000` |
| `0xEC` | 4 | Slave stack | `0x00000000` |
| `0xF0` | 4 | **1st-read address** | **`0x06004000`** (WRAM-H + 0x4000) |
| `0xF4` | 4 | 1st-read size | `0` (= whole file) |
| `0x100` | … | IP program (SH-2 code) | begins `4F 22` = `sts.l pr,@-r15` |

---

## 11. What this means for SaturnRecomp

### 11.1 Where the Genesis architecture maps cleanly

- **Evidence-driven discovery** (`game.toml` + static decode + runtime evidence) transfers directly.
- **Emit C per function against a shared CPU-state struct** transfers directly.
- **Interpreter fallback for unresolved indirect targets** transfers directly — and matters *more*
  here than on Genesis.
- **Recomp-vs-interpreter co-simulation** as the validation oracle transfers directly, and is the
  only realistic path to "100% accurate" on this machine.
- The **68000 core** from `m68k-recomp-core` is reusable as-is for `SDDRVS.TSK`.

### 11.2 Where Saturn breaks the Genesis model — and what we do about it

| Problem | Genesis | Saturn | Plan |
|---|---|---|---|
| **Code location** | ROM at `0x000000`, always present | Code is **loaded from CD into RAM**, per level | Recompile **per module**; dispatch-table swap on load |
| **CPU count** | 1× 68K (+Z80) | **2× SH-2** sharing a bus, + 68k, + SCU DSP | Cooperative fiber scheduler; cache-through-aware shared memory |
| **Cache** | none | 4 KB non-coherent per SH-2 | Model the cache; normalise `0x0x`/`0x2x` addresses for identity |
| **Address aliasing** | none | Same RAM at `0x06…`/`0x26…`; WRAM-H mirrored ×32 | Canonical-address pass in the analyser |
| **Delay slots** | none | Every branch | Explicit slot sequencing in the emitter |
| **Graphics** | one VDP | VDP1 command-list GPU + VDP2 compositor + cycle patterns | Two clean-room cores; VDP1 rasteriser first |
| **Timing** | fixed clock | Clock **changes at runtime** (320/352) | Clock change must re-time the whole scheduler |

### 11.3 Per-game findings

Deliberately **not** in this document. Anything specific to one title —
module inventory, load addresses, disc quirks — lives with that game:

    games/<name>/NOTES.md      findings
    games/<name>/game.toml     the declaration the recompiler actually reads

This file is the platform reference and stays game-agnostic.

### 11.4 Required first-party components (nothing cloned)

1. **SH-2 decoder** — port our `crazytaxirecomp` SH-4 tables restricted to the SH-2 subset,
   big-endian, validated 0-wrong against capstone `CS_MODE_SH2 | CS_MODE_BIG_ENDIAN` across all
   65,536 opcodes.
2. **SH-2 interpreter** — the co-simulation oracle. Must exist before the emitter is trusted.
3. **Disc layer** — CUE/BIN, MODE1/2352 sector extraction, ISO9660 walk, CDDA tracks.
   *(Recon already working in `tools/recon_iso.py`; the shipped version is C.)*
4. **PRS decompressor** — our own; needed to even see 14 of the code modules.
5. **Runner** — SCU (interrupts / DMA / timers), VDP1 rasteriser, VDP2 compositor, SCSP + 68k,
   SMPC, CD block, cache model, fiber scheduler.

### 11.5 Accuracy tiers

Not everything needs cycle accuracy, and pretending otherwise stalls the project. Proposed contract:

- **Tier A — must be exact.** SH-2 instruction semantics (incl. delay slots, `T`/`M`/`Q` bits,
  `MAC` saturation), memory-map decode and aliasing, SCU interrupt levels/vectors/masking, VDP1
  command-list semantics and rasterisation rules (mesh, gouraud, clipping), VDP2 priority and
  colour calculation, SMPC command semantics, CD block command/partition semantics.
- **Tier B — must be *plausible and stable*, refined by cosim.** Instruction cycle counts, VDP2
  cycle-pattern bus stalls, DMA duration, VDP1 draw duration, DIVU latency.
- **Tier C — behavioural only.** CD seek times, SCSP DSP effect coefficients, MPEG card (absent).

Everything in Tier A is validated by **recomp-vs-interpreter co-simulation**; Tier B is tuned until
NiGHTS' own frame pacing (which self-measures via Sprite Draw End) reports the same numbers as
hardware.

---

## 12. Sources

**Official Sega documentation**
- Saturn Overview Manual; VDP1 User's Manual (ST-013-R3); VDP2 User's Manual (ST-058-R2);
  SCU User's Manual (3rd ed.); SMPC User's Manual; SCSP User's Manual —
  [antime.kapsi.fi/sega/docs.html](https://antime.kapsi.fi/sega/docs.html) ·
  [docs.exodusemulator.com SSDDV25](https://docs.exodusemulator.com/Archives/SSDDV25/segahtml/hard/intr/sakuin.htm)
- [SCU User's Manual, full text](https://archive.org/details/SCU_Users_Manual_Third_Version_1994_Sega)
- [Saturn Technical Bulletins](https://antime.kapsi.fi/sega/files/Sattechs.pdf) — DMA restrictions and errata

**Reverse-engineering references**
- [Yabause hardware wiki](https://wiki.yabause.org/index.php5?title=Main_Page) — SH-2, SCU, SMPC, SCSP, VDP1, VDP2, CD block
- [Mednafen / Beetle Saturn source](https://github.com/libretro/beetle-saturn-libretro/tree/master/mednafen/ss) —
  highest-accuracy reference implementation; **authority for every [PIN] item**
- Charles MacDonald, *Sega Saturn hardware notes* (2002) — cgfm2.emuviews.com
- [Copetti, *Sega Saturn Architecture*](https://www.copetti.org/writings/consoles/sega-saturn/)
- [Jo Engine `sega_saturn.h`](https://jo-engine.org/doxygen/sega__saturn_8h.html) — consolidated register map
- PRS format: [dlang-prs](https://github.com/Sewer56/dlang-prs) · [prsutil](https://github.com/essen/prsutil)

**Measured**
- `NiGHTS Into Dreams.bin/.cue` — MK-81020 V1.000, 233,632 sectors, 21 tracks, 191 files.
