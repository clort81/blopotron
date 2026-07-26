# sprites.h — Format & Integration Guide

This document describes the format of `sprites.h` and how to integrate real sprite and animation-frame data into `bth.c`. The current build ships with 1×1 stub frames for all 12 entity types; once a tool-generated `sprites.h` replaces those stubs, the game automatically switches from colored-rectangle rendering to per-cell text-art rendering with per-cell foreground and background color.

---

## 1. Where sprites fit in the architecture

```
+-------------------+    +---------------------+    +------------------+
| .ans text-art     | -> | ANSI-to-header tool | -> | sprites.h        |
| (one file per     |    | (build_sprites)     |    |  - row hex arrays|
|  character)       |    |                     |    |  - row pointers  |
+-------------------+    +---------------------+    |  - SpriteFrame[] |
                                                    |  - SpriteSet     |
                                                    +------------------+
                                                              |
                                                              v
                                                    +------------------+
                                                    | bth.c            |
                                                    |  #include        |
                                                    |   "sprites.h"    |
                                                    |  render_sprite_  |
                                                    |   frame()        |
                                                    +------------------+
```

The game source (`bth.c`) defines the **types** (`SpriteFrame`, `FacingInfo`, `SpriteSet`) and the **render path** (`render_sprite_frame()`, `get_current_frame()`). The header file (`sprites.h`) supplies the **data** — the actual cell bytes, row pointers, and frame tables. Keeping data and code separate means you can regenerate `sprites.h` whenever art changes without touching game logic.

---

## 2. Core types (already defined in bth.c)

These types are defined in `bth.c`. **Do not redefine them in `sprites.h`.**

```c
/* One frame of text-art sprite data. */
typedef struct {
    const uint8_t* const* rows;  /* h row pointers */
    int w, h;                    /* dimensions in text cells */
    int ox, oy;                  /* placement offset in text cells (hotspot) */
} SpriteFrame;

/* Per-facing animation descriptor. */
typedef struct {
    int start;        /* index into frames[] where this facing begins */
    int count;        /* number of frames in this facing's walk cycle */
    int step_period;  /* game ticks between frame advances */
} FacingInfo;

/* Top-level sprite definition: one per entity type. */
typedef struct {
    const char* name;
    int total_frames;
    const SpriteFrame* frames;
    FacingInfo facing[4];  /* N=0, S=1, E=2, W=3 -- matches Direction enum */
} SpriteSet;
```

---

## 3. Cell format (10 bytes per cell)

Every text cell in a frame is exactly 10 bytes:

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 4 | `g[4]` | UTF-8 glyph, null-terminated (1–3 bytes + zero padding) |
| 4 | 1 | `fr` | Foreground red (0–255) |
| 5 | 1 | `fg` | Foreground green (0–255) |
| 6 | 1 | `fb` | Foreground blue (0–255) |
| 7 | 1 | `br` | Background red (0–255) |
| 8 | 1 | `bg` | Background green (0–255) |
| 9 | 1 | `bb` | Background blue (0–255) |

**Example** — a white-on-black `▀` (top-half block):

```c
0xe2, 0x96, 0x80, 0x00,   /* ▀ as UTF-8 (3 bytes), then null */
0xff,                     /* fr = 255 */
0xff,                     /* fg = 255 */
0xff,                     /* fb = 255 */
0x00,                     /* br = 0 */
0x00,                     /* bg = 0 */
0x00,                     /* bb = 0 */
```

Cells are packed left-to-right within a row, top-to-bottom within a frame.

---

## 4. Naming conventions

The tool generates identifiers using this scheme (replace `<Sprite>` with the entity name like `Player`, `Grunt`, `Hulk`, etc.):

| Element | Pattern | Example |
|---|---|---|
| Row data array | `<Sprite>_<Facing>f<frame>_r<row>` | `Player_Nf0_r0` |
| Row pointer array | `<Sprite>_<Facing>f<frame>_rows` | `Player_Nf0_rows` |
| Single frame struct | `<Sprite>_<Facing>f<frame>` | `Player_Nf0` |
| Flat frames array | `<Sprite>_frames` | `Player_frames` |
| SpriteSet | `sprite_<name>_set` | `sprite_player_set` |

**Facing codes** in names: `N` (north/up), `S` (south/down), `E` (east/right), `W` (west/left). These map to indices 0, 1, 2, 3 in `FacingInfo facing[4]` — same as the `Direction` enum in `bth.c`.

**The SpriteSet name must match what `bth.c` expects.** The game already references these symbols:

```
sprite_player_set      sprite_grunt_set      sprite_quark_set
sprite_hulk_set        sprite_brain_set      sprite_spheroid_set
sprite_enforcer_set    sprite_human_set      sprite_laser_set
sprite_terror_set      sprite_electrode_set  sprite_cruise_set
```

If the tool produces a different name (e.g. `sprite_Grunts_set` with a capital G), either fix the tool's naming or add aliases at the bottom of `sprites.h`:

```c
static const SpriteSet* const sprite_grunt_set = &sprite_Grunts_set;
```

---

## 5. Header file structure

`sprites.h` must contain, in order:

```c
#ifndef SPRITES_H
#define SPRITES_H

/* 1. Per-row hex arrays */
static const uint8_t Player_Nf0_r0[30] = { 0xe2, 0x96, 0x88, 0x00, 0xff, ... };
static const uint8_t Player_Nf0_r1[30] = { ... };
/* ... one array per (sprite, facing, frame, row) ... */

/* 2. Row pointer arrays */
static const uint8_t* const Player_Nf0_rows[] = {
    Player_Nf0_r0, Player_Nf0_r1, /* ... */
};

/* 3. SpriteFrame per frame */
static const SpriteFrame Player_Nf0 = {
    Player_Nf0_rows, 3, 2, 1, 1   /* rows, w=3, h=2, ox=1, oy=1 */
};

/* 4. Flat frames[] array (all facings concatenated, N then S then E then W) */
static const SpriteFrame Player_frames[] = {
    /* N facing */  Player_Nf0, Player_Nf1, Player_Nf2,
    /* S facing */  Player_Sf0, Player_Sf1, Player_Sf2,
    /* E facing */  Player_Ef0, Player_Ef1,
    /* W facing */  Player_Wf0, Player_Wf1,
};

/* 5. SpriteSet */
static const SpriteSet sprite_player_set = {
    "Player",
    10,                /* total_frames */
    Player_frames,
    {                  /* facing[4]: N, S, E, W */
        { 0, 3, 6 },   /* N: start=0, count=3, step_period=6 */
        { 3, 3, 6 },   /* S: start=3, count=3, step_period=6 */
        { 6, 2, 4 },   /* E: start=6, count=2, step_period=4 */
        { 8, 2, 4 }    /* W: start=8, count=2, step_period=4 */
    }
};

/* ... repeat for all 12 entity types ... */

#endif /* SPRITES_H */
```

---

## 6. Animation parameters (`step_period`)

`step_period` is the number of game ticks (frames at 60fps) between frame advances. Lower = faster animation. The game's animation advance logic in `update_entities()` and `update_player()` increments `anim_counter` each tick, and rolls over to the next frame when it reaches `step_period`:

```c
if (++e->anim_counter >= fi->step_period) {
    e->anim_counter = 0;
    e->anim_frame = (e->anim_frame + 1) % fi->count;
}
```

**Recommended values** (also listed in the existing `sprites.h` template):

| Entity | step_period | Rationale |
|---|---|---|
| Player | 6 | Smooth walk |
| Grunt | 8 | Shambling |
| Hulk | 4 | Lumbering but fast |
| Spheroid | 2 | Constantly morphing |
| Enforcer | 2 | Hovering |
| Brain | 4 | Pulsating |
| Human | 4 | Gentle wandering |
| Quark | 4 | Erratic |
| Laser | 1 | Static (no animation needed, count=1) |
| Terror | 1 | Rapid flicker |
| Cruise | 1 | Static |
| Electrode | 1 | Static |

A `count` of 1 with any `step_period` produces a static sprite (the same frame is "advanced to" forever).

---

## 7. Hotspot (`ox`, `oy`)

The `ox`, `oy` fields on each `SpriteFrame` control where on the sprite the entity's world position is anchored. They are in **text-cell units**, not pixels.

For a 3×3 player sprite centered on its position, use `ox=1, oy=1`. For a laser projectile that spawns at the player's muzzle and travels rightward, use `ox=0, oy=0` so the sprite's top-left aligns with the spawn point.

The render code currently uses the entity's top-left text-cell coordinate directly:

```c
int tx = (int)((float)e->wx / COORD_SCALE * g_term_cols / SCREEN_WIDTH);
int ty = (int)((float)e->wy / COORD_SCALE * g_term_rows / SCREEN_HEIGHT);
render_sprite_frame(sf, tx, ty);
```

If you want hotspot offsetting to take effect, change those lines to:

```c
int tx = (int)((float)e->wx / COORD_SCALE * g_term_cols / SCREEN_WIDTH) - sf->ox;
int ty = (int)((float)e->wy / COORD_SCALE * g_term_rows / SCREEN_HEIGHT) - sf->oy;
```

For now, design your sprite art so that the entity's anchor point sits at the top-left of the frame (i.e. `ox=0, oy=0`), which matches the existing collision bounds (entity position = top-left of hitbox).

---

## 8. Integration procedure

Follow these steps to replace stub sprites with real art:

### Step 1: Generate `sprites.h`

Run the ANSI-to-header tool on your `.ans` art files:

```bash
./build_sprites grunts.ans brain.ans electrode.ans ... > sprites.h
```

Or use a Makefile rule (recommended):

```makefile
SPRITE_SRC = grunts.ans brain.ans electrode.ans quark.ans hulk.ans \
             spheroid.ans enforcer.ans human.ans laser.ans terror.ans \
             cruise.ans player.ans

sprites.h: $(SPRITE_SRC)
	cat $^ > $@.tmp
	./build_sprites $@.tmp $@
	rm $@.tmp
```

### Step 2: Place `sprites.h` next to `bth.c`

```bash
cp sprites.h /path/to/game/
```

### Step 3: Include it in `bth.c`

Add this line near the top of `bth.c`, after the system includes:

```c
#include "sprites.h"
```

### Step 4: Remove the stub definitions from `bth.c`

Delete (or comment out) these blocks, which are now superseded by the data in `sprites.h`:

- `static const uint8_t stub_cell_blank[10] = { ... };`
- `static const uint8_t* const stub_frame_rows[1] = { stub_cell_blank };`
- `static const SpriteFrame stub_frames[] = { ... };`
- All 12 `static const SpriteSet sprite_*_set = { ... };` definitions

The `SpriteSet` symbols (`sprite_player_set`, `sprite_grunt_set`, etc.) will now resolve to the ones declared in `sprites.h`.

### Step 5: Update `sprite_pixel_w[]` and `sprite_pixel_h[]`

These arrays still drive **collision bounds**, **clamping**, and **safe-spawn checks**. They are in **screen-pixel units**. Update them to match your real sprite dimensions:

```c
/* Index: Player, Grunt, Quark, Hulk, Brain, Spheroid, Enforcer, Human, Laser, Terror, Electrode, Cruise */
static const int sprite_pixel_w[NUM_ENTITY_TYPES] = {
    20, 18, 20, 24, 22, 18, 20, 10, 8, 10, 16, 4
};
static const int sprite_pixel_h[NUM_ENTITY_TYPES] = {
    20, 18, 20, 24, 22, 18, 20, 22, 8, 10, 16, 4
};
```

The conversion is: **screen pixels = text-cell width × 4** (approximately — the screen-to-text-cell mapping uses `g_term_cols / SCREEN_WIDTH` which works out to roughly 4 pixels per text cell at default terminal sizes). For a 3×3 text-cell sprite, set width and height to ~12 pixels each.

You do **not** need to change `PLAYER_SPRITE_W` / `PLAYER_SPRITE_H` / `LASER_SPRITE_W` / `LASER_SPRITE_H` macros — they are only used for spawn-position offset math and remain in screen-pixel units.

### Step 6: Verify auto-toggle works

The render path in `render_all()` auto-detects real sprites by checking `anim->frames[0].w > 1`. Stubs are 1×1, so any frame wider than 1 cell triggers sprite rendering.

To confirm: build and run the game. If your sprites appear as text art with their embedded colors, the toggle worked. If they still appear as colored rectangles, the `frames[0].w` check is failing — verify your first frame's `w` field is greater than 1.

---

## 9. Rendering pipeline (what the game does)

For each entity, every frame, `render_all()` does this:

```
1. Is entity active?                       -> if no, skip
2. Does anim exist and frames[0].w > 1?    -> if no, use colored rect fallback
3. Lookup frame: get_current_frame(anim, facing_dir, anim_frame)
4. Convert world coords to text coords:    tx = wx / COORD_SCALE * cols / SCREEN_WIDTH
                                            ty = wy / COORD_SCALE * rows / SCREEN_HEIGHT
5. render_sprite_frame(sf, tx, ty):
   For each (row, col) in the frame:
     - Compute target text buffer index
     - Skip if off-screen
     - memcpy 4 bytes from cell to text_buffer.glyph
     - Copy fr, fg, fb to text_buffer.r/g/b
     - Copy br, bg, bb to text_buffer.br/bg/bb
6. flush_text_buffer() emits the entire buffer with ANSI escape sequences
```

The player has an additional 3-way branch: invulnerability flash (skip on "off" frames), death (red rect override), otherwise sprite or stub.

---

## 10. Animation advance (what the game does)

For entities, in `update_entities()`:

```c
if (e->anim) {
    const FacingInfo* fi = &e->anim->facing[e->facing_dir];
    if (++e->anim_counter >= fi->step_period) {
        e->anim_counter = 0;
        e->anim_frame = (e->anim_frame + 1) % fi->count;
    }
}
```

For the player, in `update_player()` (only animates while moving):

```c
if (p->anim && (g_autorun_vx != 0 || g_autorun_vy != 0)) {
    const FacingInfo* fi = &p->anim->facing[p->facing_dir];
    if (++p->anim_counter >= fi->step_period) {
        p->anim_counter = 0;
        p->anim_frame = (p->anim_frame + 1) % fi->count;
    }
}
```

Facing direction is updated from velocity:

```c
if (g_autorun_vx != 0 || g_autorun_vy != 0) {
    p->facing_dir = get_dir_from_vel(p->vx, p->vy);
}
```

Entities get their `facing_dir` set in `update_entities()` based on `e->vx, e->vy`.

---

## 11. Worked example: minimal Player sprite

This is the smallest valid `sprites.h` that replaces the player stub with a real 3×2 sprite, single frame, single facing:

```c
#ifndef SPRITES_H
#define SPRITES_H

/* Row 0: 3 cells, each 10 bytes = 30 bytes */
static const uint8_t Player_Nf0_r0[30] = {
    /* cell (0,0): 'P' white on black */   'P', 0x00, 0x00, 0x00,   0xff, 0xff, 0xff,  0x00, 0x00, 0x00,
    /* cell (0,1): 'P' white on black */   'P', 0x00, 0x00, 0x00,   0xff, 0xff, 0xff,  0x00, 0x00, 0x00,
    /* cell (0,2): 'P' white on black */   'P', 0x00, 0x00, 0x00,   0xff, 0xff, 0xff,  0x00, 0x00, 0x00,
};

/* Row 1: 3 cells */
static const uint8_t Player_Nf0_r1[30] = {
    /* cell (1,0): 'P' white on black */   'P', 0x00, 0x00, 0x00,   0xff, 0xff, 0xff,  0x00, 0x00, 0x00,
    /* cell (1,1): 'P' white on black */   'P', 0x00, 0x00, 0x00,   0xff, 0xff, 0xff,  0x00, 0x00, 0x00,
    /* cell (1,2): 'P' white on black */   'P', 0x00, 0x00, 0x00,   0xff, 0xff, 0xff,  0x00, 0x00, 0x00,
};

static const uint8_t* const Player_Nf0_rows[] = {
    Player_Nf0_r0, Player_Nf0_r1,
};

static const SpriteFrame Player_Nf0 = {
    Player_Nf0_rows,  /* rows */
    3,                /* w */
    2,                /* h */
    0,                /* ox */
    0,                /* oy */
};

/* All four facings point to the same single frame */
static const SpriteFrame Player_frames[] = { Player_Nf0 };

static const SpriteSet sprite_player_set = {
    "Player",
    1,                 /* total_frames */
    Player_frames,
    {
        { 0, 1, 6 },   /* N: start=0, count=1, step=6 */
        { 0, 1, 6 },   /* S */
        { 0, 1, 6 },   /* E */
        { 0, 1, 6 },   /* W */
    }
};

#endif /* SPRITES_H */
```

After including this file in `bth.c` and removing the stub `sprite_player_set` from `bth.c`, the player will render as a 3×2 block of `P` characters in white. All four facings show the same frame — to add directional art, expand `Player_frames[]` to include S/E/W variants and update each facing's `start`/`count`.

---

## 12. Merging multiple `.ans` files

If your art lives in separate `.ans` files per character, you have three options:

### Option A: Concatenate before processing (simplest)

```bash
cat player.ans grunt.ans hulk.ans brain.ans spheroid.ans \
    enforcer.ans human.ans laser.ans terror.ans electrode.ans \
    quark.ans cruise.ans > all_sprites.ans
./build_sprites all_sprites.ans sprites.h
```

### Option B: Makefile rule (recommended for iterative art work)

```makefile
SPRITE_SRC = player.ans grunt.ans hulk.ans brain.ans spheroid.ans \
             enforcer.ans human.ans laser.ans terror.ans electrode.ans \
             quark.ans cruise.ans

sprites.h: $(SPRITE_SRC)
	cat $^ > $@.tmp
	./build_sprites $@.tmp $@
	rm $@.tmp
```

Editing any `.ans` file and running `make` regenerates `sprites.h` automatically.

### Option C: Manual append

Keep one master `.ans` file. To add a new sprite, append its block (header + art) and re-run the tool:

```bash
cat >> all_sprites.ans << 'EOF'
Index: 12
Character: Cruise
HotspotX: 0
HotspotY: 0

[ANSI sprite data here]
EOF
./build_sprites all_sprites.ans sprites.h
```

---

## 13. Verification checklist

After integrating `sprites.h`:

- [ ] `bth.c` compiles with `#include "sprites.h"` and no stub definitions
- [ ] No duplicate `sprite_*_set` symbol errors at link time
- [ ] Game runs and entities appear as text art, not colored rectangles
- [ ] Per-cell colors from the sprite data override the per-type color table
- [ ] Animations cycle through frames at the rate set by `step_period`
- [ ] Facing changes when entities change direction (verify with a 4-facing sprite)
- [ ] Collisions still work (hitboxes use `sprite_pixel_w/h`, unaffected by sprite art)
- [ ] Player invulnerability flash and death red-rect still render correctly

If any check fails, refer to the corresponding section above. The most common issues are:

1. **Symbol name mismatch** — `sprite_Grunt_set` vs `sprite_grunt_set`. Fix at the tool level or alias in `sprites.h`.
2. **Forgotten stub deletion** — leaves duplicate `sprite_*_set` definitions, causing link errors.
3. **`sprite_pixel_w/h` not updated** — collisions and rendering use mismatched dimensions.
4. **First frame is 1×1** — auto-toggle won't activate. Make sure at least one frame in each SpriteSet has `w > 1`.
