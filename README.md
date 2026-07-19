# Blopotron: Terminal Arcade Game

![Blopotron Demo](blopotron-demo.gif)

**WASD** to move. **IJKL** to shoot.  Beat your old high score.

Survive waves of enemies in a terminal, rescuing humans for points. 

An homage to the incomparable dual-joystick arcade shooter; ![ROBOTRON 2084](https://en.wikipedia.org/wiki/Robotron:_2084).

AI: No tutorials, no cutscenes, no inventory. Survive and score points. 

---

## What This Be?

A single-file C game Currently hitting the scales at [[44kB]]: ```44136 Jul 18 20:06 blopotron``` (`blopotron.c`) that runs in two modes:

- **SDL mode**: A development artefact - verify positions of sprites etc
- **Text mode**: ANSI terminal rendering via [sprite_bridge.py](https://github.com/clort81/SpriteBridge)

Text-sprites are defined strings in a sprites.h header (or right in the .c).

Uses a SpriteBridge backend renderer - emitting commands to stdout (utf-8) which gets redirected to stdin of the sprite_bridge.py process.  This is mostly done as an excercise in sysadmin-nearness, showing how text as a bridge format makes for nice easy inspectability, routing, data-mangling.

WIP: [[NOT FINISHED]] [[UNDER CONSTRUCTION]] Currently working on utf8 sprite handling.

For a more current-day and normie remake of Robotron 2084, see ![Robotron2048Gym](https://github.com/stridera/robotron2084gym)

---

## Controls

```
Insert Coin: 1
Movement:  W A S D
Shooting:  I J K L
```

Eight directions. Move and shoot independently.  Using SDL to combine keydowns with debouncing for diagonals.
Eventual terminal play will need to have a direction key and seperate 'stop key' (e.g. the '5' on the NumPad).

## Building

```bash
# Debug 
gcc -g -Wall -O0 -o blopotron-dbg blopotron.c -lSDL2

# Text mode (terminal)
gcc -O2 -o blopotron blopotron.c -lSDL2
```

Running ./blopotron with the `-t` flag spawns `sprite_bridge.py` as a subprocess and renders to stdout. Works over SSH, in tmux, eventually on a real BBS with suitable player-input handling.

## Novel Concept: "Sub-Character Animation"

**Sub-Character animation with Meta-Sprites:**
A "Meta-Sprite" is just a collection of sprites (containing ansi-colored utf-8 art assets) with variants I call 'frames', each tagged with semantic metadata. The important ones to introduce are:
1. **Facing (0-8)**: Which of the 8 compass directions the sprite is oriented toward. 0:None, 1: North, 2:NE ...
2. **Halfstep (0-8)**: Whether this specific frame is designed to be rendered at an integer grid coordinate (0), or offset by 0.5 in a specific direction (1-8) to simulate sub-cell movement.

**The Capability It Buys Us:**
This enables **sub-cell resolution animation**. In traditional terminal games, an entity's Y-coordinate is an integer. Moving from row 4 to row 5 is an instantaneous, jerky "teleport." By introducing a "halfstep" frame, the game engine can say: *"The entity's logical Y-coordinate is 4.45. Therefore, render the halfstep frame."*

That halfstep frame is pre-drawn using half-block characters (like `▀` or `▄`) so that, when drawn at row 4, it visually appears to occupy the space *between* row 4 and row 5. This allows entities to glide smoothly across the screen; their visual representation interpolating seamlessly between grid cells.

EXAMPLE: Enforcer with a second frame used when game world position resolves to 'in between' two terminal rows.

![Enforcer Halfstep](enforcer-halfstep.png)

**Why It’s a Total Novelty in ANSI Terminal Games:**
Historically, ANSI/ASCII games (like `NetHack`, `Rogue`, or classic BBS doors) are strictly **cell-bound**. An entity is either at `(X, Y)` or it isn't. There is no "in-between."

By carefully drawing sprites to look identical or similar when shifted up/down (or left/right) We pre-bake the sub-cell offsets and directional variants into the asset itself. The game loop doesn't need to do any complex ASCII manipulation; it just evaluates `facing` and `halfstep`, looks up the correct pre-rendered frame, and sends a single `DRAW` command. It brings the fluidity of vector or pixel-art sub-pixel rendering to the terminal, with zero runtime overhead. It is, effectively, **sub-pixel rendering for ASCII**.

![Blopotron Spritesheet](blopotron_spritesheet.png)

## Why Who What?

Over 10 years ago i started making ansi-colored utf-8 sprites for the classic game ROBOTRON 2084.  I wanted to make an old school BSD-style terminal game like `hunt`, `robots`, `trek` — small, self-contained games that ran on any terminal. I wanted particularly to implement the idea of sub-character sprite animation in terminal arcade games.  

This repository is a small shard broken off of thousands of hours of R&D (play) towards that goal.

## License

FSL-1.1-MIT
Accredation to clort + targeted help from GLM, Qwen and MiMo.
