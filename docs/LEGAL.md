# Firmware, game data, and release policy

SaturnRecomp's public repository is source-only. It may contain original runtime/recompiler source, generic tests, documentation, configuration templates, and project-created branding.

It must not contain:

- Sega Saturn BIOS or firmware dumps.
- ROMs, CUE/BIN/ISO/CHD images, CD audio, or extracted files.
- Generated/recompiled/decompiled game code or game-specific binary modules.
- Save RAM, SMPC persistence, memory/state dumps, or crash dumps containing guest memory.
- Game screenshots, video, music, voice, textures, models, or other copyrighted assets.
- Private paths, credentials, tokens, or local development workspaces.

The release audit enforces these categories by path and extension. It cannot determine whether an innocently named source file contains copied proprietary code, so contributors must also review content and provenance.

## Local-only workflow

Keep personal inputs in `bios/`, `discs/`, and `games/<local-name>/`. All three locations are ignored. Only `games/_template/game.toml` is public. Any extracted/generated material should remain beneath the ignored local game directory or another ignored output directory.

When discussing compatibility publicly, use product numbers and cryptographic hashes rather than sharing the inputs. Do not provide or request download instructions for copyrighted firmware or games.
