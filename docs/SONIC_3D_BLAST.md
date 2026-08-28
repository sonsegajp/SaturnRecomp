# Sonic 3D Blast setup

This guide configures the U.S. Saturn release of Sonic 3D Blast for SaturnRecomp. The repository does not contain or download the game, its executable, its music, or Saturn firmware. Supply a dump of your own disc and a BIOS dumped from hardware you own.

SaturnRecomp is experimental. Sonic 3D Blast reaches gameplay, but performance and audio/DSP behavior are not yet equivalent to Ymir or original hardware. Compatibility can change as the runtime develops.

## 1. Build SaturnRecomp

Install the prerequisites from the main [game setup guide](GAME_SETUP.md), then build from the repository root:

```powershell
.\build.ps1
```

The commands below assume `runner\saturnwin.exe` and `recompiler\saturnrecomp.exe` were produced successfully.

## 2. Prepare private dumps

Create these ignored local directories:

```powershell
New-Item -ItemType Directory -Force .\bios, .\discs\sonic3d, .\games\sonic3d
```

Put a raw 512 KiB U.S. Saturn BIOS dump at:

```text
bios/saturn_101a_us.bin
```

Dump your Sonic 3D Blast disc as CUE/BIN and place the CUE plus every track file it references in:

```text
discs/sonic3d/
```

This game uses CD audio. A proper multi-track CUE/BIN dump is strongly recommended. A data-only ISO can boot, but it does not contain the separate CD-DA tracks. SaturnRecomp does not decode MP3 files referenced by a CUE; re-dump the disc to raw CUE/BIN rather than using a compressed soundtrack rip.

Do not commit any of these files. The repository ignore rules exclude `bios/`, `discs/`, and personal `games/` directories.

## 3. Inspect your dump

Replace the example CUE name with the actual filename:

```powershell
.\recompiler\saturnrecomp.exe inspect ".\discs\sonic3d\Sonic 3D Blast.cue"
```

For the U.S. release, confirm that the report identifies product number `MK-81062`, first-read address `0x06010000`, and `/0.BIN` as the IPL-loaded program. Also check that every track named by the CUE exists and is readable. If your own disc reports different metadata, use the values printed by `inspect` instead of forcing the U.S. values below.

## 4. Create the local game declaration

Create `games\sonic3d\game.toml` with the following contents. Change only the CUE or BIOS filename if your local names differ:

```toml
[game]
name       = "Sonic 3D Blast"
prefix     = "sonic3d"
product_no = "MK-81062"
disc       = "../../discs/sonic3d/Sonic 3D Blast.cue"
bios       = "../../bios/saturn_101a_us.bin"

[[module]]
name        = "main"
file        = "/0.BIN"
cpu         = "sh2"
compression = "none"
load_addr   = 0x06010000
entry       = 0x06010000
first_read  = true
```

Paths in `game.toml` are resolved relative to that file. Quotes are required around paths containing spaces.

## 5. Validate before launching

```powershell
.\recompiler\saturnrecomp.exe modules .\games\sonic3d\game.toml
```

Do not continue past an unreadable track, product-number mismatch, missing `/0.BIN`, or wrong first-read address. Those errors usually indicate a bad path, incomplete dump, or a release from a different region.

## 6. Launch through the BIOS

```powershell
.\runner\saturnwin.exe .\games\sonic3d\game.toml
```

Do not add `nobios` for normal play. The expected path is:

1. Saturn BIOS startup animation and white flash.
2. Saturn logo.
3. Sega logo.
4. Automatic game boot without navigating the BIOS menu.

If the Saturn CD-player menu appears, close the runner and recheck the BIOS region, CUE track paths, product number, `/0.BIN`, and first-read address. A first-run language or clock screen is normal; complete it once and the ignored `games\sonic3d\smpc.bin` file will preserve the console settings.

## Controls used by Sonic 3D Blast

| Saturn control | Keyboard |
| --- | --- |
| D-pad | Arrow keys or WASD |
| Start | Enter |
| A / B / C | Z / X / C |
| L / R | Q / E |
| Pause runner | Space |
| Advance one field while paused | F |
| Quit | Escape |

SDL-compatible controllers are also supported.

## Title-specific troubleshooting

- **Game is silent after boot:** verify that the CUE references raw CD-audio track files, not MP3. A data-only ISO has no soundtrack tracks.
- **Music stutters or effects sound wrong:** this can be a current CD-DA, SCSP, or DSP limitation rather than a dump failure. See [Compatibility and limitations](COMPATIBILITY.md).
- **Gameplay runs below full speed:** SaturnRecomp is not yet as optimized as mature Saturn emulators. Close diagnostic builds and confirm you launched the current release build.
- **Foreground objects cover Sonic incorrectly, or Sonic draws over foreground scenery:** confirm you built the current source. The renderer includes the VDP2 per-character special-priority and V-Blank boundary fixes used by Sonic 3D Blast.
- **Need to verify the dump independently:** boot the same private CUE/BIN and BIOS in a known-good emulator such as Ymir. Do not copy emulator files into this repository.

Only configuration text belongs in bug reports. Do not attach BIOS data, disc sectors, extracted game files, music, screenshots containing unreleased material, or generated game code.
