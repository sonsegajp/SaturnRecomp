# Setting up a game

SaturnRecomp does not download, bundle, or generate a game for you. A playable local setup combines the public source tree with two private inputs that you supply: a 512 KiB Saturn BIOS dump and a dump of a Saturn disc you own.

## 1. Install the build tools

Use 64-bit Windows with PowerShell 5.1 or newer. Install MSYS2, then install the native MinGW-w64 compiler and SDL2 package from an MSYS2 MINGW64 shell:

```sh
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL2
```

Build from PowerShell:

```powershell
.\build.ps1
```

If MSYS2 is elsewhere, point the script to the directory containing `gcc.exe` and `SDL2.dll`:

```powershell
$env:SATURN_MINGW_BIN = 'D:\msys64\mingw64\bin'
.\build.ps1
```

## 2. Dump your own firmware and disc

- Dump the BIOS from a Saturn you own. The runtime expects the raw 512 KiB ROM image.
- Dump the game from media you own. CUE/BIN is preferred because it preserves the track table and CD audio. A data-only ISO is supported when the title does not need separate audio tracks. CHD is not currently supported.
- Keep both inputs local. The default ignore rules exclude `bios/`, `discs/`, all personal game directories, and common image/audio/binary extensions.

This project does not provide download links, keys, firmware, disc images, or extracted files.

Suggested private layout:

```text
bios/saturn.bin
discs/mygame.cue
discs/mygame (Track 01).bin
games/mygame/game.toml
```

## 3. Inspect the disc

```powershell
.\recompiler\saturnrecomp.exe inspect .\discs\mygame.cue
```

The command prints the IP.BIN product number, region string, first-read address, track layout, and ISO9660 file list. Fix unreadable/missing track errors before writing a config.

## 4. Create the local declaration

```powershell
Copy-Item .\games\_template .\games\mygame -Recurse
```

Edit `games\mygame\game.toml`:

```toml
[game]
name       = "My Saturn Game"
prefix     = "mygame"
product_no = "T-0000G"              # exactly as `inspect` reports; "" disables the check
disc       = "../../discs/mygame.cue"
bios       = "../../bios/saturn.bin"

[[module]]
name        = "main"
file        = "/0GAME.BIN"           # exact ISO9660 path printed by `inspect`
cpu         = "sh2"
compression = "none"
load_addr   = 0x06004000              # use the IP.BIN first-read address
entry       = 0x06004000
first_read  = true
```

At least one module must identify the IPL-loaded executable with `first_read = true`. The file path is case-sensitive and must match the ISO listing. Additional streamed modules are useful for analysis but are not required merely to let the real BIOS boot the disc.

Do not publish this directory: a local config can reveal filenames and addresses from a copyrighted title, and the runtime creates `smpc.bin` beside it for persistent console settings.

## 5. Validate the declaration

```powershell
.\recompiler\saturnrecomp.exe modules .\games\mygame\game.toml
```

This checks the disc header/product number and confirms that every declared module exists in a readable data track. A passing declaration is necessary, not a compatibility guarantee.

## 6. Launch through the real BIOS

```powershell
.\runner\saturnwin.exe .\games\mygame\game.toml
```

The normal path starts at the BIOS reset vector, runs the genuine IPL animation, identifies the configured disc, and auto-boots it. You should not need to navigate the BIOS CD-player menu. If the BIOS menu appears instead, check the disc dump, region, product number, first-read module, and BIOS path.

The optional `nobios` argument bypasses firmware with incomplete HLE stubs. It is a diagnostic mode, not the accuracy path:

```powershell
.\runner\saturnwin.exe .\games\mygame\game.toml nobios
```

## Controls

Keyboard:

| Saturn control | Key |
| --- | --- |
| D-pad | Arrow keys or WASD |
| Start | Enter |
| A / B / C | Z / X / C |
| X / Y / Z | R / T / Y |
| L / R | Q / E |
| Pause runner | Space |
| Advance one field | F |
| Debug view | F1 |
| Quit | Escape |

SDL-compatible controllers are detected automatically. The left stick and D-pad map to the Saturn D-pad; face/shoulder/trigger mappings are documented in `runner/src/window.c`.

## Troubleshooting

- **Build cannot find GCC or SDL2:** set `SATURN_MINGW_BIN` to the native MinGW-w64 `bin` directory. Do not use the MSYS POSIX compiler.
- **BIOS rejected:** confirm that the file is a raw 524,288-byte Saturn BIOS dump and that its region is suitable for the disc.
- **Disc boots to the CD player:** run `inspect` again, verify all CUE track filenames exist, and verify the product/first-read declaration.
- **First-boot language/clock screen:** complete it once. SaturnRecomp saves the console settings in the ignored per-game `smpc.bin`.
- **No music:** prefer CUE/BIN; a data-only ISO cannot contain separate CD-DA tracks.
- **Slow gameplay or imperfect effects:** see [Compatibility and limitations](COMPATIBILITY.md). The runtime is not yet a replacement for a mature emulator.
