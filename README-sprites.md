sprites.h — Sprite & Animation Guide
This document is the single source of truth for the sprite system in
btm.c. It covers the format of sprites.h, how to generate
that file from .ans art sources, how to hand-edit the walk-cycle
tables after generation, and how to use the in-game walktable
editor (btm -e) to tune animation parameters live.

The system has three layers — art source (.ans), generated data
(sprites.h), and game logic (btm.c). Keeping them separate means
you can regenerate sprites.h whenever art changes without touching
game logic, and you can hand-tune walk tables without re-running the
builder.

1. Where sprites fit in the architecture
text

+-------------------+    +---------------------+    +------------------+
| .ans text-art     | -> | build_sprites7      | -> | sprites.h        |
| (one file per     |    | (ANSI-to-header     |    |  - row hex arrays|
|  character)       |    |  translation tool)  |    |  - row pointers  |
+-------------------+    +---------------------+    |  - SpriteFrame[] |
                                                    |  - SpriteSet     |
                                                    +------------------+
                                                              |
                                                              v
                                                    +------------------+
                                                    | btm.c            |
                                                    |  #include        |
                                                    |   "sprites.h"    |
                                                    |  render_sprite_  |
                                                    |   frame()        |
                                                    |  entity_select_  |
                                                    |   frame()        |
                                                    +------------------+
The game source (btm.c) defines the types (SpriteFrame,
FacingInfo, SpriteSet), the render path (render_sprite_frame(),
get_current_frame()), and the frame selector (entity_select_frame(),
which picks which frame to display based on world position). The header
file (sprites.h) supplies the data — the actual cell bytes, row
pointers, and frame tables.

When sprites.h is absent or a SpriteSet is missing, btm.c falls
back to colored-rectangle stubs for that entity. The render path
auto-detects real sprites by checking anim->frames[0].w > 1 — stubs
are 1×1, so any frame wider than one cell triggers sprite rendering.

2. Core types (defined in btm.c)
These types are defined in btm.c. Do not redefine them in
sprites.h — the header only declares data instances, not types.

c

/* One frame of text-art sprite data. */
typedef struct {
    const uint8_t* const* rows;  /* h row pointers */
    int w, h;                    /* dimensions in text cells */
    int ox, oy;                  /* per-frame placement offset (text cells) */
} SpriteFrame;

/* Per-facing animation descriptor. */
typedef struct {
    const int* frame_indices;  /* -> array of indices into frames[]      */
    int count;                 /* length of frame_indices[]                */
    int step_period;           /* spatial stride (cells/frame) or 0=unused */
    int offset_x, offset_y;    /* per-cycle placement offset (text cells)  */
    int scale_x, scale_y;      /* per-axis stride multipliers (default 1,1) */
                               /* INVERTED: bigger scale = FASTER animation */
} FacingInfo;

/* Top-level sprite definition: one per entity type. */
typedef struct {
    const char* name;
    int total_frames;
    const SpriteFrame* frames;
    FacingInfo facing[4];  /* N=0, S=1, E=2, W=3 -- matches Direction enum */
} SpriteSet;
The seven fields of FacingInfo are the heart of the animation system.
frame_indices and count form an indirection table — instead of
walking frames in source order, you walk entries of frame_indices[],
each of which is an index into the parent SpriteSet.frames[] array.
This lets a single piece of art be reused across facings, in custom
order (ping-pong, limp, repeated), or with different counts per facing.

step_period, scale_x, and scale_y together control how fast
the animation advances. They are spatial, not temporal: the game
picks the displayed frame purely from the entity's world position at
render time, not from a per-tick animation counter. See §7 for the
full math.

offset_x / offset_y are per-cycle placement shifts applied at
render time on top of the per-frame SpriteFrame.ox/oy (see §8).

3. Cell format (10 bytes per cell)
Every text cell in a frame is exactly 10 bytes:

Offset
Size
Field
Description
0	4	g[4]	UTF-8 glyph, null-terminated (1–3 bytes + zero padding)
4	1	fr	Foreground red (0–255)
5	1	fg	Foreground green (0–255)
6	1	fb	Foreground blue (0–255)
7	1	br	Background red (0–255)
8	1	bg	Background green (0–255)
9	1	bb	Background blue (0–255)

Example — a white-on-black ▀ (top-half block):

c

0xe2, 0x96, 0x80, 0x00,   /* ▀ as UTF-8 (3 bytes), then null */
0xff,                     /* fr = 255 */
0xff,                     /* fg = 255 */
0xff,                     /* fb = 255 */
0x00,                     /* br = 0 */
0x00,                     /* bg = 0 */
0x00,                     /* bb = 0 */
Cells are packed left-to-right within a row, top-to-bottom within a
frame. A frame of width w and height h has w * h cells, so its
row arrays total w * 10 bytes each and there are h such arrays
per frame.

The renderer (render_sprite_frame()) blits each cell directly into
the text buffer by memcpy-ing the 4 glyph bytes and copying the six
color bytes — no format conversion at runtime, no per-cell allocations.
This is what makes per-cell text-art sprites cheap enough to ship in a
48kB binary.

4. Naming conventions
The builder generates identifiers using a single-pool naming scheme
— there is no per-facing prefix in row or frame names. The four
facings share one flat frames[] array, and the walk_<dir>[]
indirection tables decide which frames each facing uses.

Element
Pattern
Example
Row data array	<name>_f<frame>_r<row>	grunt_f0_r0
Row pointer array	<name>_f<frame>_rows	grunt_f0_rows
Single frame struct	<name>_f<frame>	grunt_f0
Flat frames array	<name>_frames	grunt_frames
Walk-cycle table	<name>_walk_<dir>	grunt_walk_n
SpriteSet	sprite_<name>_set	sprite_grunt_set
Have-sprite macro	HAVE_SPRITE_<NAME>	HAVE_SPRITE_GRUNT

<dir> is one of n, s, e, w (lowercase). These map to
indices 0, 1, 2, 3 in FacingInfo facing[4] — same as the Direction
enum in btm.c (DIR_N=0, DIR_S=1, DIR_E=2, DIR_W=3).

The sprite_<name>_set symbol name must match what btm.c
expects. The game references fifteen sprite-set symbols — twelve
primary types plus three human sub-variants:

text

sprite_player_set      sprite_grunt_set      sprite_quark_set
sprite_hulk_set        sprite_brain_set      sprite_spheroid_set
sprite_enforcer_set    sprite_human_set      sprite_laser_set
sprite_terror_set      sprite_electrode_set  sprite_cruise_set
sprite_mommy_set       sprite_daddy_set      sprite_mikey_set
The bottom row (sprite_mommy_set, sprite_daddy_set,
sprite_mikey_set) are sub-variants of ENT_HUMAN. They use the
same human_type → anim pointer pattern that a future two-player
color system will use for the Player — see §12a for details.

If the builder produces a different name (e.g. sprite_Grunts_set
with a capital G), either fix the builder's naming or alias at the
bottom of sprites.h:

c

static const SpriteSet* const sprite_grunt_set = &sprite_Grunts_set;
The HAVE_SPRITE_<NAME> macro is emitted at the end of each block.
btm.c's stub definitions are guarded by the inverse
(#ifndef HAVE_SPRITE_GRUNT ... #endif), so when real data is present
the stubs are skipped and you get no duplicate-symbol errors at link
time.

5. Header file structure
sprites.h must contain, in order:

c

#ifndef SPRITES_H
#define SPRITES_H

/* 1. Per-row hex arrays (one per (sprite, frame, row)) */
static const uint8_t grunt_f0_r0[40] = { 0xe2, 0x96, 0x80, 0x00, ... };
static const uint8_t grunt_f0_r1[40] = { ... };

/* 2. Row pointer arrays (one per frame) */
static const uint8_t* const grunt_f0_rows[] = {
    grunt_f0_r0, grunt_f0_r1,
};

/* 3. SpriteFrame per frame (rows + w + h + ox + oy) */
static const SpriteFrame grunt_f0 = {
    grunt_f0_rows, 4, 2, 2, 1   /* rows, w=4, h=2, ox=2, oy=1 */
};

/* 4. Flat frames[] array — all frames in source order, all facings mixed */
static const SpriteFrame grunt_frames[] = {
    grunt_f0, grunt_f1, grunt_f2, grunt_f3,
};

/* 5. Walk-cycle tables (HAND-EDITABLE — see §9) */
static const int grunt_walk_n[] = { 0, 1 };
static const int grunt_walk_s[] = { 2, 3 };
static const int grunt_walk_e[] = { 0, 1 };
static const int grunt_walk_w[] = { 2, 3 };

/* 6. SpriteSet — ties frames + 4 FacingInfo descriptors together */
static const SpriteSet sprite_grunt_set = {
    "Grunt", 4, grunt_frames,
    { /* N, S, E, W -- {indices, count, step, ox, oy, sx, sy} */
        { grunt_walk_n, 2, 0, 0, 0, 1, 1 },   /* N */
        { grunt_walk_s, 2, 0, 0, 0, 1, 1 },   /* S */
        { grunt_walk_e, 2, 0, 0, 0, 1, 1 },   /* E */
        { grunt_walk_w, 2, 0, 0, 0, 1, 1 }    /* W */
    }
};

#define HAVE_SPRITE_GRUNT 1

/* ... repeat for all 12 entity types ... */

#endif /* SPRITES_H */
The SpriteSet initializer is the 7-field FacingInfo form. Each
entry is { frame_indices, count, step_period, offset_x, offset_y, scale_x, scale_y }. The default scale_x=1, scale_y=1 reproduces
legacy cells / step_period behavior bit-for-bit.

6. The two animation strategies
There is no temporal animation advance in this engine. The game
does not increment a per-tick frame counter — instead, the displayed
frame is chosen purely from the entity's world position at render
time. This is the same trick used by arcade classics like Robotron:
2084, where the displayed frame depends on how far the enemy has
travelled, not on how long it has existed.

Two strategies are implemented in entity_select_frame():

6.1 Stride cycle (Hulk / Spheroid / Brain / Quark style)
step_period > 0. The frame advances by one every step_period
terminal cells of motion along the axis the entity is moving in:

c

// entity_select_frame() — ENT_HULK (and ENT_SPHEROID via fallthrough)
int axis = (facing_dir == DIR_UP || facing_dir == DIR_DOWN) ? 1 : 0;
int16_t pos = axis ? wy : wx;
return stride_cycle(pos, spatial_stride, spatial_scale, count, axis);

// stride_cycle() — the actual math
int32_t cells = (axis == 0)
              ? ((int32_t)pos * g_term_cols) / ((int32_t)COORD_SCALE * SCREEN_WIDTH)
              : ((int32_t)pos * g_term_rows) / ((int32_t)COORD_SCALE * SCREEN_HEIGHT);
if (cells < 0) cells = 0;
return (int)(((cells * scale) / stride) % count);
So if walk_n = {0,1,2,3,4,5,6} (count=7) and step_period=4,
scale=1, the Hulk advances to the next frame every 4 terminal
cells it moves vertically. After 28 cells (4 × 7) the cycle wraps
back to frame 0.

6.2 Half-row picker (Grunt / Enforcer style)
step_period = 0. The frame is chosen by where the entity is, not
how far it's walked — specifically, by whether its current wy falls
in an aligned row or an in-between (half-row shifted) position:

c

// entity_select_frame() — ENT_GRUNT (and ENT_ENFORCER via fallthrough)
return half_row_pick(wy, count);

// half_row_pick() — extracted helper
int32_t raw_pixels = (int32_t)wy / COORD_SCALE;
int32_t half_rows  = raw_pixels * (g_term_rows * 2) / SCREEN_HEIGHT;
if (half_rows < 0) half_rows = 0;
return (int)(half_rows & 1);
So walk_n[] for the Grunt is a 2-element binary pick — index 0
for aligned rows, index 1 for in-between (half-row shifted) positions.
The Grunt's art uses 3-row frames where the middle row is the main
silhouette and the top/bottom rows use ▀/▄ half-blocks, so picking
frame 1 visually shifts the sprite down by half a row.

6.3 Which strategy applies to which entity?
entity_select_frame() has explicit switch cases. Adding a new
animated entity means adding a case (or a fall-through to an existing
one):

Entity
Strategy
Notes
ENT_HULK	stride cycle	step_period from FacingInfo
ENT_SPHEROID	stride cycle (falls through to Hulk case)	6-frame morph cycle
ENT_GRUNT	half-row picker	2-frame N/S bob
ENT_ENFORCER	half-row picker (falls through to Grunt case)	2-frame N/S bob
ENT_HUMAN	default return 0	Per-variant SpriteSet picked at spawn; see §12a
All others	default return 0	Single-frame stubs or unmapped

The default case returns 0 — correct for stubs (which have count=1)
and for entities that don't animate (Laser, Cruise, Electrode).

7. The 7-field FacingInfo and inverted scale math
The full initializer is:

c

{ frame_indices, count, step_period, offset_x, offset_y, scale_x, scale_y }
7.1 step_period — spatial stride, not temporal ticks
step_period is the number of terminal cells the entity must
traverse along its axis of motion before the frame index advances by
one. Lower = faster animation. A count of 1 with any step_period
produces a static sprite (the same frame is "advanced to" forever).

For half-row-picker entities (step_period = 0), step_period is
unused — the picker ignores it.

7.2 scale_x / scale_y — per-axis stride multipliers
scale_x applies to E/W facings, scale_y to N/S facings. The
per-axis split lets the Hulk's N/S walk animate at a different rate
than its E/W walk without code changes — useful when the art's N/S
frames are 3 rows tall but the E/W frames are 3 columns wide and
you want the visible cadence to match.

7.3 Inverted scale: bigger = FASTER
The math multiplies cells by scale before dividing by
step_period, preserving sub-stride precision in pure integer
arithmetic:

c

frame = ((cells * scale) / step_period) % count;
This means scale values larger than step_period yield fractional
effective strides:

step_period
scale
Effective stride
Effect
4	1	4 cells/frame	Legacy behavior
4	2	2 cells/frame	2× faster
4	4	1 cell/frame	4× faster
4	8	0.5 cells/frame	8× faster (sub-cell advance)
4	16	0.25 cells/frame	16× faster

This is intentional — the inversion lets you fine-tune animation
speed without changing step_period (which would also change the
cycle wrap distance and break spatial alignment with other entities).

7.4 Recommended starting values
Entity
step_period
scale_x, scale_y
Rationale
Player	6	1, 1	Smooth walk
Grunt	0	1, 1	Half-row picker (step unused)
Hulk	4	1, 1	Lumbering
Spheroid	2	1, 1	Constantly morphing
Enforcer	0	1, 1	Half-row picker (hover bob)
Brain	4	1, 1	Pulsating
Human	4	1, 1	Gentle wandering
Quark	4	1, 1	Erratic
Laser	1	1, 1	Static (count=1)
Terror	1	1, 1	Rapid flicker
Cruise	1	1, 1	Static
Electrode	1	1, 1	Static

These are starting points. The whole point of btm -e is to tune
them live — see §10.

8. Per-frame placement offset (ox, oy)
The ox, oy fields on each SpriteFrame control where on the
sprite the entity's world position is anchored. They are in
text-cell units, not pixels. build_sprites7.c bakes them from
the .ans source as ox_total = OffsetX + HotspotX and
oy_total = OffsetY + HotspotY.

At render time, the renderer applies them after the per-cycle
FacingInfo.offset_x/y:

c

int tx = (int)((int32_t)e->wx * g_term_cols / ((int32_t)COORD_SCALE * SCREEN_WIDTH));
int ty = (int)((int32_t)e->wy * g_term_rows / ((int32_t)COORD_SCALE * SCREEN_HEIGHT));
const SpriteFrame* sf = get_current_frame(e->anim, e->facing_dir, ...);
const FacingInfo* fi   = get_facing_info(e->anim, e->facing_dir);
if (fi) { tx += fi->offset_x; ty += fi->offset_y; }
if (sf) { tx -= sf->ox;       ty -= sf->oy;       }
render_sprite_frame(sf, tx, ty);
The ox, oy are SUBTRACTED (not added) because they represent
"the offset from the sprite's top-left to its hotspot cell" —
to land the hotspot ON the entity's terminal cell, the sprite's
top-left must be drawn at (entity_cell - ox, entity_cell - oy).
This is what lets frames of different sizes within a walk cycle
share the same visual anchor — both frames' hotspot cells land
on the same screen position, so the sprite pulses in place
rather than teleport-jumping between frame sizes.

This is what lets a small frame (e.g. Spheroid f0 = 2×1) align
visually with larger frames in the same cycle — set HotspotX and
HotspotY in the .ans so the small frame's hotspot cell coincides
with the larger frames' hotspot cells, and the sprite will appear to
pulse in place rather than teleport-jumping between frame sizes.

Known redundancy: OffsetX/Y and HotspotX/Y in the .ans
format are summed together by the builder before being stored in
SpriteFrame.ox/oy. They could be collapsed into a single pair
without changing any rendered output. Left as-is for now to avoid
breaking existing .ans files; flag for future cleanup.

For per-cycle shifts (e.g. a "half-row down" frame baked into a
3-row variant), use offset_x/offset_y on the FacingInfo, or
bake the shift into the sprite art itself.

9. Generating sprites.h from .ans files
9.1 Build the builder (one time)
bash

 $ cd /home/z/my-project/download
 $ gcc -O2 -Wall build_sprites7.c -o build_sprites7
This produces the build_sprites7 executable. You only re-run this
step if you edit build_sprites7.c itself.

9.2 Run the builder
Two CLI forms, auto-detected by whether argv[1] ends in .ans:

bash

# Multi-input form (preferred):
 $ ./build_sprites7  sprites.h  ENT_GRUNT.ans  ENT_HULK.ans

# Legacy single-input form (when argv[1] is .ans):
 $ ./build_sprites7  ENT_GRUNT.ans  sprites.h
The builder will:

Load any existing sprites.h (so previously-generated sprite
blocks are preserved — only blocks whose name appears in the new
.ans inputs get overwritten).
Parse each .ans input, extracting per-frame pixel data and
the Facing: tag (which can list multiple directions, e.g. NS).
Auto-group frames into N/S/E/W walk tables based on Facing:
tags. A frame tagged Facing: NS is added to both walk_n[]
and walk_s[].
Write the merged sprites.h.
9.3 .ans file format
Each frame in a .ans file starts with a header block followed by
the ANSI-colored text art:

text

Index: 11
Character: Spheroid
FrameNo: 0
OffsetX: 1
OffsetY: 1
HotspotX: 1
HotspotY: 1

[ANSI sprite data here — escape sequences + UTF-8 block characters]
Index — frame index in the source file (informational)
Character — entity name (must match an entry in build_sprites7.c's ENTITY_MAP[] array — currently 15 entries: player, grunt, quark, hulk, brain, spheroid, enforcer, human, mommy, daddy, mikey, laser, terror, electrode, cruise). mommy/daddy/mikey are sub-variants of ENT_HUMAN (see §12a).
FrameNo — sub-frame number within the source (informational)
OffsetX/Y — placement offset in text cells (baked into SpriteFrame.ox/oy)
HotspotX/Y — anchor offset (added to OffsetX/Y by the builder; both together SUBTRACTED from the entity's terminal cell at render time — see §8)
Facing (optional) — comma-separated directions: N, S, E,
W, or pairs like NS. Drives auto-grouping into walk tables.
Frames without a Facing: tag are reachable only via the "no
tagged frames for this facing" fallback.
9.4 Safety rails
The builder refuses to shoot you in the foot:

Extension gate: the output path must end in .h (or
.hpp/.hh/.hxx). Anything else is rejected with a clear error.
This prevents the classic footgun:
text

$ ./build_sprites7 ENT_GRUNT.ans ENT_HULK.ans sprites.h
Error: output path 'ENT_HULK.ans' does not look like a C header.
(That command gets auto-detected as legacy form because argv[1]
ends in .ans, so it would try to write the C output to
ENT_HULK.ans — clobbering your Hulk source. The safety check
stops this.)
Overwrite prompt: if the destination .h already exists, you
get a [y/N] prompt before anything is written. Answer y to
proceed, anything else (or EOF) to abort with no files changed.
Bypass for scripted use:
bash

$ echo y | ./build_sprites7 sprites.h ENT_HULK.ans
$ BTSPRITES_FORCE=1 ./build_sprites7 sprites.h ENT_HULK.ans
9.5 What you'll see on stderr
text

Loaded 1 existing sprite block(s) from 'sprites.h'.
Parsed 1 sprite(s) from 1 .ans file(s):
  - Hulk       : 23 frame(s), dims 5x3/5x4/5x3/...  (23 tagged, 0 untagged)
Note: overwriting existing 'hulk' block with new data from .ans.
Wrote 'sprites.h'.
The (N tagged, M untagged) tally tells you how many frames had a
Facing: tag vs. how many were untagged. Untagged frames are
reachable only via the "no tagged frames for this facing" fallback.

10. The walktable editor (btm -e)
The walktable editor is a built-in mode for tuning sprite animation
parameters live, without manually editing sprites.h and recompiling.
Launch it with:

bash

 $ ./btm -e
The editor has two views: a selector (pick which entity to edit)
and a walktable editor (tune that entity's animation parameters).

10.1 View 1: Entity selector
text

BLAPOTRON SPRITE EDITOR -- select entity
press 0-9, A-E to select    ESC/Q to quit

+--------+--------+--------+--------+--------+
| [0]    | [1]    | [2]    | [3]    | [4]    |
| Player | Grunt  | Quark  | Hulk   | Brain  |
+--------+--------+--------+--------+--------+
| [5]    | [6]    | [7]    | [8]    | [9]    |
| Spher. | Enforc | Human  | Laser  | Terror |
+--------+--------+--------+--------+--------+
| [A]    | [B]    | [C]    | [D]    | [E]    |
| Elect. | Cruise | (res)  | (res)  | (res)  |
+--------+--------+--------+--------+--------+
5 columns × 3 rows = 15 visible cells.
Hex keys 0-9 + A-E (case-insensitive) select the matching
entity. Slot F is accepted by the parser but currently unmapped.
Each cell shows a preview of frame 0 of the entity's walk_s
table, centered in the cell.
Reserved cells (C, D, E) render borders + (reserved)
placeholder. They're room for future sprite sets without renumbering
the existing keys.
ESC or Q quits the editor.
The preview uses auto-centering ((cell_w - sf->w) / 2) and does
not apply sf->ox/oy — that's intentional, so the gallery layout
stays uniform regardless of per-frame offsets.

10.2 View 2: Walktable editor
Once you select an entity, you enter the walktable editor. The screen
becomes a full-screen arena with the live sprite at its current
position, a HUD at top-left, and a thumbnail strip of all the
entity's frames at the bottom.

Movement keys
text

q w e    =  up-left, up, up-right
a s d    =  left, (noop), right
z x c    =  down-left, down, down-right
Diagonal keys (q, e, z, c) move the sprite diagonally without
changing the facing. Orthogonal keys (w, a, d, x) move the
sprite and update the facing — so pressing w switches to the N
walk table, d to E, a to W, x to S. Movement steps are one
half-row vertically and one full column horizontally, matching the
spatial bucketing used by entity_select_frame().

Tab — cycle facing (or human variant)
For most entity types, Tab cycles the active facing
N → S → E → W → N without moving the sprite. Useful when you want to
edit a facing's walk table without repositioning.

Human sub-variant cycling (key 7 only): When editing ENT_HUMAN
(Human, key 7), Tab instead cycles the human variant
Mommy → Daddy → Mikey → Mommy, switching the live preview between the
three per-character SpriteSets. The HUD shows the active variant name.
The facing still changes automatically when you move the sprite with
the directional keys, so you can combine variant selection with facing
changes in a single editing session.

Enter — edit walk table
Enter opens an inline text editor pre-populated with the current
facing's walk table as a comma-separated list:

text

0, 1, 2, 3_
Type to append, Backspace to delete, Enter to commit. The parser
validates each index against the sprite's total_frames and rejects
out-of-range values with a red error flash. On successful commit,
the new walk table is written to the in-memory shadow copy and the
live preview immediately reflects the change.

Examples you can type:

Input
Effect
0, 1	Standard 2-frame cycle
0, 1, 2, 1	Ping-pong (forward then back, no endpoint repeat)
0, 0, 1, 2	Limp / skip (frame 0 held twice)
0	Single-frame static
2, 3, 4, 5	Subset (skip first two frames)

+ / - — tune scale
+ (or =) increments the active axis's scale. - decrements it.

For N/S facings, the active axis is scale_y.
For E/W facings, the active axis is scale_x.
Range: 1 to 32.
Bigger scale = faster animation (see §7.3). The change is applied to
the in-memory shadow copy immediately, so you see the sprite's
animation rate change in real time as you tap +.

S — save to sprites.h
Capital S (Shift+s) writes the current shadow state back to
sprites.h. The walk tables, scale values, and step_periods for the
active entity are written into the matching SpriteSet initializer.
Other entities' blocks are preserved verbatim — the save is surgical,
not a full regen.

The save reads sprites.h line-by-line, finds the active entity's
SpriteSet initializer block, and rewrites just that block. If the
file can't be opened or the block can't be found, the save fails
silently (check stderr for diagnostics).

ESC — return to selector
ESC leaves View 2 and returns to the entity selector (View 1). Any
unsaved shadow changes are discarded — make sure to press S before
ESC if you want to keep your edits.

10.3 What the editor modifies vs. what it leaves alone
Field
Editable in btm -e?
How
walk_<dir>[] indices	Yes	Enter key, type comma-separated list
count	Yes (auto)	Updated to match walk_<dir>[] length on commit
scale_x / scale_y	Yes	+ / - keys
step_period	No	Edit sprites.h by hand
offset_x / offset_y	No	Edit sprites.h by hand
SpriteFrame.ox / oy	No	Set in .ans, baked by builder
Row pixel data	No	Set in .ans, baked by builder

The editor focuses on the parameters you're most likely to tune
interactively (which frames each facing shows, and how fast they
advance). Structural changes (frame art, frame dimensions, per-cycle
offsets) require editing the .ans source and re-running
build_sprites7.

11. Hand-editing walk tables
If you prefer a text editor to the interactive btm -e, you can edit
the walk tables directly in sprites.h. The rules are the same —
the editor just automates the bookkeeping.

11.1 What the numbers in walk_n[] actually mean
Each entry in walk_n[] is an index into frames[]. So:

c

static const int grunt_walk_n[] = { 0, 1 };
//                                ↑  ↑
//                     frame index 0  frame index 1
//                     = grunt_f0     = grunt_f1
The game's entity_select_frame() function picks which entry of
walk_n[] to display based on the entity's world position. The
number that's stored at that entry tells the renderer which
SpriteFrame to draw.

So if you want frame grunt_f2 to appear first when walking North,
change the list to:

c

static const int grunt_walk_n[] = { 2, 1 };   // f2 first, then f1
11.2 Editing rules of thumb
Indices must be valid. Every number in your walk_<dir>[]
list must be < total_frames for that sprite. Writing
walk_n[] = { 0, 7 } for the Grunt (which has only 4 frames) is
undefined behavior — the renderer will read past the end of
grunt_frames[].
Count the entries correctly. The number in the SpriteSet's
FacingInfo must match the array length exactly:
c

static const int hulk_walk_n[] = { 0,1,2,3,4,5,6 };   // 7 entries
// ...
{ hulk_walk_n, 7, 4, 0, 0, 1, 1 },   /* N — count=7, must match */
If you add or remove an entry from walk_n[], you must also
update the count field in the SpriteSet. Otherwise the
renderer will either skip frames or read garbage.
Reorder freely. The same frame can appear multiple times:
c

// Ping-pong: 0 1 2 3 2 1  (forward then back, no repeat of endpoints)
static const int hulk_walk_n[] = { 0, 1, 2, 3, 2, 1 };
And count becomes 6 in the SpriteSet.
Reuse across facings. Multiple walk_<dir>[] tables can
point at the same frames:
c

// Grunt doesn't really walk E/W — reuse the N frames as a fallback
static const int grunt_walk_e[] = { 0, 1 };   // same as walk_n
static const int grunt_walk_w[] = { 0, 1 };
step_period and scale_x/y live in the SpriteSet, not
the walk table. Each FacingInfo has its own triplet, so you can
give different strides per facing if you want (rarely needed):
c

{ hulk_walk_n, 7, 4, 0, 0, 1, 1 },   /* N — stride 4 cells/frame, scale 1 */
{ hulk_walk_s, 7, 4, 0, 0, 1, 1 },   /* S — same */
{ hulk_walk_e, 8, 4, 0, 0, 2, 2 },   /* E — 2x faster (scale=2) */
{ hulk_walk_w, 8, 4, 0, 0, 2, 2 }    /* W — same as E */
11.3 Worked example: making the Hulk's N walk use only 4 frames
Suppose you decide the Hulk's 7-frame NS cycle is too busy and you
want a simpler 4-frame walk. Edit sprites.h like this:

c

// Before:
static const int hulk_walk_n[] = { 0,1,2,3,4,5,6 };
// ...
{ hulk_walk_n, 7, 4, 0, 0, 1, 1 },

// After:
static const int hulk_walk_n[] = { 0, 2, 4, 6 };   // every other frame
// ...
{ hulk_walk_n, 4, 4, 0, 0, 1, 1 },                 // count changed 7 -> 4
Now the Hulk alternates between frames 0, 2, 4, 6 (the even-numbered
NS frames), advancing one slot every 4 cells of vertical motion.
The cycle wraps every 16 cells (4 × 4) instead of 28.

11.4 Worked example: making the Hulk walk faster
To make the Hulk's NS walk cycle visibly faster, increase
scale_y (so each frame advance happens sooner):

c

// Before:
{ hulk_walk_n, 7, 4, 0, 0, 1, 1 },   // new frame every 4 cells

// After:
{ hulk_walk_n, 7, 4, 0, 0, 2, 2 },   // new frame every 2 cells (2x faster)
The frame count (7) and the walk_n[] indices stay the same —
only the scale_y field changes. (You could also decrease
step_period from 4 to 2, but that changes the cycle wrap distance
and may break spatial alignment with other entities — scale is
the cleaner knob.)

11.5 Worked example: a ping-pong cycle
For a walk that goes forward then backward
(e.g. 0→1→2→3→2→1→repeat):

c

static const int hulk_walk_n[] = { 0, 1, 2, 3, 4, 5, 6, 5, 4, 3, 2, 1 };
// ...
{ hulk_walk_n, 12, 4, 0, 0, 1, 1 },   // count=12
This doubles the visible cycle length without needing more source art.

11.6 Critical rule: hand-edit AFTER the final builder run
build_sprites7 regenerates walk tables from scratch when it
overwrites a clashing block. If you run the builder after
hand-editing, your edits will be lost. The correct workflow is:

Run build_sprites7 as many times as needed (adding new sprites,
regenerating after .ans edits).
Then hand-edit the walk tables in the final sprites.h (or
use btm -e to apply them interactively and save).
Commit sprites.h to source control so the hand-edits are
preserved.
If you need to regenerate a sprite's row data without losing the
walk-table edits, copy the walk-table arrays out to a scratch file,
re-run the builder for that block (use the merge feature —
non-clashing blocks are preserved verbatim), then paste the walk
tables back in. Or just use btm -e after the regen — the editor
reads the existing sprites.h to seed its shadow state, so you can
re-apply your tweaks in seconds.

12. Integration procedure
Follow these steps to replace stub sprites with real art:

Step 1: Generate sprites.h
Run the ANSI-to-header tool on your .ans art files:

bash

./build_sprites7 sprites.h ENT_GRUNT.ans ENT_HULK.ans ENT_SPHEROID.ans ...
Or use a Makefile rule (recommended):

makefile

SPRITE_SRC = ENT_PLAYER.ans ENT_GRUNT.ans ENT_QUARK.ans ENT_HULK.ans \
             ENT_BRAIN.ans ENT_SPHEROID.ans ENT_ENFORCER.ans \
             ENT_HUMAN.ans ENT_LASER.ans ENT_TERROR.ans \
             ENT_ELECTRODE.ans ENT_CRUISE.ans

sprites.h: $(SPRITE_SRC)
        cat $^ > $@.tmp
        ./build_sprites7 $@.tmp $@
        rm $@.tmp
Step 2: Place sprites.h next to btm.c
bash

cp sprites.h /path/to/game/
Step 3: Include it in btm.c
Add this line near the top of btm.c, after the system includes:

c

#include "sprites.h"
Step 4: Remove (or #ifdef out) the stub definitions in btm.c
The stub definitions are guarded by #ifndef HAVE_SPRITE_<NAME>, so
once sprites.h defines those macros, the stubs are skipped
automatically. No manual deletion needed — just make sure the
#include comes before the stub definitions in btm.c.

Step 5: Update sprite_pixel_w[] and sprite_pixel_h[]
These arrays drive collision bounds, clamping, and
safe-spawn checks. They are in screen-pixel units. Update
them to match your real sprite dimensions:

c

/* Index: Player, Grunt, Quark, Hulk, Brain, Spheroid, Enforcer, Human, Laser, Terror, Electrode, Cruise */
static const int sprite_pixel_w[NUM_ENTITY_TYPES] = {
    20, 18, 20, 24, 22, 18, 20, 10, 8, 10, 16, 4
};
static const int sprite_pixel_h[NUM_ENTITY_TYPES] = {
    20, 18, 20, 24, 22, 18, 20, 22, 8, 10, 16, 4
};
The conversion is: screen pixels = text-cell width × 4
(approximately — the screen-to-text-cell mapping uses
g_term_cols / SCREEN_WIDTH which works out to roughly 4 pixels per
text cell at default terminal sizes). For a 3×3 text-cell sprite,
set width and height to ~12 pixels each.

You do not need to change PLAYER_SPRITE_W / PLAYER_SPRITE_H /
LASER_SPRITE_W / LASER_SPRITE_H macros — they are only used for
spawn-position offset math and remain in screen-pixel units.

Step 6: Verify auto-toggle works
The render path in render_all() auto-detects real sprites by
checking anim->frames[0].w > 1. Stubs are 1×1, so any frame wider
than 1 cell triggers sprite rendering.

To confirm: build and run the game. If your sprites appear as text
art with their embedded colors, the toggle worked. If they still
appear as colored rectangles, the frames[0].w check is failing —
verify your first frame's w field is greater than 1.

12a. Human character variants (per-instance anim pick)
ENT_HUMAN is a special case: rather than one global SpriteSet for
all humans, the game has three sub-variant SpriteSets — one per
named character (Mommy, Daddy, Mikey). Each spawned human picks its
variant at creation time, and the editor lets you cycle between
variants to tune each one's walk tables independently.

Architecture
text

spawn_entity(wx, wy, ENT_HUMAN)
        |
        v
human_apply_variant(e, human_type)
        |
        +-- human_type=0  --> e->anim = &sprite_mommy_set
        +-- human_type=1  --> e->anim = &sprite_daddy_set
        +-- human_type=2  --> e->anim = &sprite_mikey_set
Three things are set by human_apply_variant():

e->human_type — 0/1/2 (variant selector)
e->anim — pointer to the matching sprite_<name>_set
(The entity_select_frame() switch still falls through to the
default return 0 case for ENT_HUMAN, so the per-variant walk
table is what makes animation happen)
When does human_type get assigned?
There are two spawn paths, both in btp.c:

spawn_entity() ENT_HUMAN case — for emergent respawns during
gameplay. Picks a random variant with rand() % 3, so each
character is equally likely.
spawn_wave_for_level() — for the per-wave spawn budget. The
level designer explicitly chooses how many of each character to
spawn (e.g. w->mommies, w->daddies, w->mikeys), and calls
human_apply_variant() with the specific type after each
spawn_entity().
Stub fallback behavior
If sprites.h hasn't yet been rebuilt with ENT_MOMMY.ans,
ENT_DADDY.ans, and ENT_MIKEY.ans art, three stub SpriteSets
take over — all 1×1 cells, all named after their variant. The
colored-rect fallback in render_all() then paints the rect using
per-variant colors keyed off e->human_type:

Variant
human_type
Stub color RGB
Color name
Mommy	0	(255, 182, 193)	Pink
Daddy	1	(173, 216, 230)	Light blue
Mikey	2	(255, 218, 185)	Peach

Once real .ans art is added for any one variant, the matching
#ifndef HAVE_SPRITE_<NAME> stub is suppressed at compile time, and
that character renders as text art while the others remain colored
rects.

Editor: variant cycling
When the Human entity is selected (key 7), the walktable editor's
Tab key changes meaning: instead of cycling facing (the default
behavior for other entity types), it cycles g_editor_human_variant
through 0 → 1 → 2 → 0, switching the live preview between the
Mommy / Daddy / Mikey SpriteSets. The HUD reflects the active
variant. Use this to tune each variant's walk table without leaving
the editor.

The facing can still be changed by moving the sprite with the
directional keys (w/a/d/x), so you can combine variant
selection with facing changes in a single editing session.

Builder integration
build_sprites7.c's ENTITY_MAP[] now contains 15 entries —
the original 12 plus three new ones for the human sub-variants:

c

{ "mommy",     "mommy",     "Mommy",     4 },   /* ENT_HUMAN sub-variant 0 */
{ "daddy",     "daddy",     "Daddy",     4 },   /* ENT_HUMAN sub-variant 1 */
{ "mikey",     "mikey",     "Mikey",     4 },   /* ENT_HUMAN sub-variant 2 */
Each entry tells the builder to (a) name the resulting SpriteSet
sprite_<name>_set, (b) emit a HAVE_SPRITE_<NAME> guard macro, and
(c) treat each .ans source as a 4-column sprite set.

Adding a new human variant (template)
If you ever want a fourth human character (e.g. "Grandma"):

Add { "grandma", "grandma", "Grandma", 4 } to ENTITY_MAP[] in
build_sprites7.c.
Add sprite_grandma_set to the stub list in btp.c, guarded by
#ifndef HAVE_SPRITE_GRANDMA. Reuse stub_frames[7] (the human
stub slot).
Extend human_apply_variant() to handle human_type=3, returning
&sprite_grandma_set.
Bump the variant range in spawn_entity() ENT_HUMAN case from
rand() % 3 to rand() % 4.
In the editor, change g_editor_human_variant = (...) % 3 to
% 4 and add a human_type=3 case to
editor_get_sprite_set().
Add a grandmas field to the per-wave spawn budget struct and
spawn loop.
Optionally pick a new stub color in render_all() for
human_type=3.
The same per-instance anim pointer pattern is what a future
two-player color system will use for the Player — replace the
human_type field with a player_color field, ship 2-4
sprite_player_<color>_set definitions, and pick at
spawn_player() time.

13. Rendering pipeline (what the game does)
For each entity, every frame, render_all() does this:

text

1. Is entity active?                       -> if no, skip
2. Does anim exist and frames[0].w > 1?    -> if no, use colored rect fallback
3. Lookup frame: get_current_frame(anim, facing_dir, wx, wy, type)
   - calls entity_select_frame() to pick index into walk_<dir>[]
   - returns &ss->frames[ walk_<dir>[selected_index] ]
4. Convert world coords to text coords:
       tx = wx * g_term_cols / (COORD_SCALE * SCREEN_WIDTH)
       ty = wy * g_term_rows / (COORD_SCALE * SCREEN_HEIGHT)
5. Apply per-cycle offset from FacingInfo:
       tx += fi->offset_x; ty += fi->offset_y
6. Apply per-frame offset from SpriteFrame (SUBTRACTED -- see §8):
       tx -= sf->ox;       ty -= sf->oy
7. render_sprite_frame(sf, tx, ty):
   For each (row, col) in the frame:
     - Compute target text buffer index
     - Skip if off-screen
     - memcpy 4 bytes from cell to text_buffer.glyph
     - Copy fr, fg, fb to text_buffer.r/g/b
     - Copy br, bg, bb to text_buffer.br/bg/bb
8. flush_text_buffer() emits the entire buffer with ANSI escape sequences
The player has an additional 3-way branch: invulnerability flash
(skip on "off" frames), death (red rect override), otherwise sprite
or stub.

14. Verification checklist
After integrating sprites.h:

 btm.c compiles with #include "sprites.h" and no stub
definitions
 No duplicate sprite_*_set symbol errors at link time
 Game runs and entities appear as text art, not colored
rectangles
 Per-cell colors from the sprite data override the per-type
color table
 Animations cycle through frames at the rate set by
step_period and scale_x/y
 Facing changes when entities change direction (verify with a
4-facing sprite)
 Collisions still work (hitboxes use sprite_pixel_w/h,
unaffected by sprite art)
 Player invulnerability flash and death red-rect still render
correctly
 Small frames with non-zero ox/oy align visually with larger
frames in the same cycle (no teleport-jumping between frame sizes)
 btm -e opens the selector showing all 12 entity types in a
3×5 grid with hex keys 0-B
 In btm -e, + / - change the live sprite's animation
rate visibly
 In btm -e, S writes the changes back to sprites.h
without corrupting other entities' blocks
 In btm -e, selecting Human (key 7) and pressing Tab
cycles the live preview through Mommy → Daddy → Mikey (see §12a)
 Spawned humans pick a variant at creation time — Mommy (pink),
Daddy (blue), or Mikey (peach) colored rects visible when stubs
are still active
If any check fails, refer to the corresponding section above. The
most common issues are:

Symbol name mismatch — sprite_Grunt_set vs
sprite_grunt_set. Fix at the builder level or alias in
sprites.h.
Forgotten stub deletion — leaves duplicate sprite_*_set
definitions, causing link errors. The HAVE_SPRITE_* macros
should prevent this; if not, check include order.
sprite_pixel_w/h not updated — collisions and rendering
use mismatched dimensions.
First frame is 1×1 — auto-toggle won't activate. Make sure
at least one frame in each SpriteSet has w > 1.
Walk table count mismatch — count field in FacingInfo
doesn't match the array length. Renderer will skip frames or
read garbage.
15. Quick reference
TASK
COMMAND / EDIT
Build the builder	gcc -O2 -Wall build_sprites7.c -o build_sprites7
Generate sprites.h from one .ans	./build_sprites7 sprites.h ENT_GRUNT.ans
Generate from multiple .ans files	./build_sprites7 sprites.h ENT_GRUNT.ans ENT_HULK.ans
Force overwrite (no prompt)	BTSPRITES_FORCE=1 ./build_sprites7 sprites.h ENT_HULK.ans
Rebuild the game after sprite edits	gcc -O2 -Wall -Wno-unused-function -o btp btp.c (no -lm — see CHANGELOG)
Launch the walktable editor	./btp -e
Change which frames a facing shows	Edit walk_<dir>[] indices + update count in SpriteSet, or use btm -e Enter
Change walk-cycle speed	Edit scale_x/scale_y in the SpriteSet's FacingInfo, or use btm -e +/-
Change walk-cycle length	Add/remove entries in walk_<dir>[] + update count
Make a ping-pong cycle	Repeat indices in reverse in walk_<dir>[] (e.g. {0,1,2,3,2,1})
Save editor changes to disk	btp -e → S
Return from walktable editor to selector	btp -e → ESC
Cycle facing in walktable editor	btp -e → Tab
Cycle human variant in editor	select Human (7) → Tab (Mommy → Daddy → Mikey → Mommy)
Generate sprites for human variants	./build_sprites7 sprites.h ENT_MOMMY.ans ENT_DADDY.ans ENT_MIKEY.ans

