# Blopotron

![Blopotron Demo](blopotron-demo.gif)

**WASD** to move. **IJKL** to shoot. That's it.

Survive waves of enemies in a terminal. 
No tutorials, no cutscenes, no inventory. Survive and score points. 
An homage groundbreaking ROBOTRON 2084 dual-joystick arcade shooter.

---

## What This Is

A single-file C game (`blopotron47.c`) that runs in two modes:

- **SDL mode**: A development artefact - verify positions of sprites etc
- **Text mode**: ANSI terminal rendering via [sprite_bridge.py](https://github.com/clort81/SpriteBridge)

Currently ASCII-Only.  UTF-8 colored sprites are all made, but integration is WIP

## Controls

```
Movement:  W A S D
Shooting:  I J K L
```

Eight directions. Move and shoot independently.  Using SDL to combine keydowns with debouncing for diagonals.
Eventual terminal play will need to have a direction key and seperate 'stop key' (e.g. the '5' on the NumPad).

## Building

```bash
# SDL mode (graphics)
gcc -o blopotron blopotron47.c -lSDL2
./blopotron

# Text mode (terminal)
gcc -o blopotron blopotron47.c -lSDL2
./blopotron -t
```

The `-t` flag spawns `sprite_bridge.py` as a subprocess and renders to stdout. Works over SSH, in tmux, eventually on a real BBS.

## Status

Working. Single-player, endless waves, increasing difficulty. Text-mode rendering is functional but still being optimized for slow links.

Sprite loading and rendering is delayed for after real-world playtesting using placeholder sprites.

## Why

Over 10 years ago i started making ansi-colored utf-8 sprites for the classic game ROBOTRON 2084.  I wanted to make an old school BSD-style terminal game like `hunt`, `robots`, `trek` — small, self-contained games that ran on any terminal. 

## License

FSL-1.1-MIT
Accredation to clort + targeted help from GLM, Qwen and MiMo.
