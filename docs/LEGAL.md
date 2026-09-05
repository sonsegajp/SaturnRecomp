# Firmware, game data, and release policy

SaturnRecomp's public repository distributes source and documentation. It may contain original runtime/recompiler source, generic tests, documentation, configuration templates, project-created branding, and the launcher UI screenshots listed below.

It must not contain:

- Sega Saturn BIOS or firmware dumps.
- ROMs, CUE/BIN/ISO/CHD images, CD audio, or extracted files.
- Generated/recompiled/decompiled game code or game-specific binary modules.
- Save RAM, SMPC persistence, memory/state dumps, or crash dumps containing guest memory.
- In-game screenshots, video, music, voice, textures, models, or standalone game assets.
- Private paths, credentials, tokens, or local development workspaces.

The release audit enforces these categories by path and extension. It cannot determine whether an innocently named source file contains copied proprietary code, so contributors must also review content and provenance.

## Launcher documentation screenshots

These launcher UI screenshots are explicitly approved for publication in this repository:

- `docs/images/launcher-library.png`
- `docs/images/launcher-compact.png`
- `docs/images/launcher-controls.png`

Cover art visible in these pictures belongs to its respective owners. The pictures illustrate the launcher interface; they do not establish game compatibility or supply playable game data. This documentation exception does not permit publishing firmware, disc images, extracted game assets, or standalone cover-art collections.

## Local-only workflow

Keep personal inputs in `bios/`, `discs/`, and `games/<local-name>/`. All three locations are ignored. Only `games/_template/game.toml` is public. Any extracted/generated material should remain beneath the ignored local game directory or another ignored output directory.

When discussing compatibility publicly, use product numbers and cryptographic hashes rather than sharing the inputs. Do not provide or request download instructions for copyrighted firmware or games.
