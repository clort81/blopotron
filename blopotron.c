/*
// ============================================================
//  Blopotron 2024 terminal robotron-like game: ver. 0.75 (btn)
//  Pure Terminal Mode - No SDL Dependencies
//
//  Building:
//  gcc btk.c -o btk
// ============================================================
//
//  REVISION HISTORY (brief)
//  ------------------------
//  bta..bti  : table-driven sprite system, half-row positional
//              frame selection for the Grunt, stride-cycle frame
//              selection for the Hulk, multi-direction Facing: tags
//              in build_sprites7.c.
//  btk       : adds per-entity RENDER HOOK scaffold in render_all().
//              Table-driven path remains the default for all entity
//              types that fit the SpriteSet/FacingInfo model (Grunt,
//              Hulk, Brain, Spheroid, etc.).  Entities that need
//              custom rendering (Cruise missile with trailing body,
//              Prog with morphing shapes, etc.) can plug in via a
//              case in render_entity_custom() without disturbing
//              the universal path.  No custom renderers implemented
//              yet -- the hook is a no-op until those land.
// ============================================================
*/
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <locale.h>
#include <termios.h>
#include <sys/select.h>

// --- Constants ---
#define WALL_LEFT   (1 << 0)
#define WALL_RIGHT  (1 << 1)
#define WALL_TOP    (1 << 2)
#define WALL_BOTTOM (1 << 3)
#define COORD_SCALE 16
#define SCREEN_WIDTH 792
#define SCREEN_HEIGHT 600
#define WALL_MARGIN 24
#define FIXED_TO_SCREEN(val) ((val) / COORD_SCALE)
#define SCREEN_TO_FIXED(val) ((val) * COORD_SCALE)
#define FIXED_0_707   11585
#define LASER_SPEED    150
#define FIRE_COOLDOWN    6
#define PLAYER_SPEED    64
#define HULK_SPEED      64
#define GRUNT_SPEED    200
#define GRUNT_SHYNESS   50
#define QUARK_SPEED    200
#define SPHEROID_SPEED           9
#define SPHEROID_STATE_MOVE      0
#define SPHEROID_STATE_PAUSE     1
#define SPHEROID_WINDUP_TICKS    42
#define SPHEROID_CORNER_DIST     78
#define SPHEROID_NUDGE_RANGE     4
#define SPHEROID_RAMP_TIME       30
#define SPHEROID_REST_TIME       40

/* Spheroid walk-cycle animation tuning.
 *
 * Unlike the Grunt/Enforcer (which have only 2 frames toggled purely by
 * half-row parity) and the Hulk (which strides through N frames along its
 * axis of travel), the Spheroid is a HYBRID: its 6-frame walk table is
 * split into two banks of 3 -- frames {0,1,2} are the "aligned" bank
 * (drawn when the spheroid sits on a whole terminal row), and frames
 * {3,4,5} are the "in-between" bank (drawn when it sits on a half-row
 * between two terminal rows).  The half-row parity bit picks which
 * bank; the temporal counter (Entity.anim_counter) picks which of the
 * 3 frames within that bank.
 *
 * This constant controls how many spheroid ticks (= how many entity
 * redraws, since the spheroid's tick_period=2 means it ticks every
 * 2nd game frame) must elapse before the within-bank sub-frame
 * advances.  At 3, the walk-cycle takes 3*3=9 ticks to complete one
 * full bank cycle (or 9*2=18 game frames).  Bump higher for a slower
 * "breathing" cycle; lower for a faster spin.  Tune live by recompiling
 * -- this isn't editor-adjustable yet (it's not in FacingInfo). */
#define SPHEROID_WALK_FRAMES_PER_ADVANCE  3
#define ENFORCER_SPEED   3
#define HULK_PUSHBACK   14
#define MAX_ENTITIES   256
#define MAX_PLAYERS      1
#define MAX_LASERS      16
#define PLAYER_LIVES    8
#define EXCLUSION_RADIUS SCREEN_TO_FIXED(160)
#define INPUT_BUFFER_FRAMES 3
#define INVULNERABLE_FRAMES 150
#define RESPAWN_SAFE_DIST  SCREEN_TO_FIXED(64)
#define GHOST_TIMER         180
#define CRUISE_SPEED        (3 * COORD_SCALE)
#define CRUISE_BODY_LEN     5
#define CHORDING_WINDOW_FRAMES 6

// ============================================================
//  STRUCT DEFINITIONS
// ============================================================
typedef enum {
    DIR_UP = 0, DIR_DOWN = 1, DIR_LEFT = 2, DIR_RIGHT = 3, DIR_NONE = 4
} Direction;

typedef enum {
    ENT_PLAYER = 0, ENT_GRUNT, ENT_QUARK, ENT_HULK, ENT_BRAIN,
    ENT_SPHEROID, ENT_ENFORCER, ENT_HUMAN, ENT_LASER, ENT_TERROR,
    ENT_ELECTRODE, ENT_CRUISE, NUM_ENTITY_TYPES
} EntityType;

typedef struct {
    int width, height;
    uint8_t r, g, b, a;
    const char* name;
} SpriteData;

// One frame of text-art sprite data.
typedef struct {
    const uint8_t* const* rows;  /* h row pointers */
    int w, h;                   /* dimensions in text cells */
    int ox, oy;                 /* placement offset in screen pixels */
} SpriteFrame;

// Per-facing sprite descriptor.
//   frame_indices points at a hand-editable array of indices into
//   the parent SpriteSet's frames[] array.  This allows:
//     - Non-contiguous frame reuse across facings (no row duplication)
//     - Ping-pong / limp / custom-order walk cycles
//     - Different frame counts per facing (e.g. Walk N=3, Walk E=8)
//   offset_x / offset_y are per-cycle placement offsets in text-cell
//   units, added to the per-frame ox/oy at render time.  Set to 0
//   if not needed.
//
//   step_period is now SPATIAL STRIDE, not temporal.  There is no
//   per-tick animation advance; the displayed frame is chosen purely
//   from world position at render time (see entity_select_frame()).
//   For stride-cycling entities (e.g. Hulk), step_period = terminal
//   cells traversed per frame advance.  0 = unused (entity uses a
//   non-stride selector like Grunt's binary sub-row pick).
typedef struct {
    const int* frame_indices;  /* -> array of indices into frames[]      */
    int count;                 /* length of frame_indices[]                */
    int step_period;           /* spatial stride (cells/frame) or 0=unused */
    int offset_x, offset_y;    /* per-cycle placement offset (text cells)  */
    int scale_x, scale_y;      // per-axis stride multipliers (default 1,1)
                               // INVERTED: bigger scale = FASTER animation.
                               // Conceptually eff_stride = step_period / scale;
                               //  actual math multiplies cells by scale before
                               //  dividing by step_period to preserve sub-cell
                               //  precision in integer arithmetic.
                               // Editable via `btk -e`; persists in sprites.h
} FacingInfo;

// Top-level sprite definition: one per entity type.
typedef struct {
    const char* name;
    int total_frames;
    const SpriteFrame* frames;
    FacingInfo facing[4];  /* N=0, S=1, E=2, W=3 -- matches Direction enum */
} SpriteSet;

// Pull in real sprite data when available.  sprites.h #defines
// HAVE_SPRITE_<NAME> for each entity it provides a SpriteSet for;
// the stub definitions below are guarded by the same macros so
// they are skipped automatically when real data is present.
// If sprites.h is absent or doesn't define a sprite, the stub is used.
#include "sprites.h"

// Shorthand to get the pixel width/height of an entity from its sprite.
/* Collision dimensions in fixed-point (match wx/wy units).
   sprite_fallback_w/h holds screen-pixel sizes set at spawn time;
   SCREEN_TO_FIXED converts to the fixed-point coordinate system. */
#define ENTITY_W(e) SCREEN_TO_FIXED((e)->sprite_fallback_w)
#define ENTITY_H(e) SCREEN_TO_FIXED((e)->sprite_fallback_h)

typedef struct {
    int16_t wx, wy, tx, ty;
    int16_t vx, vy;
    int anim_frame;
    int target_idx;
    int16_t target_entity;
    int state;
    int16_t stgx, stgy;
    int16_t mtgx, mtgy;
    int target_period;
    int target_counter;
    int tick_period;
    int tick_phase;
    int tick_counter;
    int attitude_period;
    int attitude_phase;
    int attitude_counter;
    int fire_period;
    int fire_phase;
    int fire_counter;
    int move_dir, facing_dir;
    int spawn_count;
    int spawn_max;
    int age;
    int pushback_timer;
    int human_type;
    EntityType type;
    const SpriteSet* anim;      /* sprite animation set (NULL = use fallback) */
    int anim_counter;          /* Spheroid temporal walk-phase counter.
                                * Repurposed from a vestigial slot --
                                * previously unused, now drives the
                                * within-bank sub-frame advance for
                                * ENT_SPHEROID (see SPHEROID_WALK_FRAMES_PER_ADVANCE).
                                * Incremented per spheroid tick in
                                * update_entities(); read at render time
                                * via get_current_frame() -> entity_select_frame().
                                * Other entity types leave this at 0
                                * (their pickers ignore it). */
    int sprite_fallback_w;     /* width in screen pixels (used when anim is NULL) */
    int sprite_fallback_h;     /* height in screen pixels */
    bool active;
    bool onscreen;
} Entity;

typedef struct {
    int16_t wx, wy;
    int16_t vx, vy;
    int lives;
    bool active;
    EntityType type;
    const SpriteSet* anim;
    int anim_counter;        /* VESTIGIAL -- no temporal animation; kept for struct stability */
    int sprite_fallback_w;
    int sprite_fallback_h;
    int death_timer;
    int invulnerable_timer;
    int16_t ghost_x, ghost_y;
    int ghost_timer;
    int shot_buffer_timer;
    int shot_pending_vx, shot_pending_vy;
    int facing_dir;        /* current facing (Direction enum) */
    int anim_frame;        /* VESTIGIAL -- no temporal animation; frame is chosen positionally */
} Player;

typedef struct {
    int grunts, electrodes, hulks, brains, spheroids, quarks, mommies, daddies, mikeys;
} LevelWave;

typedef struct {
    char glyph[4];             // UTF-8 character (up to 3 bytes + null terminator)
    unsigned char r, g, b;     // Foreground RGB
    unsigned char br, bg, bb;  // Background RGB
} TextCell;

// ============================================================
//  GLOBALS
// ============================================================

// Add these after your sprite definitions, before the globals section:


// ============================================================
//  DECAL SYSTEM
// ============================================================
#define MAX_DECALS 32
#define DECAL_LAYER_FLOOR 0
#define DECAL_LAYER_OVERLAY 1

#define DECAL_SCORE_BONUS 0
#define DECAL_SQUISHED 1

typedef struct {
    bool active;
    int type;
    int layer;
    int ttl_frames;
    int max_ttl_frames;
    int start_frame;
    float world_x, world_y;  // Fixed-point world coordinates
    int param;               // Type-specific parameter (e.g., score value)
} Decal;

static Decal g_decals[MAX_DECALS];

// ASCII digit strings for score display
static const char* g_digit_chars[] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9"
};

// Squished human 3x3 pattern (Unicode diagonal crosses)
static const char* g_squished_pattern[3][3] = {
    {"\xe2\x95\xb2", " ", "\xe2\x95\xb1"},    //  ╲ ╱
    {" ", "\xe2\x95\xb3", " "},               //   ╳  
    {"\xe2\x95\xb1", " ", "\xe2\x95\xb2"}     //  ╱ ╲
};

#define GREEN "\x1b[32m"
#define RESET "\x1b[0m"

// Digit sprites 500-509 (0-9), 3x3 box-drawing characters
// Each string is exactly one character (1 to 3 bytes + null terminator)
static const char* digit_sprites[10][9] = {
    // 0
    {"┏", "━", "┓", "┃", "┃", "┃", "┗", "━", "┛"},
    // 1
    {"╺", "┓", " ", " ", "┃", " ", "╺", "┻", " "},
    // 2
    {"┏", "━", "┓", "┏", "━", "┛", "┗", "━", "╸"},
    // 3
    {"┏", "━", "┓", "╺", "━", "┫", "┗", "━", "┛"},
    // 4
    {"╻", " ", "╻", "┗", "━", "┫", " ", " ", "╹"},
    // 5
    {"┏", "━", "╸", "┗", "━", "┓", "┗", "━", "┛"},
    // 6
    {"┏", "━", "┓", "┣", "━", "┓", "┗", "━", "┛"},
    // 7
    {"┏", "━", "┓", " ", " ", "┃", " ", " ", "╹"},
    // 8
    {"┏", "━", "┓", "┣", "━", "┫", "┗", "━", "┛"},
    // 9
    {"┏", "━", "┓", "┗", "━", "┫", "┗", "━", "┛"}
};

static int g_autofire_vx = 0;
static int g_autofire_vy = 0;

// Add these for movement autorun
static int g_autorun_vx = 0;
static int g_autorun_vy = 0;

// --- Keychording: rapid two-keypress diagonal fire ---
typedef struct { int k1, k2, vx, vy; } ChordEntry;
static int g_chord_prev_key   = -1;
static int g_chord_prev_frame = -999;
static const ChordEntry g_chord_table[] = {
    /* middle pad */
    { 'i', 'j', -1, -1 },   /* NW */
    { 'i', 'l',  1, -1 },   /* NE */
    { ',', 'j', -1,  1 },   /* SW */
    { ',', 'l',  1,  1 },   /* SE */
    /* right pad */
    { '8', '4', -1, -1 },   /* NW */
    { '8', '6',  1, -1 },   /* NE */
    { '2', '4', -1,  1 },   /* SW */
    { '2', '6',  1,  1 },   /* SE */
};
#define NUM_CHORDS 8
static int g_move_chord_prev_key   = -1;
static int g_move_chord_prev_frame = -999;
static const ChordEntry g_move_chord_table[] = {
    { 'w', 'a', -1, -1 },   /* NW */
    { 'w', 'd',  1, -1 },   /* NE */
    { 'x', 'a', -1,  1 },   /* SW */
    { 'x', 'd',  1,  1 },   /* SE */
};
#define NUM_MOVE_CHORDS 4

static const LevelWave g_waves[40] = {
    { 5,  2,  0,  0, 0, 0, 1, 1, 0}, {17, 15,  5,  0, 1, 0, 1, 1, 1},
    {22, 25,  6,  0, 3, 0, 2, 2, 2}, {34, 25,  7,  0, 4, 0, 2, 2, 2},
    {20, 20,  0, 15, 1, 0,15, 0, 1}, {32, 25,  7,  0, 4, 0, 3, 3, 3},
    { 0,  0, 12,  0, 0,10, 4, 4, 4}, {35, 25,  8,  0, 5, 0, 3, 3, 3},
    {60,  0,  4,  0, 5, 0, 3, 3, 3}, {25, 20,  0, 20, 1, 0, 0,22, 0},
    {35, 25,  8,  0, 5, 0, 3, 3, 3}, { 0,  0, 13,  0, 0,12, 3, 3, 3},
    {35, 25,  8,  0, 5, 0, 3, 3, 3}, {27,  5, 20,  0, 2, 0, 5, 5, 5},
    {25, 20,  2, 20, 1, 0, 0, 0,22}, {35, 25,  3,  0, 5, 0, 3, 3, 3},
    { 0,  0, 14,  0, 0,12, 3, 3, 3}, {35, 25,  8,  0, 5, 0, 3, 3, 3},
    {70,  0,  3,  0, 5, 0, 3, 3, 3}, {25, 20,  2, 20, 2, 0, 8, 8, 8},
    {35, 25,  8,  0, 5, 0, 3, 3, 3}, { 0,  0, 15,  0, 0,12, 3, 3, 3},
    {35, 25,  8,  0, 5, 0, 3, 3, 3}, { 0,  0, 13,  0, 6, 7, 3, 3, 3},
    {25, 20,  1, 21, 1, 0,25, 0, 1}, {35, 25,  8,  0, 5, 0, 3, 3, 3},
    { 0,  0, 16,  0, 0,12, 3, 3, 3}, {35, 25,  8,  0, 5, 1, 3, 3, 3},
    {75,  0,  4,  0, 5, 1, 3, 3, 3}, {25, 20,  1, 22, 1, 1, 0,25, 0},
    {35, 25,  8,  0, 5, 1, 3, 3, 3}, { 0,  0, 16,  0, 0,13, 3, 3, 3},
    {35, 25,  8,  0, 5, 1, 3, 3, 3}, {30,  0, 25,  0, 2, 2, 3, 3, 3},
    {27, 15,  2, 23, 1, 2, 0, 0,25}, {35, 25,  8,  0, 5, 2, 3, 3, 3},
    { 0,  0, 16,  0, 0,14, 3, 3, 3}, {35, 25,  8,  0, 5, 2, 3, 3, 3},
    {80,  0,  6,  0, 5, 1, 3, 3, 3}, { 0,  0,  0,  0, 6, 0, 0, 0, 0},
};

static Player g_players[MAX_PLAYERS] = {0};
static int16_t g_cruise_body_x[MAX_ENTITIES][CRUISE_BODY_LEN];
static int16_t g_cruise_body_y[MAX_ENTITIES][CRUISE_BODY_LEN];
static int g_cruise_body_len[MAX_ENTITIES];
static int g_term_cols = 132;
static int g_term_rows = 50;

static int scorebump = 0;  // 0-100, decays each frame, boosts score color toward white

static Entity g_entities[MAX_ENTITIES];
static int16_t g_next[MAX_ENTITIES];
static int16_t g_prev[MAX_ENTITIES];
static int16_t g_list_head;
static int16_t g_list_tail;
static int16_t g_free_head;
static int g_player_count = 0;
static int g_laser_count = 0;
static int g_level = 1;
static bool g_game_over = false;
static int g_frame_count = 0;
static bool g_show_game_over = false;
static int g_score = 0;
static int g_rescue_count = 0;
static bool g_remove_laser[MAX_ENTITIES];
static bool g_remove_enemy[MAX_ENTITIES];

// Terminal Input State (replaces SDL_GetKeyboardState)
static uint8_t g_keys[256] = {0};
static uint8_t g_keys_prev[256] = {0};


// Text Buffer
static TextCell* text_buffer = NULL;

// Spatial Grid
#define GRID_CELL_SIZE 512
#define GRID_COLS 25
#define GRID_ROWS 19
typedef struct { int16_t entity_idx; int16_t next; } GridNode;
static GridNode g_grid_nodes[MAX_ENTITIES];
static int16_t g_grid_heads[GRID_COLS * GRID_ROWS];
static int16_t g_grid_free;

// --- Pixel dimensions for each entity type (used for collision, clamping, spawning) ---
// Order matches EntityType enum: Player, Grunt, Quark, Hulk, Brain, Spheroid, Enforcer, Human, Laser, Terror, Electrode, Cruise
static const int sprite_pixel_w[NUM_ENTITY_TYPES] = {
    20, 18, 20, 24, 22, 18, 20, 10, 8, 10, 16, 4
};
static const int sprite_pixel_h[NUM_ENTITY_TYPES] = {
    20, 18, 20, 24, 22, 18, 20, 22, 8, 10, 16, 4
};
#define PLAYER_SPRITE_W 20
#define PLAYER_SPRITE_H 20
#define LASER_SPRITE_W   8
#define LASER_SPRITE_H   8

// --- Stub frames: one 1x1 blank cell per entity type ---
// Placeholders until real sprite art is loaded from sprites.h.
static const uint8_t stub_cell_blank[10] = {
    ' ', 0, 0, 0,           /* space glyph, null-padded */
    0xff, 0xff, 0xff,       /* fg white */
    0x00, 0x00, 0x00        /* bg black */
};
static const uint8_t* const stub_frame_rows[1] = { stub_cell_blank };
static const SpriteFrame stub_frames[] = {
    { stub_frame_rows, 1, 1, 0, 0 },  /* [0]  Player */
    { stub_frame_rows, 1, 1, 0, 0 },  /* [1]  Grunt */
    { stub_frame_rows, 1, 1, 0, 0 },  /* [2]  Quark */
    { stub_frame_rows, 1, 1, 0, 0 },  /* [3]  Hulk */
    { stub_frame_rows, 1, 1, 0, 0 },  /* [4]  Brain */
    { stub_frame_rows, 1, 1, 0, 0 },  /* [5]  Spheroid */
    { stub_frame_rows, 1, 1, 0, 0 },  /* [6]  Enforcer */
    { stub_frame_rows, 1, 1, 0, 0 },  /* [7]  Human */
    { stub_frame_rows, 1, 1, 0, 0 },  /* [8]  Laser */
    { stub_frame_rows, 1, 1, 0, 0 },  /* [9]  Terror */
    { stub_frame_rows, 1, 1, 0, 0 },  /* [10] Electrode */
    { stub_frame_rows, 1, 1, 0, 0 },  /* [11] Cruise */
};
// Shared indices table for stub sprites (single frame, index 0).
static const int stub_indices[1] = { 0 };
// --- Stub SpriteSet definitions ---
// Facing: N S E W -- all point to frame index 0, count=1, no animation,
// zero per-cycle offset.  The stub_indices[] array is shared across all
// stubs because they all reference frame 0 of their respective frames[].
//
// Each stub is wrapped in #ifndef HAVE_SPRITE_<NAME> so that when
// sprites.h provides real sprite data for an entity, the matching stub
// is suppressed automatically -- no need to edit this file when adding
// a new sprite.  To enable real sprite rendering for an entity, run
// its .ans through build_sprites7 (which regenerates sprites.h with
// both the SpriteSet definition and the matching #define), then
// recompile bti.c.
#ifndef HAVE_SPRITE_PLAYER
static const SpriteSet sprite_player_set    = { "Player",    1, &stub_frames[0],  {{stub_indices,1,6,0,0,1,1},{stub_indices,1,6,0,0,1,1},{stub_indices,1,6,0,0,1,1},{stub_indices,1,6,0,0,1,1}} };
#endif
#ifndef HAVE_SPRITE_GRUNT
static const SpriteSet sprite_grunt_set     = { "Grunt",     1, &stub_frames[1],  {{stub_indices,1,8,0,0,1,1},{stub_indices,1,8,0,0,1,1},{stub_indices,1,8,0,0,1,1},{stub_indices,1,8,0,0,1,1}} };
#endif
#ifndef HAVE_SPRITE_QUARK
static const SpriteSet sprite_quark_set     = { "Quark",     1, &stub_frames[2],  {{stub_indices,1,4,0,0,1,1},{stub_indices,1,4,0,0,1,1},{stub_indices,1,4,0,0,1,1},{stub_indices,1,4,0,0,1,1}} };
#endif
#ifndef HAVE_SPRITE_HULK
static const SpriteSet sprite_hulk_set      = { "Hulk",      1, &stub_frames[3],  {{stub_indices,1,4,0,0,1,1},{stub_indices,1,4,0,0,1,1},{stub_indices,1,4,0,0,1,1},{stub_indices,1,4,0,0,1,1}} };
#endif
#ifndef HAVE_SPRITE_BRAIN
static const SpriteSet sprite_brain_set     = { "Brain",     1, &stub_frames[4],  {{stub_indices,1,4,0,0,1,1},{stub_indices,1,4,0,0,1,1},{stub_indices,1,4,0,0,1,1},{stub_indices,1,4,0,0,1,1}} };
#endif
#ifndef HAVE_SPRITE_SPHEROID
static const SpriteSet sprite_spheroid_set  = { "Spheroid",  1, &stub_frames[5],  {{stub_indices,1,2,0,0,1,1},{stub_indices,1,2,0,0,1,1},{stub_indices,1,2,0,0,1,1},{stub_indices,1,2,0,0,1,1}} };
#endif
#ifndef HAVE_SPRITE_ENFORCER
static const SpriteSet sprite_enforcer_set  = { "Enforcer",  1, &stub_frames[6],  {{stub_indices,1,2,0,0,1,1},{stub_indices,1,2,0,0,1,1},{stub_indices,1,2,0,0,1,1},{stub_indices,1,2,0,0,1,1}} };
#endif
#ifndef HAVE_SPRITE_HUMAN
static const SpriteSet sprite_human_set     = { "Human",     1, &stub_frames[7],  {{stub_indices,1,4,0,0,1,1},{stub_indices,1,4,0,0,1,1},{stub_indices,1,4,0,0,1,1},{stub_indices,1,4,0,0,1,1}} };
#endif
#ifndef HAVE_SPRITE_LASER
static const SpriteSet sprite_laser_set     = { "Laser",     1, &stub_frames[8],  {{stub_indices,1,1,0,0,1,1},{stub_indices,1,1,0,0,1,1},{stub_indices,1,1,0,0,1,1},{stub_indices,1,1,0,0,1,1}} };
#endif
#ifndef HAVE_SPRITE_TERROR
static const SpriteSet sprite_terror_set    = { "Terror",    1, &stub_frames[9],  {{stub_indices,1,1,0,0,1,1},{stub_indices,1,1,0,0,1,1},{stub_indices,1,1,0,0,1,1},{stub_indices,1,1,0,0,1,1}} };
#endif
#ifndef HAVE_SPRITE_ELECTRODE
static const SpriteSet sprite_electrode_set = { "Electrode", 1, &stub_frames[10], {{stub_indices,1,1,0,0,1,1},{stub_indices,1,1,0,0,1,1},{stub_indices,1,1,0,0,1,1},{stub_indices,1,1,0,0,1,1}} };
#endif
#ifndef HAVE_SPRITE_CRUISE
static const SpriteSet sprite_cruise_set    = { "Cruise",    1, &stub_frames[11], {{stub_indices,1,1,0,0,1,1},{stub_indices,1,1,0,0,1,1},{stub_indices,1,1,0,0,1,1},{stub_indices,1,1,0,0,1,1}} };
#endif

// Forward declarations
static Entity* spawn_entity(int16_t x, int16_t y, EntityType type);
static void spawn_player(int16_t x, int16_t y);
static void spawn_laser(int16_t x, int16_t y, int16_t vx, int16_t vy, Direction dir);
static void clamp_to_screen(int16_t* x, int16_t* y, int sprite_w, int sprite_h);
static int32_t dist_sq_world(int16_t x1, int16_t y1, int16_t x2, int16_t y2);
static void play_level_intro_text(void);
static void process_player_vs_entities(void);
static void init_text_mode(void);
static void fini_text_mode(void);
static void poll_input(void);

/* ---- Sprite editor forward declarations ----
 * The walk-table editor (`btk -e`) lives near the bottom of the file,
 * but get_current_frame() needs to consult its shadow walk tables and
 * scale multipliers so edits take effect in the live preview and in
 * the game itself.  These declarations break the circular dependency. */
#define EDITOR_MAX_WALK_LEN 64
#define EDITOR_MAX_FRAMES   32   /* Hulk currently has 23; 32 leaves headroom */

typedef struct {
    int  indices[EDITOR_MAX_WALK_LEN];
    int  count;
    int  scale_x, scale_y;   /* shadow copy of FacingInfo.scale_x/y */
    bool dirty;
} EditorWalk;

/* Per-frame placement-offset shadow.  Indexed by [entity_type][frame_idx].
 *
 * Mirrors SpriteFrame.ox / oy (the per-frame placement offset baked from
 * .ans OffsetX+HotspotX / OffsetY+HotspotY by build_sprites7.c).  The
 * editor lets the user tune these interactively with i/j/k/l keys; the
 * shadow is consulted by editor_render_walktable() so the live preview
 * reflects edits immediately, and on save the matching SpriteFrame
 * initializer line in sprites.h is rewritten with the new values.
 *
 * TODO / design note: FacingInfo also has offset_x/offset_y (per-facing
 * placement offsets), but those are almost always left at 0,0 in
 * practice.  The per-frame ox/oy on SpriteFrame is the offset that
 * actually matters -- it's tied to the shape of the sprite and the
 * amount of blank space around it relative to the normalized sprite
 * size, which is a per-frame property, not a per-facing one.  Consider
 * eliminating FacingInfo.offset_x/offset_y in a future cleanup pass
 * and consolidating all placement offsetting onto SpriteFrame.ox/oy.
 * The render path already sums both, so removing the per-facing pair
 * is a non-breaking change. */
typedef struct {
    int  ox, oy;   /* shadow copy of SpriteFrame.ox / oy */
    bool dirty;    /* set when user modifies; only dirty entries are saved */
} EditorFrameOffset;

static bool       g_editor_active = false;
static int        g_editor_entity = -1;     /* EntityType being edited */
static EditorWalk g_editor_walks[NUM_ENTITY_TYPES][4]; /* [type][N,S,E,W] */
static EditorFrameOffset g_editor_frame_offsets[NUM_ENTITY_TYPES][EDITOR_MAX_FRAMES];

// ============================================================
//  LINKED LIST & SPATIAL GRID MANAGEMENT
// ============================================================
static void init_lists(void) {
    g_free_head = -1; g_list_head = -1; g_list_tail = -1;
    for (int i = MAX_ENTITIES - 1; i >= 0; i--) {
        g_entities[i].active = false;
        g_next[i] = g_free_head; g_prev[i] = -1; g_free_head = i;
    }
    g_grid_free = -1;
    for (int i = MAX_ENTITIES - 1; i >= 0; i--) {
        g_grid_nodes[i].next = g_grid_free; g_grid_free = i;
    }
    for (int i = 0; i < GRID_COLS * GRID_ROWS; i++) g_grid_heads[i] = -1;
}

static int16_t alloc_entity(void) {
    if (g_free_head == -1) return -1;
    int16_t idx = g_free_head; g_free_head = g_next[idx]; return idx;
}

static void free_entity(int16_t idx) {
    g_entities[idx].active = false;
    g_next[idx] = g_free_head; g_prev[idx] = -1; g_free_head = idx;
}

static void unlink_entity(int16_t idx) {
    int16_t p = g_prev[idx], n = g_next[idx];
    if (p != -1) g_next[p] = n; else g_list_head = n;
    if (n != -1) g_prev[n] = p; else g_list_tail = p;
    g_next[idx] = -1; g_prev[idx] = -1;
}

static void link_entity(int16_t idx) {
    g_next[idx] = -1; g_prev[idx] = g_list_tail;
    if (g_list_tail != -1) g_next[g_list_tail] = idx; else g_list_head = idx;
    g_list_tail = idx;
}

static int16_t alloc_grid_node(void) {
    if (g_grid_free == -1) return -1;
    int16_t idx = g_grid_free; g_grid_free = g_grid_nodes[idx].next; return idx;
}

static void rebuild_grid(void) {
    g_grid_free = -1;
    for (int i = 0; i < MAX_ENTITIES; i++) { g_grid_nodes[i].next = g_grid_free; g_grid_free = i; }
    for (int i = 0; i < GRID_COLS * GRID_ROWS; i++) g_grid_heads[i] = -1;
    for (int16_t idx = g_list_head; idx != -1; idx = g_next[idx]) {
        Entity* e = &g_entities[idx];
        if (!e->active) continue;
        int cell_x = (int)e->wx / GRID_CELL_SIZE;
        int cell_y = (int)e->wy / GRID_CELL_SIZE;
        if (cell_x < 0) cell_x = 0;
        if (cell_x >= GRID_COLS) cell_x = GRID_COLS - 1;
        if (cell_y < 0) cell_y = 0;
        if (cell_y >= GRID_ROWS) cell_y = GRID_ROWS - 1;
        int cell = cell_y * GRID_COLS + cell_x;
        int16_t node_idx = alloc_grid_node();
        if (node_idx == -1) continue;
        g_grid_nodes[node_idx].entity_idx = idx;
        g_grid_nodes[node_idx].next = g_grid_heads[cell];
        g_grid_heads[cell] = node_idx;
    }
}

// ============================================================
//  TERMINAL & TEXT BUFFER FUNCTIONS
// ============================================================
typedef struct { int rows; int cols; } TermSize;

static TermSize get_terminal_size(void) {
    struct winsize w;
    TermSize size = {24, 80};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) != -1) { size.rows = w.ws_row; size.cols = w.ws_col; }
    return size;
}

static void init_terminal_size(void) {
    TermSize size = get_terminal_size();
    g_term_rows = size.rows; g_term_cols = size.cols;
    if (g_term_rows < 24) exit(1);
    if (g_term_cols < 40) exit(1);
    if (g_term_rows > 180) g_term_rows = 180;
    if (g_term_cols > 240) g_term_cols = 240;
}

static void init_text_mode(void) {
    struct termios term;
    tcgetattr(0, &term);
    term.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(0, TCSANOW, &term);
    printf("\x1b[?25l"); fflush(stdout);
}

static void fini_text_mode(void) {
    struct termios term;
    tcgetattr(0, &term);
    term.c_lflag |= (ICANON | ECHO);
    tcsetattr(0, TCSANOW, &term);
    printf("\x1b[?25h"); fflush(stdout);
}

static void clear_text_buffer(void) {
    if (!text_buffer) return;
    for (int y = 0; y < g_term_rows; y++) {
        for (int x = 0; x < g_term_cols; x++) {
            int idx = y * g_term_cols + x;
            strcpy(text_buffer[idx].glyph, " "); // Use strcpy for arrays
            text_buffer[idx].r = 0;
            text_buffer[idx].g = 0;
            text_buffer[idx].b = 0;
            text_buffer[idx].br = 0;
            text_buffer[idx].bg = 0;
            text_buffer[idx].bb = 0;
        }
    }
}
static void init_text_buffer(void) {
    if (text_buffer) free(text_buffer);
    text_buffer = malloc(g_term_rows * g_term_cols * sizeof(TextCell));
    if (!text_buffer) { fprintf(stderr, "Failed to allocate text buffer\n"); exit(1); }
    clear_text_buffer();
}

static void fini_text_buffer(void) {
    if (text_buffer) { free(text_buffer); text_buffer = NULL; }
}

// ============================================================
//  HALF-STEP RECT RENDERING (with 0.90 threshold)
// ============================================================
static const char* g_full   = "\xe2\x96\x88"; // █
static const char* g_right   = "\xe2\x96\x8c"; // ▌
static const char* g_left  = "\xe2\x96\x90"; // ▐
static const char* g_top    = "\xe2\x96\x84"; // ▄
static const char* g_bottom = "\xe2\x96\x80"; // ▀
static const char* g_nw     = "\xe2\x96\x97"; // ▗
static const char* g_ne     = "\xe2\x96\x96"; // ▖
static const char* g_sw     = "\xe2\x96\x9d"; // ▝
static const char* g_se     = "\xe2\x96\x98"; // ▘

static void draw_text_rect(float x, float y, float w, float h, unsigned char r, unsigned char g, unsigned char b) {
    int start_col = (int)x;
    int start_row = (int)y;
    int end_col = (int)(x + w);
    int end_row = (int)(y + h);

    if (start_col < 0) start_col = 0;
    if (start_row < 0) start_row = 0;
    if (end_col >= g_term_cols) end_col = g_term_cols - 1;
    if (end_row >= g_term_rows) end_row = g_term_rows - 1;

    for (int cy = start_row; cy <= end_row; cy++) {
        for (int cx = start_col; cx <= end_col; cx++) {
            float cell_left = cx;
            float cell_right = cx + 1.0f;
            float cell_top = cy;
            float cell_bottom = cy + 1.0f;

            float overlap_left = (x > cell_left) ? x : cell_left;
            float overlap_right = (x + w < cell_right) ? x + w : cell_right;
            float overlap_top = (y > cell_top) ? y : cell_top;
            float overlap_bottom = (y + h < cell_bottom) ? y + h : cell_bottom;

            if (overlap_left >= overlap_right || overlap_top >= overlap_bottom) continue;

            float fx = overlap_right - overlap_left;
            float fy = overlap_bottom - overlap_top;

            int idx = cy * g_term_cols + cx;
            const char* glyph_str = " ";
            bool is_left = (overlap_left > cell_left);
            bool is_top = (overlap_top > cell_top);

            if (fx >= 0.90f && fy >= 0.90f) {
                glyph_str = g_full;
            } else if (fx < 0.90f && fy >= 0.90f) {
                glyph_str = is_left ? g_left : g_right;
            } else if (fx >= 0.90f && fy < 0.90f) {
                glyph_str = is_top ? g_top : g_bottom;
            } else {
                if (is_left && is_top) glyph_str = g_nw;
                else if (!is_left && is_top) glyph_str = g_ne;
                else if (is_left && !is_top) glyph_str = g_sw;
                else glyph_str = g_se;
            }

            // Copy the string directly into the cell's buffer
            strcpy(text_buffer[idx].glyph, glyph_str);
            // Inside draw_text_rect, at the end of the loop:
            strcpy(text_buffer[idx].glyph, glyph_str);
            text_buffer[idx].r = r;
            text_buffer[idx].g = g;
            text_buffer[idx].b = b;
        }
    }
}
// 3x3 player icon for lives display (avoids UTF-8 byte-splitting)
// 3x3 player icon for lives display
static const char* player_lives_icon[9] = {
    " ", "█", " ",
    "█", "█", "█",
    "▀", " ", "▀"
};

static void draw_lives_text(int lives) {
    if (!text_buffer) return;
    
    // Cap display at 10 lives max
    int display_lives = lives;
    if (display_lives > 10) display_lives = 10;
    if (display_lives < 0) display_lives = 0;
    
    // MOVED LEFT: Subtracting 3 moves the entire block 3 columns away from the right edge.
    // (Adjust this number to -4, -5, etc., if you want it even further left)
    int rightmost_col = g_term_cols - 4; 
    
    for (int i = 0; i < display_lives; i++) {
        int start_col_for_this_life = rightmost_col - 2 - (display_lives - 1 - i) * 3;
        
        // MOVED DOWN: Start at row 1 instead of row 0
        for (int row = 1; row < 4; row++) {
            for (int col = 0; col < 3; col++) {
                int buf_col = start_col_for_this_life + col;
                int buf_idx = row * g_term_cols + buf_col;
                
                if (buf_idx >= 0 && buf_idx < g_term_rows * g_term_cols && buf_col >= 0 && buf_col < g_term_cols) {
                    // Use (row - 1) so we still correctly index the 0-8 array elements
                    snprintf(text_buffer[buf_idx].glyph, 4, "%s", player_lives_icon[(row - 1) * 3 + col]);
                    text_buffer[buf_idx].r = 255; // White
                    text_buffer[buf_idx].g = 255;
                    text_buffer[buf_idx].b = 255;
                }
            }
        }
    }
}

static void flush_text_buffer(void) {
    if (!text_buffer) return;
    printf("\x1b[H");
    unsigned char last_r = 255, last_g = 255, last_b = 255;
    unsigned char last_br = 0, last_bg = 0, last_bb = 0;
    for (int y = 0; y < g_term_rows; y++) {
        for (int x = 0; x < g_term_cols; x++) {
            TextCell cell = text_buffer[y * g_term_cols + x];
            if (cell.r != last_r || cell.g != last_g || cell.b != last_b) {
                printf("\x1b[38;2;%d;%d;%dm", cell.r, cell.g, cell.b);
                last_r = cell.r; last_g = cell.g; last_b = cell.b;
            }
            if (cell.br != last_br || cell.bg != last_bg || cell.bb != last_bb) {
                printf("\x1b[48;2;%d;%d;%dm", cell.br, cell.bg, cell.bb);
                last_br = cell.br; last_bg = cell.bg; last_bb = cell.bb;
            }
            printf("%s", cell.glyph);
        }
        if (y < g_term_rows - 1) putchar('\n');
    }
    printf("\x1b[0m");
    fflush(stdout);
}

// ============================================================
//  INPUT POLLING (replaces SDL_GetKeyboardState)
// ============================================================
static void poll_input(void) {
    memcpy(g_keys_prev, g_keys, sizeof(g_keys));
    memset(g_keys, 0, sizeof(g_keys));
    fd_set set;
    struct timeval timeout = {0, 0};
    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);
    while (select(STDIN_FILENO + 1, &set, NULL, NULL, &timeout) > 0) {
        char ch;
        if (read(STDIN_FILENO, &ch, 1) == 1) {
            if ((unsigned char)ch < 256) g_keys[(unsigned char)ch] = 1;
        }
    }
}

// ============================================================
//  HELPER FUNCTIONS
// ============================================================
static bool check_any_key_pressed(void) {
    fd_set set;
    struct timeval tv = {0, 0}; // Zero timeout = non-blocking poll
    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);
    
    if (select(STDIN_FILENO + 1, &set, NULL, NULL, &tv) > 0) {
        char ch;
        read(STDIN_FILENO, &ch, 1); // Consume the single byte, guaranteed not to block
        return true;
    }
    return false;
}

static void clamp_to_screen(int16_t* x, int16_t* y, int sprite_w, int sprite_h) {
    int16_t max_x = SCREEN_TO_FIXED(SCREEN_WIDTH - sprite_w);
    int16_t max_y = SCREEN_TO_FIXED(SCREEN_HEIGHT - sprite_h);
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    if (*x > max_x) *x = max_x;
    if (*y > max_y) *y = max_y;
}

static int32_t dist_sq_world(int16_t x1, int16_t y1, int16_t x2, int16_t y2) {
    int32_t dx = (int32_t)x1 - (int32_t)x2;
    int32_t dy = (int32_t)y1 - (int32_t)y2;
    return (dx * dx) + (dy * dy);
}

static int get_wall_bitmask(Entity* e) {
    int mask = 0;
    int max_x = SCREEN_TO_FIXED(SCREEN_WIDTH) - ENTITY_W(e);
    int max_y = SCREEN_TO_FIXED(SCREEN_HEIGHT) - ENTITY_H(e);
    int margin = SCREEN_TO_FIXED(WALL_MARGIN);
    if (e->wx <= margin) mask |= WALL_LEFT;
    if (e->wx >= max_x - margin) mask |= WALL_RIGHT;
    if (e->wy <= margin) mask |= WALL_TOP;
    if (e->wy >= max_y - margin) mask |= WALL_BOTTOM;
    return mask;
}

static bool is_position_safe(int16_t wx, int16_t wy) {
    Player* p = &g_players[0];
    if (!p->active) return true;
    int32_t dist_sq = dist_sq_world(wx, wy, p->wx, p->wy);
    int32_t exclusion_sq = (int32_t)EXCLUSION_RADIUS * (int32_t)EXCLUSION_RADIUS;
    return dist_sq > exclusion_sq;
}

static bool find_safe_spawn(int16_t* out_x, int16_t* out_y) {
    int16_t cx = SCREEN_TO_FIXED(SCREEN_WIDTH / 2);
    int16_t cy = SCREEN_TO_FIXED(SCREEN_HEIGHT / 2);
    bool safe = true;
    for (int16_t idx = g_list_head; idx != -1; idx = g_next[idx]) {
        Entity* e = &g_entities[idx];
        if (!e->active) continue;
        if (e->type == ENT_HUMAN || e->type == ENT_LASER || e->type == ENT_TERROR) continue;
        if (dist_sq_world(cx, cy, e->wx, e->wy) < RESPAWN_SAFE_DIST * RESPAWN_SAFE_DIST) { safe = false; break; }
    }
    if (safe) { *out_x = cx; *out_y = cy; return true; }
    for (int step = 1; step <= 10; step++) {
        for (int dir = 0; dir < 8; dir++) {
            int16_t tx = cx + (int16_t)(((step * 48) * COORD_SCALE) * ((dir == 0 || dir == 1 || dir == 7) ? 1 : (dir == 3 || dir == 4 || dir == 5) ? -1 : 0));
            int16_t ty = cy + (int16_t)(((step * 48) * COORD_SCALE) * ((dir == 1 || dir == 2 || dir == 3) ? 1 : (dir == 5 || dir == 6 || dir == 7) ? -1 : 0));
            clamp_to_screen(&tx, &ty, PLAYER_SPRITE_W, PLAYER_SPRITE_H);
            safe = true;
            for (int16_t idx = g_list_head; idx != -1; idx = g_next[idx]) {
                Entity* e = &g_entities[idx];
                if (!e->active) continue;
                if (e->type == ENT_HUMAN || e->type == ENT_LASER || e->type == ENT_TERROR) continue;
                if (dist_sq_world(tx, ty, e->wx, e->wy) < RESPAWN_SAFE_DIST * RESPAWN_SAFE_DIST) { safe = false; break; }
            }
            if (safe) { *out_x = tx; *out_y = ty; return true; }
        }
    }
    *out_x = cx; *out_y = cy; return false;
}

static void remove_entity(int16_t idx) { if (idx < 0) return; unlink_entity(idx); free_entity(idx); }

static int get_dir_from_vel(int16_t vx, int16_t vy) {
    if (abs(vx) > abs(vy)) return (vx > 0) ? DIR_RIGHT : DIR_LEFT;
    return (vy > 0) ? DIR_DOWN : DIR_UP;
}

static int16_t fixed_mul(int16_t a, int16_t b) { return (int16_t)(((int32_t)a * (int32_t)b) / 16384); }

static bool check_swept_collision(Entity* laser, Entity* enemy) {
    int16_t e_min_x = enemy->wx, e_max_x = enemy->wx + ENTITY_W(enemy);
    int16_t e_min_y = enemy->wy, e_max_y = enemy->wy + ENTITY_H(enemy);
    for (int i = 0; i <= 8; i++) {
        int16_t cx = laser->wx + (laser->vx * i) / 8;
        int16_t cy = laser->wy + (laser->vy * i) / 8;
        if (cx >= e_min_x && cx <= e_max_x && cy >= e_min_y && cy <= e_max_y) return true;
    }
    return false;
}

static bool check_player_enemy_collision(Player* p, Entity* e) {
    int16_t p_min_x = p->wx, p_max_x = p->wx + SCREEN_TO_FIXED(p->sprite_fallback_w);
    int16_t p_min_y = p->wy, p_max_y = p->wy + SCREEN_TO_FIXED(p->sprite_fallback_h);
    int16_t e_min_x = e->wx, e_max_x = e->wx + ENTITY_W(e);
    int16_t e_min_y = e->wy, e_max_y = e->wy + ENTITY_H(e);
    return (p_min_x < e_max_x && p_max_x > e_min_x && p_min_y < e_max_y && p_max_y > e_min_y);
}

static bool check_entity_entity_collision(Entity* a, Entity* b) {
    int16_t a_min_x = a->wx, a_max_x = a->wx + ENTITY_W(a);
    int16_t a_min_y = a->wy, a_max_y = a->wy + ENTITY_H(a);
    int16_t b_min_x = b->wx, b_max_x = b->wx + ENTITY_W(b);
    int16_t b_min_y = b->wy, b_max_y = b->wy + ENTITY_H(b);
    return (a_min_x < b_max_x && a_max_x > b_min_x && 
            a_min_y < b_max_y && a_max_y > b_min_y);
}

// ============================================================
//  ENTITY AI
// ============================================================
static void ai_human(Entity* e) {
    e->attitude_counter--;
    if (e->attitude_counter <= 0) {
        int dir = rand() % 5;
        if (dir < 4) {
            int16_t speed = COORD_SCALE;
            e->vx = (dir == DIR_LEFT) ? -speed : (dir == DIR_RIGHT) ? speed : 0;
            e->vy = (dir == DIR_UP) ? -speed : (dir == DIR_DOWN) ? speed : 0;
        } else { e->vx = 0; e->vy = 0; }
        e->attitude_counter = e->attitude_period + (rand() % e->attitude_period/4) - e->attitude_period/8;
    }
}

static void ai_grunt(Entity* e) {
    if (e->target_idx < 0 || e->target_idx >= g_player_count) e->target_idx = 0;
    Player* p = &g_players[e->target_idx];
    if (!p->active) return;
    int16_t target_x = (p->ghost_timer > 0) ? p->ghost_x : p->wx;
    int16_t target_y = (p->ghost_timer > 0) ? p->ghost_y : p->wy;
    int16_t dx = target_x - e->wx, dy = target_y - e->wy;
    int16_t step = GRUNT_SPEED;
    int16_t ndx = dx + (rand() % 32) - 16, ndy = dy + (rand() % 32) - 16;
    e->vx = (ndx > 0) ? step : (ndx < 0) ? -step : 0;
    e->vy = (ndy > 0) ? step : (ndy < 0) ? -step : 0;
    if (rand() % 100 < 5) {
        int dir = rand() % 4;
        e->vx = (dir == DIR_LEFT) ? -step : (dir == DIR_RIGHT) ? step : 0;
        e->vy = (dir == DIR_UP) ? -step : (dir == DIR_DOWN) ? step : 0;
    }
    if (rand() % 100 < GRUNT_SHYNESS) {
        int16_t shy_x = 0, shy_y = 0;
        int cell_x = (int)e->wx / GRID_CELL_SIZE, cell_y = (int)e->wy / GRID_CELL_SIZE;
        for (int dy2 = -1; dy2 <= 1; dy2++) {
            for (int dx2 = -1; dx2 <= 1; dx2++) {
                int cx = cell_x + dx2, cy = cell_y + dy2;
                if (cx < 0 || cx >= GRID_COLS || cy < 0 || cy >= GRID_ROWS) continue;
                int cell = cy * GRID_COLS + cx;
                for (int nidx = g_grid_heads[cell]; nidx != -1; nidx = g_grid_nodes[nidx].next) {
                    Entity* other = &g_entities[g_grid_nodes[nidx].entity_idx];
                    if (other == e || !other->active || other->type != ENT_GRUNT) continue;
                    int32_t sdx = (int32_t)e->wx - (int32_t)other->wx;
                    int32_t sdy = (int32_t)e->wy - (int32_t)other->wy;
                    int32_t sd = abs(sdx) + abs(sdy);
                    if (sd < SCREEN_TO_FIXED(60) && sd > 0) { shy_x += (sdx * COORD_SCALE) / sd; shy_y += (sdy * COORD_SCALE) / sd; }
                }
            }
        }
        e->vx += shy_x; e->vy += shy_y;
    }
}

static void ai_hulk(Entity* e) {
    if (e->target_entity >= 0) {
        Entity* target = &g_entities[e->target_entity];
        if (!target->active || target->type != ENT_HUMAN) e->target_entity = -1;
    }
    if (e->target_entity >= 0) {
        Entity* target = &g_entities[e->target_entity];
        if (e->target_counter > 0) { e->target_counter--; return; }
        int16_t dx = target->wx - e->wx, dy = target->wy - e->wy;
        if (abs(dx) > abs(dy)) { e->vx = (dx > 0) ? HULK_SPEED : -HULK_SPEED; e->vy = 0; }
        else { e->vx = 0; e->vy = (dy > 0) ? HULK_SPEED : -HULK_SPEED; }
        e->target_counter = e->target_period; return;
    }
    e->attitude_counter--;
    if (e->attitude_counter <= 0) {
        int dir = rand() % 4; int16_t spd = HULK_SPEED;
        e->vx = (dir == DIR_LEFT) ? -spd : (dir == DIR_RIGHT) ? spd : 0;
        e->vy = (dir == DIR_UP) ? -spd : (dir == DIR_DOWN) ? spd : 0;
        e->attitude_counter = e->attitude_period + (rand() % (e->attitude_period / 4)) - (e->attitude_period / 8);
    }
    int cell_x = (int)e->wx / GRID_CELL_SIZE, cell_y = (int)e->wy / GRID_CELL_SIZE;
    for (int dy2 = -3; dy2 <= 3; dy2++) {
        for (int dx2 = -3; dx2 <= 3; dx2++) {
            int cx = cell_x + dx2, cy = cell_y + dy2;
            if (cx < 0 || cx >= GRID_COLS || cy < 0 || cy >= GRID_ROWS) continue;
            int cell = cy * GRID_COLS + cx;
            for (int nidx = g_grid_heads[cell]; nidx != -1; nidx = g_grid_nodes[nidx].next) {
                Entity* other = &g_entities[g_grid_nodes[nidx].entity_idx];
                if (!other->active || other->type != ENT_HUMAN) continue;
                int32_t dist = abs((int32_t)other->wx - (int32_t)e->wx) + abs((int32_t)other->wy - (int32_t)e->wy);
                if (dist < SCREEN_TO_FIXED(192)) {
                    e->target_entity = g_grid_nodes[nidx].entity_idx; e->target_counter = e->target_period;
                    int16_t dx = other->wx - e->wx, dy = other->wy - e->wy;
                    if (abs(dx) > abs(dy)) { e->vx = (dx > 0) ? HULK_SPEED : -HULK_SPEED; e->vy = 0; }
                    else { e->vx = 0; e->vy = (dy > 0) ? HULK_SPEED : -HULK_SPEED; }
                    return;
                }
            }
        }
    }
}

static void ai_quark(Entity* e) {
    int mask = get_wall_bitmask(e);
    int32_t margin = SCREEN_TO_FIXED(100);
    if (mask & WALL_LEFT) { int32_t dist = e->wx; if (dist < margin) e->vx += (int16_t)((margin - dist) * COORD_SCALE / margin)*10; }
    if (mask & WALL_RIGHT) { int32_t dist = SCREEN_TO_FIXED(SCREEN_WIDTH) - e->wx; if (dist < margin) e->vx -= (int16_t)((margin - dist) * COORD_SCALE / margin)*10; }
    if (mask & WALL_TOP) { int32_t dist = e->wy; if (dist < margin) e->vy += (int16_t)((margin - dist) * COORD_SCALE / margin)*10; }
    if (mask & WALL_BOTTOM) { int32_t dist = SCREEN_TO_FIXED(SCREEN_HEIGHT) - e->wy; if (dist < margin) e->vy -= (int16_t)((margin - dist) * COORD_SCALE / margin)*10; }
    int16_t max_speed = QUARK_SPEED * COORD_SCALE;
    if (abs(e->vx) > max_speed) e->vx = (e->vx > 0) ? max_speed : -max_speed;
    if (abs(e->vy) > max_speed) e->vy = (e->vy > 0) ? max_speed : -max_speed;
}

static void ai_spheroid(Entity* e) {
    if (e->state == SPHEROID_STATE_PAUSE) {
        e->target_counter--;
        if (e->target_counter <= 0) {
            e->state = SPHEROID_STATE_MOVE; e->fire_counter = 0;
            int16_t spd = SPHEROID_SPEED * COORD_SCALE;
            int16_t dx = (rand() % 2000) - 1000, dy = (rand() % 2000) - 1000;
            int16_t hw = SCREEN_TO_FIXED(SCREEN_WIDTH / 2), hh = SCREEN_TO_FIXED(SCREEN_HEIGHT / 2);
            if (e->wx < hw) dx = abs(dx); else dx = -abs(dx);
            if (e->wy < hh) dy = abs(dy); else dy = -abs(dy);
            int16_t mag = (abs(dx) > abs(dy)) ? abs(dx) : abs(dy);
            if (mag == 0) { dx = 500; dy = 800; mag = 800; }
            e->vx = (dx * spd) / mag; e->vy = (dy * spd) / mag;
        }
        return;
    }
    if (e->vx == 0 && e->vy == 0) {
        int16_t spd = SPHEROID_SPEED * COORD_SCALE / 2;
        e->vx = (rand() % 2) ? spd : -spd; e->vy = (rand() % 2) ? spd : -spd; e->fire_counter = 0;
    }
    if (e->fire_counter < SPHEROID_RAMP_TIME) { e->vx = (e->vx * 11) / 10; e->vy = (e->vy * 11) / 10; e->fire_counter++; }
    else {
        e->vx += (rand() % (SPHEROID_NUDGE_RANGE * 2 + 1)) - SPHEROID_NUDGE_RANGE;
        e->vy += (rand() % (SPHEROID_NUDGE_RANGE * 2 + 1)) - SPHEROID_NUDGE_RANGE;
        int16_t wall_x_min = SCREEN_TO_FIXED(SCREEN_WIDTH * 12 / 100), wall_x_max = SCREEN_TO_FIXED(SCREEN_WIDTH * 88 / 100);
        int16_t wall_y_min = SCREEN_TO_FIXED(SCREEN_HEIGHT * 12 / 100), wall_y_max = SCREEN_TO_FIXED(SCREEN_HEIGHT * 88 / 100);
        if (e->wx < wall_x_min || e->wx > wall_x_max) { e->vy = (e->vy * 10) / 9; e->vx = (e->vx * 8) / 10; }
        if (e->wy < wall_y_min || e->wy > wall_y_max) { e->vx = (e->vx * 10) / 9; e->vy = (e->vy * 8) / 10; }
    }
    int16_t max_speed = SPHEROID_SPEED * COORD_SCALE;
    int32_t mag = (int32_t)sqrt((double)e->vx * e->vx + (double)e->vy * e->vy);
    if (mag > max_speed && mag > 0) { e->vx = (e->vx * max_speed) / mag; e->vy = (e->vy * max_speed) / mag; }
    int cd = SCREEN_TO_FIXED(SPHEROID_CORNER_DIST);
    bool in_corner = (e->wx < cd && e->wy < cd) || (e->wx > SCREEN_TO_FIXED(SCREEN_WIDTH) - cd && e->wy < cd) || (e->wx < cd && e->wy > SCREEN_TO_FIXED(SCREEN_HEIGHT) - cd) || (e->wx > SCREEN_TO_FIXED(SCREEN_WIDTH) - cd && e->wy > SCREEN_TO_FIXED(SCREEN_HEIGHT) - cd);
    if (in_corner) {
        e->vx = (e->vx * 9) / 10; e->vy = (e->vy * 9) / 10;
        if (abs(e->vx) < 2 && abs(e->vy) < 2) {
            e->vx = 0; e->vy = 0;
            if (e->spawn_count < e->spawn_max) {
                int count = 1 + (rand() % 4);
                for (int i = 0; i < count && e->spawn_count < e->spawn_max; i++) { Entity* enf = spawn_entity(e->wx, e->wy, ENT_ENFORCER); if (enf) e->spawn_count++; }
            }
            e->state = SPHEROID_STATE_PAUSE; e->target_counter = SPHEROID_REST_TIME;
        }
    }
}

static void ai_enforcer(Entity* e) {
    Player* p = &g_players[0];
    if (!p->active) return;
    int16_t dx = p->wx - e->wx, dy = p->wy - e->wy;
    int32_t dist = abs(dx) + abs(dy);
    if (dist == 0) dist = 1;
    int16_t perp_x = -dy, perp_y = dx;
    int32_t perp_dist = abs(perp_x) + abs(perp_y);
    if (perp_dist == 0) perp_dist = 1;
    perp_x = (perp_x * 256) / perp_dist; perp_y = (perp_y * 256) / perp_dist;
    int16_t nudge = COORD_SCALE;
    e->vx += (perp_x * nudge) / 256; e->vy += (perp_y * nudge) / 256;
    int16_t max_speed = ENFORCER_SPEED * COORD_SCALE;
    if (abs(e->vx) > max_speed) e->vx = (e->vx > 0) ? max_speed : -max_speed;
    if (abs(e->vy) > max_speed) e->vy = (e->vy > 0) ? max_speed : -max_speed;
    e->fire_counter--;
    if (e->fire_counter <= 0) {
        int count = 1 + (rand() % 4);
        for (int i = 0; i < count; i++) {
            Entity* terror = spawn_entity(e->wx, e->wy, ENT_TERROR);
            if (terror) {
                int16_t target_x = (p->ghost_timer > 0) ? p->ghost_x : p->wx;
                int16_t target_y = (p->ghost_timer > 0) ? p->ghost_y : p->wy;
                int16_t tdx = target_x - terror->wx, tdy = target_y - terror->wy;
                int16_t tdist = abs(tdx) + abs(tdy);
                int16_t speed = 6 * COORD_SCALE;
                if (tdist == 0) tdist = 1;
                terror->vx = (tdx * speed) / tdist + (rand() % 7 - 3);
                terror->vy = (tdy * speed) / tdist + (rand() % 7 - 3);
            }
        }
        e->fire_counter = e->fire_period + (rand() % 30);
    }
    int cell_x = (int)e->wx / GRID_CELL_SIZE, cell_y = (int)e->wy / GRID_CELL_SIZE;
    for (int dy2 = -1; dy2 <= 1; dy2++) {
        for (int dx2 = -1; dx2 <= 1; dx2++) {
            int cx = cell_x + dx2, cy = cell_y + dy2;
            if (cx < 0 || cx >= GRID_COLS || cy < 0 || cy >= GRID_ROWS) continue;
            int cell = cy * GRID_COLS + cx;
            for (int nidx = g_grid_heads[cell]; nidx != -1; nidx = g_grid_nodes[nidx].next) {
                Entity* other = &g_entities[g_grid_nodes[nidx].entity_idx];
                if (other == e || !other->active || other->type != ENT_ENFORCER) continue;
                int32_t sdx = (int32_t)e->wx - (int32_t)other->wx, sdy = (int32_t)e->wy - (int32_t)other->wy;
                int32_t sd = abs(sdx) + abs(sdy);
                if (sd < SCREEN_TO_FIXED(80) && sd > 0) { int16_t repel = COORD_SCALE / 2; e->vx += (sdx * repel) / sd; e->vy += (sdy * repel) / sd; }
            }
        }
    }
}

// ============================================================
//  SPAWNING & LEVEL MANAGEMENT
// ============================================================
static Entity* spawn_entity(int16_t wx, int16_t wy, EntityType type) {
    int16_t idx = alloc_entity();
    if (idx == -1) { fprintf(stderr, "BLOCKED: entity pool full\n"); return NULL; }
    Entity* e = &g_entities[idx];
    e->wx = wx; e->wy = wy; e->tx = 0; e->ty = 0; e->vx = 0; e->vy = 0;
    e->target_idx = -1; e->target_period = 0; e->target_counter = 0; e->type = type;
    e->tick_period = 1; e->tick_counter = 0; e->tick_phase = 0; e->active = true;
    e->attitude_counter = 0; e->anim_frame = 0; e->anim_counter = 0; e->facing_dir = DIR_DOWN; e->move_dir = DIR_DOWN;
    e->anim = NULL; e->sprite_fallback_w = sprite_pixel_w[type]; e->sprite_fallback_h = sprite_pixel_h[type];
    e->age = 0; e->pushback_timer = 0; e->human_type = 0; e->spawn_count = 0; e->fire_phase = 0;
    switch (type) {
        case ENT_HUMAN: e->anim = &sprite_human_set; e->tick_period = 2; e->attitude_period = 128; e->attitude_counter = 128; e->human_type = rand() % 3; break;
        case ENT_GRUNT: e->anim = &sprite_grunt_set; e->tick_period = 23; e->tick_phase = rand() % e->tick_period; break;
        case ENT_HULK: e->anim = &sprite_hulk_set; e->tick_period = 4; e->attitude_period = 45; e->attitude_counter = rand() % e->attitude_period; e->target_entity = -1; e->target_period = 8; e->target_counter = 0; break;
        case ENT_SPHEROID: e->anim = &sprite_spheroid_set; e->tick_period = 2; e->attitude_period = 4; e->attitude_counter = rand() % 4; e->fire_period = 128; e->fire_counter = 128; e->spawn_count = 0; e->spawn_max = 4; e->state = SPHEROID_STATE_MOVE; e->target_counter = 0; e->target_period = SPHEROID_WINDUP_TICKS;
            /* Spheroid walk-cycle phase init: randomize both the
             * sub-divider (tick_counter) and the sub-frame index
             * (anim_counter) so a swarm of spheroids spawned in the
             * same frame doesn't sync up into a mechanical lockstep
             * march.  Each spheroid breathes on its own phase.
             * Range: tick_counter in [0, FRAMES_PER_ADVANCE-1],
             *        anim_counter in [0, 2] (one of the 3 sub-frames). */
            e->tick_counter = rand() % SPHEROID_WALK_FRAMES_PER_ADVANCE;
            e->anim_counter = rand() % 3;
            { int corner = rand() % 4; switch (corner) { case 0: e->mtgx = 0; e->mtgy = 0; break; case 1: e->mtgx = SCREEN_TO_FIXED(SCREEN_WIDTH); e->mtgy = 0; break; case 2: e->mtgx = 0; e->mtgy = SCREEN_TO_FIXED(SCREEN_HEIGHT); break; case 3: e->mtgx = SCREEN_TO_FIXED(SCREEN_WIDTH); e->mtgy = SCREEN_TO_FIXED(SCREEN_HEIGHT); break; } int16_t dx = e->mtgx - e->wx, dy = e->mtgy - e->wy; int32_t dist = abs(dx) + abs(dy); if (dist == 0) dist = 1; int16_t sspd = SPHEROID_SPEED * COORD_SCALE; e->vx = (int16_t)(((int32_t)dx * sspd) / dist); e->vy = (int16_t)(((int32_t)dy * sspd) / dist); } break;
        case ENT_ENFORCER: e->anim = &sprite_enforcer_set; e->attitude_period = 20; e->attitude_counter = 20; e->fire_period = 40; e->fire_counter = 40; e->tick_period = 2; e->tick_phase = 0; { int mask = get_wall_bitmask(e); int16_t speed = ENFORCER_SPEED * COORD_SCALE; e->vx = speed + rand() % speed * (((mask & WALL_LEFT) != 0) - ((mask & WALL_RIGHT) != 0)); e->vy = speed + rand() % speed * (((mask & WALL_TOP) != 0) - ((mask & WALL_BOTTOM) != 0)); } break;
        case ENT_BRAIN: e->anim = &sprite_brain_set; e->tick_period = 4; e->tick_phase = rand() % e->tick_period; e->tick_counter = e->tick_period; e->attitude_period = 32; e->attitude_counter = e->attitude_period; e->fire_period = 32 + (rand() % 32); break;
        case ENT_CRUISE: e->anim = &sprite_cruise_set; e->tick_period = 1; e->attitude_counter = 20; break;
        case ENT_TERROR: e->anim = &sprite_terror_set; e->tick_period = 1; break;
        case ENT_LASER: e->anim = &sprite_laser_set; e->tick_period = 1; break;
        case ENT_ELECTRODE: e->anim = &sprite_electrode_set; e->tick_period = 9999999; break;
        case ENT_QUARK: e->anim = &sprite_quark_set; e->tick_period = 4; break;
        default: e->active = false; free_entity(idx); return NULL;
    }
    link_entity(idx); return e;
}

static void spawn_player(int16_t wx, int16_t wy) {
    if (g_player_count >= MAX_PLAYERS) return;
    Player* p = &g_players[g_player_count++];
    p->wx = wx; p->wy = wy; p->vx = 0; p->vy = 0; p->type = ENT_PLAYER;
    p->anim = &sprite_player_set; p->anim_counter = 0; p->sprite_fallback_w = PLAYER_SPRITE_W; p->sprite_fallback_h = PLAYER_SPRITE_H; p->active = true; p->lives = PLAYER_LIVES;
    p->death_timer = 0; p->invulnerable_timer = 0; p->ghost_x = 0; p->ghost_y = 0;
    p->ghost_timer = 0; p->shot_buffer_timer = 0; p->shot_pending_vx = 0; p->shot_pending_vy = 0;
    p->facing_dir = DIR_DOWN; p->anim_frame = 0;
}

static void spawn_laser(int16_t x, int16_t y, int16_t vx, int16_t vy, Direction dir) {
    (void)dir;
    if (g_laser_count >= MAX_LASERS) return;
    Entity* l = spawn_entity(x, y, ENT_LASER);
    if (!l) {
        return; 
    } 
    l->vx = vx; 
    l->vy = vy; 
    g_laser_count++;
}

static void reset_level(void) {
    while (g_list_head != -1) { int16_t idx = g_list_head; unlink_entity(idx); free_entity(idx); }
    g_laser_count = 0;
    int16_t spawn_x, spawn_y; find_safe_spawn(&spawn_x, &spawn_y);
    g_players[0].wx = spawn_x; g_players[0].wy = spawn_y; g_players[0].vx = 0; g_players[0].vy = 0; g_players[0].death_timer = 0;
    g_rescue_count = 0;   /* reset human-rescue streak at start of each wave */
    g_level++;
    int wave_idx = (g_level - 1) % 40;
    const LevelWave* w = &g_waves[wave_idx];
    #define SPAWN_WITH_SAFETY(count, type) for (int _i = 0; _i < (count); _i++) { int _attempts = 0; int16_t _x, _y; do { _x = rand() % SCREEN_TO_FIXED(SCREEN_WIDTH); _y = rand() % SCREEN_TO_FIXED(SCREEN_HEIGHT); _attempts++; if (_attempts > 50) break; } while (!is_position_safe(_x, _y)); spawn_entity(_x, _y, (type)); }
    SPAWN_WITH_SAFETY(w->grunts, ENT_GRUNT) SPAWN_WITH_SAFETY(w->electrodes, ENT_ELECTRODE) SPAWN_WITH_SAFETY(w->hulks, ENT_HULK) SPAWN_WITH_SAFETY(w->brains, ENT_BRAIN) SPAWN_WITH_SAFETY(w->spheroids, ENT_SPHEROID) SPAWN_WITH_SAFETY(w->quarks, ENT_QUARK)
    #undef SPAWN_WITH_SAFETY
    for (int i = 0; i < w->mommies; i++) { int attempts = 0; int16_t x, y; do { x = rand() % SCREEN_TO_FIXED(SCREEN_WIDTH); y = rand() % SCREEN_TO_FIXED(SCREEN_HEIGHT); attempts++; if (attempts > 50) break; } while (!is_position_safe(x, y)); Entity* h = spawn_entity(x, y, ENT_HUMAN); if (h) h->human_type = 0; }
    for (int i = 0; i < w->daddies; i++) { int attempts = 0; int16_t x, y; do { x = rand() % SCREEN_TO_FIXED(SCREEN_WIDTH); y = rand() % SCREEN_TO_FIXED(SCREEN_HEIGHT); attempts++; if (attempts > 50) break; } while (!is_position_safe(x, y)); Entity* h = spawn_entity(x, y, ENT_HUMAN); if (h) h->human_type = 1; }
    for (int i = 0; i < w->mikeys; i++) { int attempts = 0; int16_t x, y; do { x = rand() % SCREEN_TO_FIXED(SCREEN_WIDTH); y = rand() % SCREEN_TO_FIXED(SCREEN_HEIGHT); attempts++; if (attempts > 50) break; } while (!is_position_safe(x, y)); Entity* h = spawn_entity(x, y, ENT_HUMAN); if (h) h->human_type = 2; }
}

// -----------____DECALS____----------
//
static void init_decals(void) {
    for (int i = 0; i < MAX_DECALS; i++) {
        g_decals[i].active = false;
    }
}

static int spawn_decal(int type, int16_t wx, int16_t wy, int ttl, int param, int layer) {
    // Find inactive slot
    for (int i = 0; i < MAX_DECALS; i++) {
        if (!g_decals[i].active) {
            g_decals[i].active = true;
            g_decals[i].type = type;
            g_decals[i].layer = layer;
            g_decals[i].ttl_frames = ttl;
            g_decals[i].max_ttl_frames = ttl;
            g_decals[i].start_frame = g_frame_count;
            g_decals[i].world_x = (float)wx;
            g_decals[i].world_y = (float)wy;
            g_decals[i].param = param;
            return i;
        }
    }
    return -1;  // Pool full
}

static void update_decals(void) {
    for (int i = 0; i < MAX_DECALS; i++) {
        if (g_decals[i].active) {
            g_decals[i].ttl_frames--;
            if (g_decals[i].ttl_frames <= 0) {
                g_decals[i].active = false;
            }
        }
    }
}

// Draw a string at world coordinates
static void draw_text_string(float wx, float wy, const char* str, unsigned char r, unsigned char g, unsigned char b) {
    float tx = (wx / COORD_SCALE) * g_term_cols / SCREEN_WIDTH;
    float ty = (wy / COORD_SCALE) * g_term_rows / SCREEN_HEIGHT;
    int start_col = (int)tx;
    int start_row = (int)ty;
    if (start_col < 0 || start_row < 0) return;
    
    for (int i = 0; str[i] != '\0'; i++) {
        int col = start_col + i;
        if (col >= g_term_cols) break;
        int idx = start_row * g_term_cols + col;
        if (idx >= 0 && idx < g_term_rows * g_term_cols) {
            // Use snprintf to safely copy the string into the char array
            if (str[i] >= '0' && str[i] <= '9') {
                snprintf(text_buffer[idx].glyph, 4, "%s", g_digit_chars[str[i] - '0']);
            } else {
                snprintf(text_buffer[idx].glyph, 4, "%s", " ");
            }
            text_buffer[idx].r = r; 
            text_buffer[idx].g = g; 
            text_buffer[idx].b = b;
        }
    }
}

// Draw floor layer decals (under entities)
static void draw_floor_decals(void) {
    for (int i = 0; i < MAX_DECALS; i++) {
        if (!g_decals[i].active || g_decals[i].layer != DECAL_LAYER_FLOOR) continue;
        Decal* d = &g_decals[i];
        if (d->type == DECAL_SQUISHED) {
            float tx = (d->world_x / COORD_SCALE) * g_term_cols / SCREEN_WIDTH;
            float ty = (d->world_y / COORD_SCALE) * g_term_rows / SCREEN_HEIGHT;
            int start_col = (int)tx;
            int start_row = (int)ty;
            int pulse = (g_frame_count / 6) % 2;
            unsigned char c = (pulse == 0) ? 128 : 255;
            
            for (int row = 0; row < 3; row++) {
                for (int col = 0; col < 3; col++) {
                    int buf_col = start_col + col;
                    int buf_row = start_row + row;
                    if (buf_col >= 0 && buf_col < g_term_cols && buf_row >= 0 && buf_row < g_term_rows) {
                        int idx = buf_row * g_term_cols + buf_col;
                        // Use snprintf to safely copy the string into the char array
                        snprintf(text_buffer[idx].glyph, 4, "%s", g_squished_pattern[row][col]);
                        text_buffer[idx].r = c; 
                        text_buffer[idx].g = c; 
                        text_buffer[idx].b = c;
                    }
                }
            }
        }
    }
}

// Draw overlay layer decals (above entities)
static void draw_overlay_decals(void) {
    for (int i = 0; i < MAX_DECALS; i++) {
        if (!g_decals[i].active || g_decals[i].layer != DECAL_LAYER_OVERLAY) continue;
        
        Decal* d = &g_decals[i];
        
        if (d->type == DECAL_SCORE_BONUS) {
            // Rainbow color cycling (6 phases, change every 4 frames)
            int phase = (g_frame_count / 4) % 6;
            unsigned char r, g, b;
            
            switch (phase) {
                case 0: r = 255; g = 0;   b = 0;   break;  // Red
                case 1: r = 255; g = 127; b = 0;   break;  // Orange
                case 2: r = 255; g = 255; b = 0;   break;  // Yellow
                case 3: r = 0;   g = 255; b = 0;   break;  // Green
                case 4: r = 0;   g = 0;   b = 255; break;  // Blue
                case 5: r = 127; g = 0;   b = 255; break;  // Purple
                default: r = 255; g = 255; b = 255; break;
            }
            
            // Format score as string
            char score_str[8];
            snprintf(score_str, sizeof(score_str), "%d", d->param);
            
            // Draw at world position
            draw_text_string(d->world_x, d->world_y, score_str, r, g, b);
        }
    }
}

// ============================================================
//  UPDATE (using g_keys[] instead of SDL)
// ============================================================
static void update_player(Player* p) {
    if (!p->active || g_game_over) return;
    
    // 1. Movement Input (Autorun via wqexadc, stop with s)
    p->vx = 0; p->vy = 0;
    int run_vx = 0, run_vy = 0;
    bool run_pressed = false;
    
    #define CHECK_RUN(key, dx, dy) \
        if (g_keys[(unsigned char)(key)]) { run_vx = dx; run_vy = dy; run_pressed = true; }
        
    CHECK_RUN('w',  0, -1) // Up
    CHECK_RUN('x',  0,  1) // Down
    CHECK_RUN('a', -1,  0) // Left
    CHECK_RUN('d',  1,  0) // Right
    CHECK_RUN('q', -1, -1) // Up-Left
    CHECK_RUN('e',  1, -1) // Up-Right
    CHECK_RUN('z', -1,  1) // Down-Left
    CHECK_RUN('c',  1,  1) // Down-Right
    
    // --- Movement keychording: rapid same-pad cardinal pairs ---
    {
        int chord_hit = 0;
        for (int c = 0; c < NUM_MOVE_CHORDS && !chord_hit; c++) {
            int k1 = g_move_chord_table[c].k1, k2 = g_move_chord_table[c].k2;
            if ((g_keys[k1] && !g_keys_prev[k1] && g_move_chord_prev_key == k2) ||
                (g_keys[k2] && !g_keys_prev[k2] && g_move_chord_prev_key == k1)) {
                if ((g_frame_count - g_move_chord_prev_frame) <= CHORDING_WINDOW_FRAMES) {
                    run_vx = g_move_chord_table[c].vx;
                    run_vy = g_move_chord_table[c].vy;
                    run_pressed = true;
                    chord_hit = 1;
                } 
            } 
        }
        if (chord_hit) {
            g_move_chord_prev_key = -1;
        } else {  
            for (int c = 0; c < NUM_MOVE_CHORDS; c++) {
                int k1 = g_move_chord_table[c].k1, k2 = g_move_chord_table[c].k2;
                if (g_keys[k1] && !g_keys_prev[k1]) { 
                    g_move_chord_prev_key = k1; g_move_chord_prev_frame = g_frame_count; break; }
                if (g_keys[k2] && !g_keys_prev[k2]) { 
                    g_move_chord_prev_key = k2; g_move_chord_prev_frame = g_frame_count; break; }
            } 
        }
    }
    // 's' stops autorun
    if (g_keys['s']) {
        g_autorun_vx = 0;
        g_autorun_vy = 0;
    } else if (run_pressed) {
        g_autorun_vx = run_vx;
        g_autorun_vy = run_vy;
    }
    
    // Apply autorun velocity
    p->vx = g_autorun_vx * PLAYER_SPEED;
    p->vy = g_autorun_vy * PLAYER_SPEED;
    
    // Normalize diagonal movement speed
    if (p->vx != 0 && p->vy != 0) { 
        p->vx = fixed_mul(p->vx, FIXED_0_707); 
        p->vy = fixed_mul(p->vy, FIXED_0_707); 
    }
    // Update facing direction from movement
    if (g_autorun_vx != 0 || g_autorun_vy != 0) {
        p->facing_dir = get_dir_from_vel(p->vx, p->vy);
    }
    // (No temporal animation advance -- sprite frames are now chosen
    // positionally at render time.  See entity_select_frame().)
    // 2. Firing Input (Autofire via Numpad OR Synonym Keys)
    // Numpad:  7 8 9
    //          4 5 6
    //          1 2 3
    // Synonyms:u i o
    //          j k l
    //          m , .
    
    int fire_vx = 0, fire_vy = 0;
    bool fire_pressed = false;
    
    #define CHECK_FIRE(key, dx, dy) \
        if (g_keys[(unsigned char)(key)]) { fire_vx = dx; fire_vy = dy; fire_pressed = true; }
    
    // Numpad keys
    CHECK_FIRE('7', -1, -1)
    CHECK_FIRE('8',  0, -1)
    CHECK_FIRE('9',  1, -1)
    CHECK_FIRE('4', -1,  0)
    CHECK_FIRE('6',  1,  0)
    CHECK_FIRE('1', -1,  1)
    CHECK_FIRE('2',  0,  1)
    CHECK_FIRE('3',  1,  1)
    
    // Synonym keys (for keyboards without numpads)
    CHECK_FIRE('u', -1, -1)  // maps to 7
    CHECK_FIRE('i',  0, -1)  // maps to 8
    CHECK_FIRE('o',  1, -1)  // maps to 9
    CHECK_FIRE('j', -1,  0)  // maps to 4
    CHECK_FIRE('l',  1,  0)  // maps to 6
    CHECK_FIRE('m', -1,  1)  // maps to 1
    CHECK_FIRE(',',  0,  1)  // maps to 2
    CHECK_FIRE('.',  1,  1)  // maps to 3
    
    // --- Keychording: diagonal from two rapid same-pad cardinal keypresses ---
    {
        int chord_hit = 0;
        for (int c = 0; c < NUM_CHORDS && !chord_hit; c++) {
            int k1 = g_chord_table[c].k1, k2 = g_chord_table[c].k2;
            if ((g_keys[k1] && !g_keys_prev[k1] && g_chord_prev_key == k2) ||
                (g_keys[k2] && !g_keys_prev[k2] && g_chord_prev_key == k1)) {
                if ((g_frame_count - g_chord_prev_frame) <= CHORDING_WINDOW_FRAMES) {
                    fire_vx = g_chord_table[c].vx;
                    fire_vy = g_chord_table[c].vy;
                    fire_pressed = true;
                    chord_hit = 1;
                }
            }
        }
        if (chord_hit) {
            g_chord_prev_key = -1;
        } else {
            for (int c = 0; c < NUM_CHORDS; c++) {
                int k1 = g_chord_table[c].k1, k2 = g_chord_table[c].k2;
                if (g_keys[k1] && !g_keys_prev[k1]) {
                    g_chord_prev_key = k1; g_chord_prev_frame = g_frame_count; break; }
                if (g_keys[k2] && !g_keys_prev[k2]) {
                    g_chord_prev_key = k2; g_chord_prev_frame = g_frame_count; break; }
            }
        }
    }
    
    // '5' or 'k' turns off autofire
    if (g_keys['5'] || g_keys['k']) {
        g_autofire_vx = 0;
        g_autofire_vy = 0;
    } else if (fire_pressed) {
        g_autofire_vx = fire_vx;
        g_autofire_vy = fire_vy;
    }

    bool any_fire = (g_autofire_vx != 0 || g_autofire_vy != 0);
    
    // 3. Continuous Firing Logic
    static int fire_cooldown = 0;
    if (fire_cooldown > 0) fire_cooldown--;
    
    if (any_fire && fire_cooldown == 0) {
        int dx = g_autofire_vx;
        int dy = g_autofire_vy;
        int16_t pvx = 0, pvy = 0;
        if (dx != 0 && dy != 0) { int16_t diag = fixed_mul(LASER_SPEED, FIXED_0_707); pvx = diag * dx; pvy = diag * dy; }
        else if (dx != 0) { pvx = LASER_SPEED * dx; }
        else if (dy != 0) { pvy = LASER_SPEED * dy; }
        
        if (pvx != 0 || pvy != 0) {
            Direction dir = get_dir_from_vel(pvx, pvy);
    spawn_laser(p->wx + SCREEN_TO_FIXED(PLAYER_SPRITE_W/2) - SCREEN_TO_FIXED(LASER_SPRITE_W/2),
                        p->wy + SCREEN_TO_FIXED(PLAYER_SPRITE_H/2) - SCREEN_TO_FIXED(LASER_SPRITE_H/2),
                        pvx, pvy, dir);
            fire_cooldown = FIRE_COOLDOWN;
        }
    }
    
    p->wx += p->vx; p->wy += p->vy;
    clamp_to_screen(&p->wx, &p->wy, p->sprite_fallback_w, p->sprite_fallback_h);
}

static void update_entities(void) {
    for (int16_t idx = g_list_head; idx != -1; ) {
        int16_t next_idx = g_next[idx]; Entity* e = &g_entities[idx];
        if (!e->active) { idx = next_idx; continue; }
        if (e->type == ENT_HULK && e->pushback_timer > 0) { e->pushback_timer--; e->wx += e->vx; e->wy += e->vy; clamp_to_screen(&e->wx, &e->wy, e->sprite_fallback_w, e->sprite_fallback_h); idx = next_idx; continue; }
        bool tick_ready = ((g_frame_count - e->tick_phase) % e->tick_period) == 0;
        if (tick_ready) {
            switch (e->type) {
                case ENT_HUMAN: ai_human(e); break; case ENT_GRUNT: ai_grunt(e); break;
                case ENT_SPHEROID: ai_spheroid(e); break; case ENT_ENFORCER: ai_enforcer(e); break;
                case ENT_HULK: ai_hulk(e); break;
                default: break;
            }
            e->wx += e->vx; e->wy += e->vy;

            /* Spheroid temporal walk-cycle advance.
             *
             * The Spheroid is the only entity whose frame selection
             * has a temporal component (see entity_select_frame()'s
             * ENT_SPHEROID case): half-row parity picks the bank
             * {0,1,2} or {3,4,5}, and anim_counter % 3 picks the
             * sub-frame within that bank.
             *
             * We advance anim_counter by 1 every SPHEROID_WALK_FRAMES_PER_ADVANCE
             * ticks.  At tick_period=2 and FRAMES_PER_ADVANCE=3, that's
             * one sub-frame advance every 3*2=6 game frames, so a full
             * 3-frame bank cycle takes 18 game frames (~0.3s at 60fps).
             *
             * We hardcode the divider here rather than reading from
             * FacingInfo.step_period because the spheroid's step_period
             * field is currently used for the (vestigial) stride-cycle
             * math; overloading it would conflate two concepts.  If we
             * ever want editor-tunable advance rate, add a new field
             * to FacingInfo (e.g. anim_advance_period) and read it here.
             *
             * NOTE: This advances on EVERY tick_ready, not every
             * game frame -- so the cycle pauses if tick_phase shifts
             * (which it doesn't, currently, but is a future-proofing
             * consideration if we ever add slow-motion or pause
             * states that suppress ticks).  The pause state
             * (SPHEROID_STATE_PAUSE) does NOT pause the walk cycle
             * intentionally -- the spheroid still "breathes" while
             * resting in a corner. */
            if (e->type == ENT_SPHEROID) {
                /* Use a local sub-counter so we only increment
                 * anim_counter (the % 3 input) on every Nth tick,
                 * not every tick.  We piggyback on tick_counter
                 * (unused by the spheroid's AI) as the sub-divider
                 * so we don't need to add a new field to Entity. */
                e->tick_counter++;
                if (e->tick_counter >= SPHEROID_WALK_FRAMES_PER_ADVANCE) {
                    e->tick_counter = 0;
                    e->anim_counter++;
                    /* Wrap on a multiple of 3 to keep the value small
                     * (defensive -- modulo in the picker would handle
                     * any value, but keeping it bounded makes the
                     * editor HUD readout cleaner). */
                    if (e->anim_counter >= 3) e->anim_counter = 0;
                }
            }
        }
        if (e->type == ENT_LASER || e->type == ENT_TERROR) {
            e->age++;
            if (e->wx < -ENTITY_W(e) || e->wx > SCREEN_TO_FIXED(SCREEN_WIDTH) || e->wy < -ENTITY_H(e) || e->wy > SCREEN_TO_FIXED(SCREEN_HEIGHT)) {
                if (e->type == ENT_LASER) g_laser_count--;
                remove_entity(idx); idx = next_idx; continue;
            }
        } else {
            clamp_to_screen(&e->wx, &e->wy, e->sprite_fallback_w, e->sprite_fallback_h);
            int new_dir = get_dir_from_vel(e->vx, e->vy);
            if (new_dir != DIR_NONE) { e->move_dir = new_dir; e->facing_dir = new_dir; }
        }
        // (No temporal animation advance -- sprite frames are chosen
        // positionally at render time.  See entity_select_frame().)
        idx = next_idx;
    }
}

static void process_player_vs_entities(void) {
    for (int p = 0; p < g_player_count; p++) {
        Player* pl = &g_players[p];
        if (!pl->active || g_game_over || pl->invulnerable_timer > 0) continue;
        int cell_x = (int)pl->wx / GRID_CELL_SIZE, cell_y = (int)pl->wy / GRID_CELL_SIZE;
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int cx = cell_x + dx, cy = cell_y + dy;
                if (cx < 0 || cx >= GRID_COLS || cy < 0 || cy >= GRID_ROWS) continue;
                int cell = cy * GRID_COLS + cx;
                for (int nidx = g_grid_heads[cell]; nidx != -1; nidx = g_grid_nodes[nidx].next) {
                    Entity* e = &g_entities[g_grid_nodes[nidx].entity_idx];
                    if (!e->active) continue;
                    if (!check_player_enemy_collision(pl, e)) continue;
                    switch (e->type) {
                        
case ENT_HUMAN: {
    g_rescue_count++;  // Increment FIRST
    int points = (g_rescue_count <= 5) ? (g_rescue_count * 1000) : 5000;
    spawn_decal(DECAL_SCORE_BONUS, e->wx, e->wy, 60, points, DECAL_LAYER_OVERLAY);
    g_remove_enemy[g_grid_nodes[nidx].entity_idx] = true; 
    g_score += points;
    if (points > 1000) scorebump = 100;  // Trigger brightness bump for big scores
} break;
                        case ENT_TERROR: case ENT_CRUISE: case ENT_GRUNT: case ENT_HULK: case ENT_SPHEROID: case ENT_ENFORCER: case ENT_BRAIN: case ENT_QUARK: case ENT_ELECTRODE:
                            g_remove_enemy[g_grid_nodes[nidx].entity_idx] = true; pl->lives--; pl->ghost_x = pl->wx; pl->ghost_y = pl->wy; pl->active = false; pl->death_timer = 30;
                            if (pl->lives <= 0) { g_game_over = true; g_show_game_over = true; } break;
                        default: break;
                    }
                }
            }
        }
    }
    for (int16_t idx = g_list_head; idx != -1; ) { int16_t next_idx = g_next[idx]; if (g_remove_enemy[idx]) remove_entity(idx); idx = next_idx; }
}

static void process_entity_vs_entity(void) {
    for (int i = 0; i < MAX_ENTITIES; i++) { 
        g_remove_laser[i] = false; 
        g_remove_enemy[i] = false; 
    }
    
    // --- PASS 1: Laser vs Enemy (existing logic) ---
    for (int16_t idx = g_list_head; idx != -1; idx = g_next[idx]) {
        Entity* a = &g_entities[idx];
        if (!a->active || a->type != ENT_LASER) continue;
        int cell_x = (int)a->wx / GRID_CELL_SIZE, cell_y = (int)a->wy / GRID_CELL_SIZE;
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int cx = cell_x + dx, cy = cell_y + dy;
                if (cx < 0 || cx >= GRID_COLS || cy < 0 || cy >= GRID_ROWS) continue;
                int cell = cy * GRID_COLS + cx;
                for (int nidx = g_grid_heads[cell]; nidx != -1; nidx = g_grid_nodes[nidx].next) {
                    Entity* b = &g_entities[g_grid_nodes[nidx].entity_idx];
                    if (!b->active || b == a) continue;
                    if (b->type == ENT_HUMAN || b->type == ENT_LASER) continue;
                    if (b->type == ENT_HULK) {
                        int16_t dist = SCREEN_TO_FIXED(HULK_PUSHBACK);
                        if (a->vx > 0) b->wx += dist; else if (a->vx < 0) b->wx -= dist;
                        if (a->vy > 0) b->wy += dist; else if (a->vy < 0) b->wy -= dist;
                        clamp_to_screen(&b->wx, &b->wy, b->sprite_fallback_w, b->sprite_fallback_h);
                        g_remove_laser[idx] = true; continue;
                    }
                    if (check_swept_collision(a, b)) {
                        g_remove_laser[idx] = true; 
                        g_remove_enemy[g_grid_nodes[nidx].entity_idx] = true;
                        switch (b->type) {
                            case ENT_GRUNT: g_score += 100; break; 
                            case ENT_BRAIN: g_score += 500; break;
                            case ENT_SPHEROID: g_score += 250; break; 
                            case ENT_ENFORCER: g_score += 200; break;
                            case ENT_ELECTRODE: g_score += 50; break; 
                            case ENT_TERROR: break;
                            case ENT_CRUISE: g_score += 300; break; 
                            default: break;
                        }
                    }
                }
            }
        }
    }
    
    // --- PASS 2: Hulk vs Human (RESTORED from btronold.c) ---
    // Hulks "run over" humans when they collide, eliminating them
    for (int16_t idx = g_list_head; idx != -1; idx = g_next[idx]) {
        Entity* a = &g_entities[idx];
        if (!a->active || a->type != ENT_HULK) continue;
        int cell_x = (int)a->wx / GRID_CELL_SIZE;
        int cell_y = (int)a->wy / GRID_CELL_SIZE;
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int cx = cell_x + dx, cy = cell_y + dy;
                if (cx < 0 || cx >= GRID_COLS || cy < 0 || cy >= GRID_ROWS) continue;
                int cell = cy * GRID_COLS + cx;
                for (int nidx = g_grid_heads[cell]; nidx != -1; nidx = g_grid_nodes[nidx].next) {
                    Entity* b = &g_entities[g_grid_nodes[nidx].entity_idx];
                    if (!b->active || b == a) continue;
                    if (b->type == ENT_HUMAN) {
                        if (check_entity_entity_collision(a, b)) {
                            // Hulk ran over the human - mark for removal
                            g_remove_enemy[g_grid_nodes[nidx].entity_idx] = true;
                        }
                    }
                }
            }
        }
    }
    
    // --- REMOVAL PASS (existing logic) ---
    for (int16_t idx = g_list_head; idx != -1; ) {
        int16_t next_idx = g_next[idx];
        if (g_remove_laser[idx] || g_remove_enemy[idx]) { 
            if (g_entities[idx].type == ENT_LASER) g_laser_count--; 
            remove_entity(idx); 
        }
        idx = next_idx;
    }
}

static void check_level_complete(void) {
    int enemy_count = 0;
    for (int16_t idx = g_list_head; idx != -1; idx = g_next[idx]) {
        Entity* e = &g_entities[idx];
        if (e->active && e->type != ENT_LASER && e->type != ENT_TERROR && e->type != ENT_HULK && e->type != ENT_HUMAN && e->type != ENT_ELECTRODE && e->type != ENT_CRUISE) enemy_count++;
    }
    // If no primary threats remain, advance to the next wave
    if (enemy_count == 0 && !g_game_over) {
        play_level_intro_text();
        reset_level();
    }
}

static void update_all(void) {
    if (g_show_game_over) return;
    if (!g_players[0].active && g_players[0].death_timer > 0) {
        g_players[0].death_timer--;
        if (g_players[0].death_timer == 0) {
            if (g_players[0].lives > 0) {
                int16_t spawn_x, spawn_y;
                if (find_safe_spawn(&spawn_x, &spawn_y)) {
                    g_players[0].active = true; g_players[0].wx = spawn_x; g_players[0].wy = spawn_y;
                    g_players[0].vx = 0; g_players[0].vy = 0; g_players[0].ghost_timer = GHOST_TIMER; g_players[0].invulnerable_timer = INVULNERABLE_FRAMES;
                } else { g_players[0].lives = 0; g_game_over = true; g_show_game_over = true; }
            }
        }
    }
    if (g_players[0].invulnerable_timer > 0) g_players[0].invulnerable_timer--;
    if (g_players[0].ghost_timer > 0) g_players[0].ghost_timer--;
    if (!g_game_over) { 
        update_player(&g_players[0]); 
        rebuild_grid(); 
        update_entities(); 
        update_decals(); // <-- Added
        process_entity_vs_entity(); 
        process_player_vs_entities(); 
        check_level_complete(); 
    }
    g_frame_count++;
}


static void restart_game(int start_level) {
    init_lists(); 
    init_decals(); // <-- Added
    g_laser_count = 0; g_player_count = 0;
    g_level = start_level - 1; g_game_over = false; g_show_game_over = false;
    g_frame_count = 0; g_score = 0; g_rescue_count = 0;
    spawn_player(SCREEN_TO_FIXED(SCREEN_WIDTH / 2), SCREEN_TO_FIXED(SCREEN_HEIGHT / 2));
    reset_level();
    init_terminal_size(); init_text_buffer();
}



// ============================================================
//  TEXT-ONLY DRAWING (with half-step quantization)
// ============================================================
static void draw_player_text(Player* p) {
    float raw_tx = ((float)p->wx / COORD_SCALE) * g_term_cols / SCREEN_WIDTH;
    float raw_ty = ((float)p->wy / COORD_SCALE) * g_term_rows / SCREEN_HEIGHT;
    float tx = roundf(raw_tx * 2.0f) / 2.0f;
    float ty = roundf(raw_ty * 2.0f) / 2.0f;
    float tw = (float)p->sprite_fallback_w * g_term_cols / SCREEN_WIDTH;
    float th = (float)p->sprite_fallback_h * g_term_rows / SCREEN_HEIGHT;
    uint8_t cr = 255, cg = 255, cb = 255;
    if (p->invulnerable_timer > 0 && (g_frame_count/6)%2==0) { cr=0; cg=255; cb=255; }
    if (p->death_timer > 0) { cr = 255; cg = 0; cb = 0; }
    draw_text_rect(tx, ty, tw, th, cr, cg, cb);
}

static void draw_entity_text(Entity* e) {
    float raw_tx = ((float)e->wx / COORD_SCALE) * g_term_cols / SCREEN_WIDTH;
    float raw_ty = ((float)e->wy / COORD_SCALE) * g_term_rows / SCREEN_HEIGHT;
    float tx = roundf(raw_tx * 2.0f) / 2.0f;
    float ty = roundf(raw_ty * 2.0f) / 2.0f;
    float tw = (float)e->sprite_fallback_w * g_term_cols / SCREEN_WIDTH;
    float th = (float)e->sprite_fallback_h * g_term_rows / SCREEN_HEIGHT;
    uint8_t cr = 0xff, cg = 0xff, cb = 0xff;
    if (e->type == ENT_HUMAN) {
        static const uint8_t hc[3][3] = {{255,100,200},{100,200,255},{255,255,100}};
        int ht = e->human_type; if (ht<0||ht>2) ht=0;
        cr=hc[ht][0]; cg=hc[ht][1]; cb=hc[ht][2];
    }
    draw_text_rect(tx, ty, tw, th, cr, cg, cb);
}

// Add this function before render_all()
// 3x3 box-drawing characters for digits 0-9 (Pure text buffer, no sprite_bridge)
// 3x3 box-drawing characters for digits 0-9 (Pure text buffer, no sprite_bridge)
// Add this static buffer right above draw_score_text (near your digit_sprites definition)
static void draw_score_text(void) {
    if (!text_buffer) return;

    // Decay the bump (~0.42 seconds at 60fps)
    if (scorebump > 0) {
        scorebump -= 4;
        if (scorebump < 0) scorebump = 0;
    }

    // Calculate color: green baseline, boosted toward white when bump > 0
    unsigned char r = (unsigned char)(scorebump * 2);  // 0-200
    unsigned char g = 255;
    unsigned char b = (unsigned char)(scorebump * 2);  // 0-200
    if (r > 255) r = 255;
    if (b > 255) b = 255;

    char score_str[8];
    snprintf(score_str, sizeof(score_str), "%07d", g_score);

    for (int i = 0; i < 7; i++) {
        int digit = score_str[i] - '0';
        int col_offset = i * 3;

        for (int row = 0; row < 3; row++) {
            for (int col = 0; col < 3; col++) {
                int buf_idx = row * g_term_cols + (col_offset + col);
                if (buf_idx >= 0 && buf_idx < g_term_rows * g_term_cols) {
                    snprintf(text_buffer[buf_idx].glyph, 4, "%s", 
                             digit_sprites[digit][row * 3 + col]);
                    text_buffer[buf_idx].r = r;
                    text_buffer[buf_idx].g = g;
                    text_buffer[buf_idx].b = b;
                }
            }
        }
    }
}


//
// Per-entity positional frame selection.
//
// There is NO temporal animation for sprites.  The displayed frame is
// computed at render time from the entity's world coordinates (wx, wy)
// and its current facing_dir.  Each entity type picks its own mapping
// from (wx, wy, facing_dir) to an index in [0, count) into the facing's
// frame_indices[] table.
//
//   ENT_GRUNT  -- binary sub-row pick:
//       count=2, frame 0 = row-aligned, frame 1 = half-row-down.
//       Driven by wy alone; ignores spatial_stride.
//
//   ENT_HULK   -- stride cycle:
//       Advance one frame per `spatial_stride` terminal cells traversed
//       along the axis of motion (X for E/W, Y for N/S).  Wraps via
//       % count.  Reversing direction picks the opposite facing's
//       frame_indices[] table; the cycle then continues forward within
//       that table (it does NOT walk backward).
//
//   default    -- single-frame stubs or unmapped entities:
//       return 0 (correct for stubs which have count=1).
//
// `spatial_stride` is read from the facing's repurposed step_period
// field (see FacingInfo).  0 means "unused" -- the entity uses a
// non-stride selector like the Grunt's binary pick.

// Stride-based positional cycle: advance one frame per `stride` cells
// traversed along the given axis (0 = X, 1 = Y).  Wraps via % count.
static int stride_cycle(int16_t pos, int stride, int scale, int count, int axis) {
    if (count <= 0) return 0;
    if (stride <= 0) return 0;
    int32_t cells;
    if (axis == 0) {
        cells = ((int32_t)pos * g_term_cols) / ((int32_t)COORD_SCALE * SCREEN_WIDTH);
    } else {
        cells = ((int32_t)pos * g_term_rows) / ((int32_t)COORD_SCALE * SCREEN_HEIGHT);
    }
    if (cells < 0) cells = 0;
    /* INVERTED SCALE: bigger scale = FASTER animation.
     *
     * The math is `frame = ((cells * scale) / stride) % count` -- we
     * multiply by scale BEFORE dividing by stride so sub-stride
     * precision is preserved in pure integer arithmetic.  This lets
     * scale values larger than stride yield a fractional effective
     * stride (e.g. stride=4, scale=8 -> one frame advance per
     * half-cell, which is exactly what the Hulk N/S walk needs).
     *
     * Defaults of scale=1 reproduce the legacy `cells / stride`
     * behaviour bit-for-bit, so existing sprites are unchanged
     * until the user hand-tunes or `btk -e` raises scale above 1.
     */
    if (scale <= 0) scale = 1;
    return (int)(((cells * (int32_t)scale) / stride) % count);
}

static int entity_select_frame(EntityType type, int16_t wx, int16_t wy,
                                int facing_dir, int count, int spatial_stride,
                                int spatial_scale, int anim_phase)
{
    switch (type) {
        case ENT_GRUNT:
        case ENT_ENFORCER: {
            // Half-row positional pick -- the Grunt is the simple 2-frame
            // proof-of-concept for terminal double-resolution rendering.
            // The Enforcer shares the same scheme: its up/down facings
            // reuse the same two-frame canvas pair (there is no
            // directional difference between N and S, just a vertical
            // bob), so half-row parity picks the frame identically.
            //
            // We pretend the terminal is 2x taller (each row becomes two
            // half-rows), integer-divide the entity's Y position to find
            // which half-row it's on, and use the low bit to pick frame:
            //   even (low bit 0) = aligned   -> frame 0 (4x2 canvas)
            //   odd  (low bit 1) = inbetween -> frame 1 (4x3 canvas, half-row-down)
            //
            // The half-row shift itself is baked into the sprite bytes
            // via half-block glyphs; here we only decide which canvas to
            // blit.  Pure integer math -- no floats, no roundf, no floorf.
            //
            // This same scheme generalizes: bump the multiplier from 2
            // to N to get N sub-positions per row (e.g. multiplier=4 +
            // 4 frames per facing = smooth quarter-row walk cycle).
            //
            // scale_x/scale_y are unused here -- the half-row picker has
            // no configurable stride.  (spatial_stride and spatial_scale
            // are accepted for signature uniformity but ignored.)
            // anim_phase is also unused (Grunt/Enforcer have no temporal
            // walk cycle -- the half-row bit IS the frame index).
            (void)spatial_stride;
            (void)spatial_scale;
            (void)anim_phase;
            if (count < 2) return 0;
            /* Compute the half-row index directly from wy in one
             * integer-division step.  The previous two-step formula
             *   raw_pixels = wy / COORD_SCALE;
             *   half_rows  = raw_pixels * (2*term_rows) / SCREEN_HEIGHT;
             * compounded truncation errors: the first division floors
             * wy to a 16-unit bucket, so the half-row index would
             * sometimes fail to advance when wy crossed a true
             * half-row boundary mid-bucket (e.g. wy 4564..4575 all
             * collapsed to raw_pixels=285 -> half_rows 57, when the
             * true half-row at wy>=4564 is 58).  This produced
             * "stuck frame" symptoms where tapping the down key
             * advanced wy by editor_half_row_step() (=78) but the
             * displayed sprite stayed on the previous frame for one
             * extra step, then snapped back.
             *
             * Computing in one shot from wy avoids the intermediate
             * floor and matches the denominator used by
             * editor_half_row_step() exactly, so a single key press
             * advances the half-row index by exactly 1 (except at
             * sub-step rounding, which is now distributed evenly
             * instead of clustered in dead zones). */
            int32_t half_rows = (int32_t)wy * ((int32_t)g_term_rows * 2)
                              / ((int32_t)SCREEN_HEIGHT * COORD_SCALE);
            if (half_rows < 0) half_rows = 0;
            return (int)(half_rows & 1);
        }
        case ENT_HULK: {
            // Stride cycle along the axis of motion.  spatial_stride
            // (read from fi->step_period) = cells per base frame advance;
            // spatial_scale (from fi->scale_x/y) multiplies the advance
            // rate -- bigger scale = faster animation.  See stride_cycle()
            // for the inverted-math rationale.
            //
            // The Hulk is purely positional: no temporal counter, no
            // bank selection -- the stride IS the frame index.
            (void)anim_phase;
            int axis = (facing_dir == DIR_UP || facing_dir == DIR_DOWN) ? 1 : 0;
            int16_t pos = axis ? wy : wx;
            return stride_cycle(pos, spatial_stride, spatial_scale, count, axis);
        }
        case ENT_SPHEROID: {
            // HYBRID picker -- half-row parity picks the bank, the
            // temporal anim_phase counter picks the sub-frame within
            // the bank.  This is the only entity that combines both
            // positional and temporal selection.
            //
            // Bank layout (matches the .ans authoring convention and
            // the flat walk-table emitted by build_sprites7.c):
            //   frames 0,1,2 = aligned bank    (half-row parity = 0)
            //   frames 3,4,5 = in-between bank (half-row parity = 1)
            //
            // MATH TRICK: instead of maintaining a nested walk table
            // like [(0,1,2),(3,4,5)] and indexing it as
            //   walk_table[bank][sub]
            // we keep the walk table FLAT as [0,1,2,3,4,5] and compute
            // the walk-table index directly:
            //
            //   idx_in_walk_table = bank_base + sub
            //                     = (half_row_parity * 3) + (anim_phase % 3)
            //
            // Why this is equivalent and preferable:
            //   - The walk table is already a flat C array of frame
            //     indices; a nested [(0,1,2),(3,4,5)] would require
            //     either a 2D array (changes the SpriteSet data
            //     layout, breaks the editor's flat-array editor) or
            //     arithmetic to find the row offset (which is what
            //     we're doing here anyway, just at lookup time).
            //   - The flat layout means the editor's "walk_n: [0,1,2,3,4,5]"
            //     readout stays readable and the S-key save format
            //     matches what build_sprites7.c emits.
            //   - The math collapses to one add and one mod, both
            //     cheap integer ops; no extra pointer dereference.
            //
            // Authoring implication: if you want to reorder frames
            // within a bank (e.g. ping-pong: 0,2,1,3,5,4), edit the
            // flat walk table in sprites.h directly -- the picker
            // doesn't care about the bank's internal order, only that
            // the first 3 entries belong to bank 0 and the next 3 to
            // bank 1.  (If you ever want a non-3-sized bank -- e.g.
            // 2 aligned + 4 in-between -- you'd need to either change
            // this constant or move to a real nested layout.)
            (void)spatial_stride;
            (void)spatial_scale;
            (void)facing_dir;     /* spheroid is omnidirectional */

            /* Half-row parity selects the bank.  Mirrors the Grunt's
             * one-shot formula -- see the long comment in the
             * ENT_GRUNT case for the truncation-dead-zone rationale. */
            int32_t half_rows = (int32_t)wy * ((int32_t)g_term_rows * 2)
                              / ((int32_t)SCREEN_HEIGHT * COORD_SCALE);
            if (half_rows < 0) half_rows = 0;
            int bank = (int)(half_rows & 1);    /* 0 = aligned, 1 = in-between */

            /* Temporal sub-frame within the bank.  anim_phase is
             * Entity.anim_counter (or a g_frame_count-derived
             * substitute in the editor preview).  The caller is
             * responsible for advancing anim_counter at the right
             * cadence (see SPHEROID_WALK_FRAMES_PER_ADVANCE and the
             * tick path in update_entities()). */
            int sub = anim_phase % 3;
            if (sub < 0) sub += 3;     /* defensive: C's % can be negative */

            int idx = bank * 3 + sub;
            if (idx < 0 || idx >= count) idx = 0;
            return idx;
        }
        default:
            // Stubs and unmapped entities -- always frame 0.
            return 0;
    }
}

//
// Lookup the current frame for a sprite set + facing + world position.
// Uses the per-facing frame_indices[] table for indirection -- this
// allows non-contiguous frame reuse, ping-pong cycles, and different
// frame counts per facing (e.g. Walk N=3 frames, Walk E=8 frames).
//
// The index into frame_indices[] is chosen by entity_select_frame()
// and is purely positional for most entities (Grunt, Enforcer, Hulk).
// The Spheroid is the exception: it passes anim_phase through to
// entity_select_frame() so the temporal sub-frame within the
// half-row-selected bank can advance independently of position.
// Callers that don't care about temporal animation can pass 0.
static const SpriteFrame* get_current_frame(const SpriteSet* ss, int facing_dir,
                                             int16_t wx, int16_t wy,
                                             EntityType type, int anim_phase) {
    if (!ss || !ss->frames) return NULL;
    if (facing_dir < 0 || facing_dir > 3) facing_dir = 1;  /* default to S */
    const FacingInfo* fi = &ss->facing[facing_dir];
    if (!fi->frame_indices || fi->count <= 0) return NULL;

    /* Editor shadow: when the walk-table editor is active for this
     * entity, consult the mutable shadow instead of the const table.
     * This lets the user see their edits take effect immediately in
     * the live sprite preview.  The const table is still used for
     * all other entities and during normal gameplay. */
    const int* indices = fi->frame_indices;
    int count = fi->count;
    int scale_x = fi->scale_x > 0 ? fi->scale_x : 1;
    int scale_y = fi->scale_y > 0 ? fi->scale_y : 1;
    if (g_editor_active && type == g_editor_entity
        && type >= 0 && type < NUM_ENTITY_TYPES) {
        EditorWalk* ew = &g_editor_walks[type][facing_dir];
        if (ew->count > 0) {
            indices = ew->indices;
            count = ew->count;
        }
        scale_x = ew->scale_x > 0 ? ew->scale_x : 1;
        scale_y = ew->scale_y > 0 ? ew->scale_y : 1;
    }
    if (count <= 0) return NULL;

    /* Apply per-axis stride multiplier (always on, even in gameplay,
     * so the user's `btk -e` tuning takes effect in the game too).
     * scale_x applies to E/W facings, scale_y to N/S.
     *
     * INVERTED MATH (see stride_cycle): bigger scale = FASTER.
     * We pass base_stride (= step_period) and scale separately --
     * stride_cycle multiplies cells by scale BEFORE dividing by
     * stride, preserving sub-stride precision so scale values
     * larger than step_period yield fractional effective strides
     * (e.g. step_period=4, scale=8 -> one frame per half-cell).
     *
     * For the Spheroid, spatial_stride and spatial_scale are unused
     * (the picker ignores them) -- but we still pass them through
     * for signature uniformity.  The anim_phase is what the Spheroid
     * actually consumes. */
    int base_stride = fi->step_period;
    int scale = (facing_dir == DIR_UP || facing_dir == DIR_DOWN)
              ? scale_y : scale_x;
    if (base_stride <= 0) base_stride = 1;  /* defensive; Grunt/Spheroid bypass anyway */

    int idx_in_facing = entity_select_frame(type, wx, wy, facing_dir,
                                             count, base_stride, scale,
                                             anim_phase);
    if (idx_in_facing < 0 || idx_in_facing >= count) idx_in_facing = 0;

    int idx = indices[idx_in_facing];
    if (idx < 0 || idx >= ss->total_frames) return NULL;
    return &ss->frames[idx];
}

// Helper: get the FacingInfo for a sprite set + facing, with safe fallback.
static const FacingInfo* get_facing_info(const SpriteSet* ss, int facing_dir) {
    if (!ss) return NULL;
    if (facing_dir < 0 || facing_dir > 3) facing_dir = 1;  /* default to S */
    return &ss->facing[facing_dir];
}

// Render a sprite frame's cell data into the text buffer.
// sf: the frame to render. screen_x/y: top-left position in text cells.
static void render_sprite_frame(const SpriteFrame* sf, int screen_x, int screen_y) {
    if (!sf || !sf->rows) return;
    for (int row = 0; row < sf->h; row++) {
        const uint8_t* r = sf->rows[row];
        if (!r) continue;
        for (int col = 0; col < sf->w; col++) {
            int sx = screen_x + col;
            int sy = screen_y + row;
            if (sx < 0 || sx >= g_term_cols || sy < 0 || sy >= g_term_rows) continue;
            TextCell* cell = &text_buffer[sy * g_term_cols + sx];
            memcpy(cell->glyph, r + col * 10, 4);
            cell->r  = r[col * 10 + 4];
            cell->g  = r[col * 10 + 5];
            cell->b  = r[col * 10 + 6];
            cell->br = r[col * 10 + 7];
            cell->bg = r[col * 10 + 8];
            cell->bb = r[col * 10 + 9];
        }
    }
}

//------------PER-ENTITY RENDER HOOKS------------------------------
//
// btk introduces a per-entity render hook so entities that don't fit
// the table-driven SpriteSet/FacingInfo model (Cruise missile with
// trailing body, Prog with morphing shapes, etc.) can plug in custom
// renderers without disturbing the universal path.
//
// Architecture:
//   - render_entity_custom(e) returns true if it handled the entity.
//   - render_player_custom(p) returns true if it handled the player.
//   - In render_all(), each of these is consulted FIRST.  If they
//     return false, the universal table-driven path runs as before.
//
// To add a custom renderer for, say, Cruise:
//   1. Implement  static void render_cruise(Entity* e)  somewhere
//      above render_all().
//   2. Add a case to render_entity_custom():
//         case ENT_CRUISE: render_cruise(e); return true;
//   3. Done.  No changes to the table-driven path or to other entity
//      types.
//
// The default behaviour (no custom cases) is identical to bti: every
// entity with real sprite data goes through get_current_frame() +
// render_sprite_frame().  This keeps Grunt, Hulk, Brain, Spheroid,
// etc. on the well-tested table-driven path.

static bool render_entity_custom(Entity* e) {
    // Per-entity-type custom renderers go here.  Each case MUST call
    // its renderer and return true.  Returning false falls through to
    // the universal table-driven path.
    //
    // Example (not yet implemented):
    //   case ENT_CRUISE: render_cruise(e); return true;
    //   case ENT_QUARK:  render_prog(e);   return true;
    switch (e->type) {
        default:
            return false;
    }
}

static bool render_player_custom(Player* p) {
    // Hook for player-specific rendering (firing-direction overlay,
    // invuln flash, death animation, etc.).  Returning false falls
    // through to the universal table-driven path.
    (void)p;  // unused for now
    return false;
}

//------------RENDER ALL------------------------------
//
// Update render_all() to call draw_score_text()
static void render_all(void) {
    // 1. Clear the offscreen text buffer
    clear_text_buffer();
    
    // 2. Draw FLOOR layer decals (under entities, e.g., squished human 'X's)
    draw_floor_decals();
    
    // 3. Draw Player (sprite or colored-rect fallback)
    if (g_players[0].active || g_players[0].death_timer > 0) {
        Player* p = &g_players[0];
        /* Invulnerability flash: skip rendering on "off" frames */
        if (p->invulnerable_timer > 0 && (g_frame_count/6)%2==0) {
            /* off frame — leave blank for flash */
        } else if (render_player_custom(p)) {
            /* Custom per-player renderer handled it.  No further work. */
        } else if (p->death_timer > 0) {
            /* death — red rect fallback */
            float raw_tx = ((float)p->wx / COORD_SCALE) * g_term_cols / SCREEN_WIDTH;
            float raw_ty = ((float)p->wy / COORD_SCALE) * g_term_rows / SCREEN_HEIGHT;
            float tx = roundf(raw_tx * 2.0f) / 2.0f;
            float ty = roundf(raw_ty * 2.0f) / 2.0f;
            float tw = (float)p->sprite_fallback_w * g_term_cols / SCREEN_WIDTH;
            float th = (float)p->sprite_fallback_h * g_term_rows / SCREEN_HEIGHT;
            draw_text_rect(tx, ty, tw, th, 255, 0, 0);
        } else if (p->anim && p->anim->frames[0].w > 1) {
            /* Real sprite data loaded — render per-cell art.
             * Apply per-cycle offset from the active FacingInfo.
             * tx/ty are computed by integer truncation of the world
             * position to terminal-cell coordinates, matching the
             * half-row bucketing used by entity_select_frame() so
             * the chosen sprite frame lines up with its true row. */
            int tx = (int)((int32_t)p->wx * g_term_cols / ((int32_t)COORD_SCALE * SCREEN_WIDTH));
            int ty = (int)((int32_t)p->wy * g_term_rows / ((int32_t)COORD_SCALE * SCREEN_HEIGHT));
            const SpriteFrame* sf = get_current_frame(p->anim, p->facing_dir, p->wx, p->wy, ENT_PLAYER, 0);
            const FacingInfo* fi = get_facing_info(p->anim, p->facing_dir);
            if (fi) { tx += fi->offset_x; ty += fi->offset_y; }
            /* Per-frame placement offset (baked from .ans OffsetX+HotspotX,
             * OffsetY+HotspotY by build_sprites7.c).  Applied at render
             * time per the FacingInfo doc comment.  This is what lets a
             * small frame (e.g. Spheroid f0 = 2x1) align visually with
             * larger frames in the same cycle. */
            if (sf) { tx += sf->ox; ty += sf->oy; }
            render_sprite_frame(sf, tx, ty);
        } else {
            /* Stub sprites — colored rect fallback */
            float raw_tx = ((float)p->wx / COORD_SCALE) * g_term_cols / SCREEN_WIDTH;
            float raw_ty = ((float)p->wy / COORD_SCALE) * g_term_rows / SCREEN_HEIGHT;
            float tx = roundf(raw_tx * 2.0f) / 2.0f;
            float ty = roundf(raw_ty * 2.0f) / 2.0f;
            float tw = (float)p->sprite_fallback_w * g_term_cols / SCREEN_WIDTH;
            float th = (float)p->sprite_fallback_h * g_term_rows / SCREEN_HEIGHT;
            draw_text_rect(tx, ty, tw, th, 255, 255, 255);
        }
    }
    
    // 4. Draw Entities (sprite or colored-rect fallback)
    for (int16_t idx = g_list_head; idx != -1; idx = g_next[idx]) {
        Entity* e = &g_entities[idx];
        if (!e->active) continue;
        
        if (render_entity_custom(e)) {
            /* Custom per-entity renderer handled it.  No further work. */
            continue;
        }
        if (e->anim && e->anim->frames[0].w > 1) {
            /* Real sprite data loaded — render per-cell art.
             * Apply per-cycle offset from the active FacingInfo.
             * tx/ty are computed by integer truncation of the world
             * position to terminal-cell coordinates, matching the
             * half-row bucketing used by entity_select_frame() so
             * the chosen sprite frame lines up with its true row. */
            int tx = (int)((int32_t)e->wx * g_term_cols / ((int32_t)COORD_SCALE * SCREEN_WIDTH));
            int ty = (int)((int32_t)e->wy * g_term_rows / ((int32_t)COORD_SCALE * SCREEN_HEIGHT));
            /* Pass e->anim_counter as the temporal phase so the Spheroid
             * picker can advance its within-bank sub-frame.  Other entity
             * types ignore the value (see entity_select_frame() -- their
             * pickers don't read anim_phase). */
            const SpriteFrame* sf = get_current_frame(e->anim, e->facing_dir, e->wx, e->wy, e->type, e->anim_counter);
            const FacingInfo* fi = get_facing_info(e->anim, e->facing_dir);
            if (fi) { tx += fi->offset_x; ty += fi->offset_y; }
            /* Per-frame placement offset (baked from .ans OffsetX+HotspotX,
             * OffsetY+HotspotY by build_sprites7.c).  Applied at render
             * time per the FacingInfo doc comment.  This is what lets a
             * small frame (e.g. Spheroid f0 = 2x1) align visually with
             * larger frames in the same cycle. */
            if (sf) { tx += sf->ox; ty += sf->oy; }
            render_sprite_frame(sf, tx, ty);
        } else {
            /* Stub sprites — colored rect fallback */
            float raw_tx = ((float)e->wx / COORD_SCALE) * g_term_cols / SCREEN_WIDTH;
            float raw_ty = ((float)e->wy / COORD_SCALE) * g_term_rows / SCREEN_HEIGHT;
            float tx = roundf(raw_tx * 2.0f) / 2.0f;
            float ty = roundf(raw_ty * 2.0f) / 2.0f;
            float tw = (float)e->sprite_fallback_w * g_term_cols / SCREEN_WIDTH;
            float th = (float)e->sprite_fallback_h * g_term_rows / SCREEN_HEIGHT;

            /* Per-entity-type color table */
            uint8_t cr = 255, cg = 255, cb = 255;
            switch (e->type) {
                case ENT_GRUNT:     cr=178; cg= 34; cb= 34; break; /* brick-red */
                case ENT_HULK:      cr=  0; cg=200; cb=  0; break; /* green */
                case ENT_SPHEROID:  cr=255; cg=165; cb=  0; break; /* orange */
                case ENT_ENFORCER:  cr=200; cg= 50; cb=150; break; /* reddish purple */
                case ENT_LASER:     cr=255; cg=255; cb=  0; break; /* yellow */
                case ENT_TERROR:    cr=180; cg=  0; cb=255; break; /* violet/purple */
                case ENT_QUARK:     cr=255; cg= 60; cb= 60; break; /* red */
                case ENT_BRAIN:     cr=180; cg=  0; cb=180; break; /* magenta */
                case ENT_ELECTRODE: cr=255; cg=165; cb=  0; break; /* yellow-orange */
                case ENT_CRUISE:    cr=  0; cg=200; cb=200; break; /* cyan */
                case ENT_HUMAN: {
                    static const uint8_t hc[3][3] = {
                        {255,182,193}, /* mommy: pink */
                        {173,216,230}, /* daddy: light blue */
                        {255,218,185}  /* mikey: peach */
                    };
                    int ht = e->human_type;
                    if (ht<0||ht>2) ht=0;
                    cr=hc[ht][0]; cg=hc[ht][1]; cb=hc[ht][2];
                    break;
                }
                default: break;
            }
            draw_text_rect(tx, ty, tw, th, cr, cg, cb);
        }
    }
    
    // 5. Draw OVERLAY layer decals (above entities, e.g., rainbow score bonuses)
    draw_overlay_decals();
    
    // 6. Draw HUD (Score top-left, Lives top-right)
    draw_score_text();
    draw_lives_text(g_players[0].lives);
    
    // 7. Commit the frame to the terminal
    flush_text_buffer();
}


// ============================================================
//  INTRO ANIMATION
// ============================================================
static void play_level_intro_text(void) {
    const char *CURSOR_HOME = "\x1b[H";
    const char *CLEAR_SCREEN = "\x1b[2J";
    const char *RESET_STR = "\x1b[0m";
    const char *HIDE_CURSOR = "\x1b[?25l";
    const char *SHOW_CURSOR = "\x1b[?25h";
    const char *BLACK = "\x1b[30m";
    static int num_colors = 10;
    static const char* colors[10] = {
        "\x1b[95m", "\x1b[91m", "\x1b[38;5;208m", "\x1b[93m", "\x1b[92m",
        "\x1b[96m", "\x1b[94m", "\x1b[34m", "\x1b[38;5;129m", "\x1b[95m"
    };
    int blag = 22;
    int center_x = g_term_cols / 2;
    int center_y = g_term_rows / 2;
    int max_w = g_term_cols;
    int max_h = g_term_rows;
    int numframes = (g_term_rows / 2) < (g_term_cols / 4) ? (g_term_rows / 2) : (g_term_cols / 4);
    typedef struct { int bx1, by1, bx2, by2; const char* color; } TBox;
    TBox myboxes[128];
    int bwidth = max_w - 2;
    int bheight = max_h - 2;
    for (int i = numframes - 1; i >= 0; i--) {
        myboxes[i].bx1 = center_x - (bwidth / 2);
        myboxes[i].by1 = center_y - (bheight / 2);
        myboxes[i].bx2 = center_x + (bwidth / 2);
        myboxes[i].by2 = center_y + (bheight / 2);
        myboxes[i].color = colors[i % num_colors];
        bwidth -= 4; bheight -= 2;
    }
    int x, y;
    printf("%s%s%s", CLEAR_SCREEN, CURSOR_HOME, HIDE_CURSOR);
    fflush(stdout);
    for (int frame = 1; frame < (int)(1.89*numframes); frame++) {
        int fr = frame; if (fr > numframes -1) fr = numframes -1;
        int er = (frame - blag); if (er < 0) er = 0; if (er > numframes -1) er = numframes -1;
        TBox *box = &myboxes[fr];
        printf("%s", colors[fr % num_colors]);
        printf("\x1b[%d;%dH", box->by1 + 1, box->bx1 + 1);
        for (x = box->bx1; x <= box->bx2; x++) printf("─");
        printf("\x1b[%d;%dH", box->by2 + 1, box->bx1 + 1);
        for (x = box->bx1; x <= box->bx2; x++) printf("─");
        y=box->by1; printf("\x1b[%d;%dH%s", y + 1, box->bx1 + 1, "┌");
        for (y = box->by1+1; y <= box->by2-1; y++) printf("\x1b[%d;%dH%s", y + 1, box->bx1 + 1, "│");
        printf("\x1b[%d;%dH%s", y + 1, box->bx1 + 1, "└");
        y=box->by1; printf("\x1b[%d;%dH%s", y + 1, box->bx2 + 1, "┐");
        for (y = box->by1+1; y <= box->by2-1; y++) printf("\x1b[%d;%dH%s", y + 1, box->bx2 + 1, "│");
        printf("\x1b[%d;%dH%s", y + 1, box->bx2 + 1, "┘");
        box = &myboxes[er];
        printf("%s", BLACK);
        printf("\x1b[%d;%dH", box->by1 + 1, box->bx1 + 1);
        for (x = box->bx1; x <= box->bx2; x++) printf(" ");
        printf("\x1b[%d;%dH", box->by2 + 1, box->bx1 + 1);
        for (x = box->bx1; x <= box->bx2; x++) printf(" ");
        y=box->by1; printf("\x1b[%d;%dH%s", y + 1, box->bx1 + 1, " ");
        for (y = box->by1+1; y <= box->by2-1; y++) printf("\x1b[%d;%dH%s", y + 1, box->bx1 + 1, " ");
        printf("\x1b[%d;%dH%s", y + 1, box->bx1 + 1, " ");
        y=box->by1; printf("\x1b[%d;%dH%s", y + 1, box->bx2 + 1, " ");
        for (y = box->by1+1; y <= box->by2-1; y++) printf("\x1b[%d;%dH%s", y + 1, box->bx2 + 1, " ");
        printf("\x1b[%d;%dH%s", y + 1, box->bx2 + 1, " ");
        printf("%s", RESET_STR);
        fflush(stdout);
        usleep(33333);
    }
    /* Reset colors only — do NOT show cursor here.
     * The cursor was hidden by init_text_mode() at game start and
     * is shown by fini_text_mode() on cleanup.  Re-showing it here
     * leaves it visible during the gameplay loop, causing a flicker
     * at the bottom-right of the screen every frame.
     * (DEC private mode: \x1b[?25l = hide, \x1b[?25h = show.) */
    printf("%s", RESET_STR);
    fflush(stdout);
}




























// ============================================================
//  INTRO SCREEN - VT100 Double-Size Wargames Style
// ============================================================

      
static void load_and_draw_ansi_logo(const char* filename) {
    const int logo_width = 86;  // Fixed width of the logo
    
    // Calculate centered starting column
    int start_col = (g_term_cols - logo_width) / 2 + 2;
    if (start_col < 1) start_col = 1;

    FILE* f = fopen(filename, "r");
    if (!f) {
        // Fallback: draw text logo if file not found
        const char* fallback_logo[] = {
            "d8888b. db      .d88b.  d8888b.  .d88b.  d888888b d8888b.  .d88b.  d8b   db ",
            "88  `8D 88     .8P  Y8. 88  `8D .8P  Y8. `~~88~~' 88  `8D .8P  Y8. 888o  88 ",
            "88oooY' 88     88    88 88oodD' 88    88    88    88oobY' 88    88 88V8o 88 ",
            "88~~~b. 88     88    88 88~~~   88    88    88    88`8b   88    88 88 V8o88 ",
            "88   8D 88booo.`8b  d8' 88      `8b  d8'    88    88 `88. `8b  d8' 88  V888 ",
            "Y8888P' Y88888P `Y88P'  88       `Y88P'     YP    88   YD  `Y88P'  VP   V8P "
        };
        int num_lines = sizeof(fallback_logo) / sizeof(fallback_logo[0]);
        
        printf("\x1b[38;2;255;100;0m"); // Orange color
        for (int i = 0; i < num_lines; i++) {
            printf("\x1b[%d;%dH%s", i + 3, start_col, fallback_logo[i]);
        }
        printf("\x1b[0m");
        return;
    }

    // File exists: draw from file
    char line[256];
    int row = 1;

    while (fgets(line, sizeof(line), f)) {
        int len = strlen(line);
        
        // Strip newline and carriage return
        if (len > 0 && line[len-1] == '\n') { line[--len] = '\0'; }
        if (len > 0 && line[len-1] == '\r') { line[--len] = '\0'; }
        
        // Crop if too wide
        if (len > logo_width) {
            line[logo_width] = '\0';
        }

        printf("\x1b[%d;%dH%s", row, start_col, line);
        row++;
    }
    
    fclose(f);
    printf("\x1b[0m");
}


static void draw_double_size_text(int start_row, int start_col, const char* text, 
                                   unsigned char r, unsigned char g, unsigned char b) {
    if (!text || start_row < 1 || start_row + 1 > g_term_rows) return;
    if (start_col < 1) start_col = 1;
    
    // Set the color
    printf("\x1b[38;2;%d;%d;%dm", r, g, b);
    
    // Top half: cursor to column 1, emit \e#3, pad to start_col, print text
    printf("\x1b[%d;1H\x1b#3", start_row);
    for (int i = 1; i < start_col; i++) {
        putchar(' ');
    }
    printf("%s", text);
    
    // Bottom half: cursor to column 1 of next row, emit \e#4, pad, print text
    printf("\x1b[%d;1H\x1b#4", start_row + 1);
    for (int i = 1; i < start_col; i++) {
        putchar(' ');
    }
    printf("%s", text);
    
    // Reset attributes
    printf("\x1b[0m");
    fflush(stdout);
}

static void draw_marquee_text(void) {
    const char* marquee_lines[] = {
        "      IT IS THE YEAR 2024       ",
        "     THE BLOPS HAVE INVADED     ",
        "THEY HUNT THE LAST HUMAN FAMILES",
        " SAVE THEM FROM THE BLOP HORDE  ",
        "    YOU ARE THEIR ONLY HOPE     "
    };

    int num_lines = sizeof(marquee_lines) / sizeof(marquee_lines[0]);
    int start_row = 12;
    int textwidth = 33;
    
    int start_col = (g_term_cols / 4) - (textwidth / 2) + 0;
    if (start_col < 1) start_col = 1;

    unsigned char r = 255, g = 175, b = 0;
    const char* cursor = "\xe2\x96\x92";

    bool skip = false;
    
    for (int i = 0; i < num_lines; i++) {
        const char* line = marquee_lines[i];
        int len = strlen(line);
        char current_text[256];
        int j;
        
        for (j = 0; j < len; j++) {
            // Non-blocking keyboard check
            fd_set set;
            struct timeval timeout = {0, 0};
            FD_ZERO(&set);
            FD_SET(STDIN_FILENO, &set);
            
            if (select(STDIN_FILENO + 1, &set, NULL, NULL, &timeout) > 0) {
                char ch;
                fd_set drain_set;
                struct timeval drain_timeout = {0, 0};
                
                // SAFE DRAIN: Use select before every read to prevent blocking
                for (;;) {
                    FD_ZERO(&drain_set);
                    FD_SET(STDIN_FILENO, &drain_set);
                    if (select(STDIN_FILENO + 1, &drain_set, NULL, NULL, &drain_timeout) <= 0)
                        break;
                    if (read(STDIN_FILENO, &ch, 1) <= 0)
                        break;
                }
                skip = true;
                break;
            }
            
            current_text[j] = line[j];
            current_text[j + 1] = '\0';

            if (j < len - 1) {
                char draw_text[256];
                snprintf(draw_text, sizeof(draw_text), "%s%s", current_text, cursor);
                draw_double_size_text(start_row + (i * 2), start_col, draw_text, r, g, b);
            } else {
                draw_double_size_text(start_row + (i * 2), start_col, current_text, r, g, b);
            }

            usleep(90000);
        }

        if (skip) {
            for (int k = i; k < num_lines; k++) {
                draw_double_size_text(start_row + (k * 2), start_col, marquee_lines[k], r, g, b);
            }
            break;
        }

        draw_double_size_text(start_row + (i * 2), start_col, current_text, r, g, b);
        
        int cursor_col = start_col + len;
        printf("\x1b[%d;%dH \x1b[%d;%dH ", 
               start_row + (i * 2), cursor_col,
               start_row + (i * 2) + 1, cursor_col);
        fflush(stdout);

        usleep(150000);
    }
}


static void draw_keyboard_schematic(void) {
    int start_row = g_term_rows - 22;  
    int schematic_width = 46;          // Width of the longest line in the schematic
    
    // YOUR EXACT LOGIC:
    int start_col = (g_term_cols / 4) - (schematic_width / 2) + 1;
    if (start_col < 1) start_col = 1;

    draw_double_size_text(start_row +  0, start_col, "............................................", 210, 100, 0);
    draw_double_size_text(start_row +  2, start_col, ".... W.A.L.K ................. F.I.R.E .....", 219, 100, 0);
    draw_double_size_text(start_row +  4, start_col, "..┌───┬───┬───┐ ........... ┌───┬───┬───┐...", 240, 110, 10);
    draw_double_size_text(start_row +  6, start_col, "..│ Q │ W │ E │ ........... │ 7 │ 8 │ 9 │...", 250, 115, 10);
    draw_double_size_text(start_row +  8, start_col, "..├───┼───┼───┤ ........... ├───┼───┼───┤...", 250, 115, 10);
    draw_double_size_text(start_row + 10, start_col, "..│ A │ S │ D │ .S to Stop. │ 4 │ 5 │ 6 │...", 240, 105, 5);
    draw_double_size_text(start_row + 12, start_col, "..├───┼───┼───┤ ..walking.. ├───┼───┼───┤...", 230, 105, 5);
    draw_double_size_text(start_row + 14, start_col, "..│ Z │ X │ C │ ........... │ 1 │ 2 │ 3 │...", 220, 100, 0);
    draw_double_size_text(start_row + 16, start_col, "..└───┴───┴───┘ ........... └───┴───┴───┘...", 210, 100, 0);
    draw_double_size_text(start_row + 18, start_col, "......................... 5 to stop firing..", 200, 100, 0);
    
    printf("\x1b[0m");
}

static void play_demo_animation(void) {
    // Clear middle section for animation
    for (int row = 12; row < g_term_rows - 14; row++) {
        printf("\x1b[%d;1H\x1b[K", row);
    }
    
    int anim_row = 20;
    int frame = 0;
    int max_frames = 300;  // 5 seconds at 60fps
    
    // Entity positions (in terminal columns)
    float mommy_x = -5;
    float hulk_x = -10;
    float grunt1_x = -15;
    float grunt2_x = -20;
    float player_x = -5;
    
    // Speeds (columns per frame)
    float mommy_speed = 0.05;
    float hulk_speed = mommy_speed * 1.1;
    float grunt_speed = hulk_speed * 0.9;
    float player_speed = 0.08;
    
    int mommy_pause_start = -1;
    bool grunt1_dead = false;
    bool grunt2_dead = false;
    int player_shoot_timer = 0;
    float laser_x = -100;
    bool laser_active = false;
    
    while (frame < max_frames) {
        // Clear previous frame
        for (int row = anim_row - 1; row <= anim_row + 1; row++) {
            printf("\x1b[%d;1H\x1b[K", row);
        }
        
        // Update positions
        mommy_x += mommy_speed;
        
        // Mommy pauses after reaching center
        if (mommy_x > g_term_cols / 3 && mommy_pause_start == -1) {
            mommy_pause_start = frame;
        }
        if (mommy_pause_start != -1 && frame - mommy_pause_start < 180) {
            // Paused
        } else {
            mommy_x += mommy_speed;
        }
        
        hulk_x += hulk_speed;
        
        // Grunts move jerkily
        if (frame % 3 == 0) {
            grunt1_x += grunt_speed;
            grunt2_x += grunt_speed;
        }
        
        // Player appears later and moves faster
        if (frame > 60) {
            player_x += player_speed;
        }
        
        // Player shooting logic
        if (frame > 90 && !laser_active && player_shoot_timer == 0) {
            // Shoot at grunt1
            if (!grunt1_dead && player_x > grunt1_x - 10) {
                laser_active = true;
                laser_x = player_x;
                player_shoot_timer = 30;
            }
        }
        
        if (frame > 120 && !laser_active && player_shoot_timer == 0) {
            // Shoot at grunt2
            if (!grunt2_dead && player_x > grunt2_x - 10) {
                laser_active = true;
                laser_x = player_x;
                player_shoot_timer = 30;
            }
        }
        
        if (frame > 150 && !laser_active && player_shoot_timer == 0) {
            // Shoot at hulk
            if (player_x > hulk_x - 10) {
                laser_active = true;
                laser_x = player_x;
                player_shoot_timer = 30;
            }
        }
        
        // Update laser
        if (laser_active) {
            laser_x += 0.3;
            
            // Check hits
            if (!grunt1_dead && abs(laser_x - grunt1_x) < 1) {
                grunt1_dead = true;
                laser_active = false;
            }
            if (!grunt2_dead && abs(laser_x - grunt2_x) < 1) {
                grunt2_dead = true;
                laser_active = false;
            }
            // Hulk gets pushed
            if (abs(laser_x - hulk_x) < 1) {
                hulk_x += 2;  // Push hulk forward
                laser_active = false;
            }
            
            if (laser_x > g_term_cols) {
                laser_active = false;
            }
        }
        
        if (player_shoot_timer > 0) player_shoot_timer--;
        
        // Draw entities
        // Mommy (pink)
        if (mommy_x > 0 && mommy_x < g_term_cols) {
            printf("\x1b[%d;%dH\x1b[38;2;255;100;200m██\x1b[0m", anim_row, (int)mommy_x);
        }
        
        // Hulk (green)
        if (hulk_x > 0 && hulk_x < g_term_cols) {
            printf("\x1b[%d;%dH\x1b[38;2;0;255;0m████\x1b[0m", anim_row, (int)hulk_x);
        }
        
        // Grunts (red)
        if (!grunt1_dead && grunt1_x > 0 && grunt1_x < g_term_cols) {
            printf("\x1b[%d;%dH\x1b[38;2;220;55;55m██\x1b[0m", anim_row, (int)grunt1_x);
        }
        if (!grunt2_dead && grunt2_x > 0 && grunt2_x < g_term_cols) {
            printf("\x1b[%d;%dH\x1b[38;2;220;55;55m██\x1b[0m", anim_row, (int)grunt2_x);
        }
        
        // Player (white)
        if (player_x > 0 && player_x < g_term_cols) {
            printf("\x1b[%d;%dH\x1b[38;2;255;255;255m██\x1b[0m", anim_row, (int)player_x);
        }
        
        // Laser (yellow)
        if (laser_active && laser_x > 0 && laser_x < g_term_cols) {
            printf("\x1b[%d;%dH\x1b[38;2;255;255;0m█\x1b[0m", anim_row, (int)laser_x);
        }
        
        fflush(stdout);
        frame++;
        usleep(16000);  // ~60fps
    }
}

static void play_intro_screen(void) {
    printf("\x1b[2J\x1b[H");
    
    load_and_draw_ansi_logo("blopotron.ans");
    fflush(stdout);
    usleep(800000); // Brief pause before gameplay initialization
    draw_marquee_text();
    draw_keyboard_schematic();
    
    const char* prompt = "INSERT TOKEN TO CONTINUE";
    int prompt_len = strlen(prompt);
    int prompt_col = (g_term_cols - prompt_len) / 2 - 1;
    if (prompt_col < 1) prompt_col = 1;
    int prompt_row = g_term_rows - 1;
    
    unsigned char r = 255, g = 175, b = 0; // Amber
    bool visible = true;
    bool done = false;
    
    while (!done) {
        if (visible) {
            printf("\x1b[%d;%dH\x1b[38;2;%d;%d;%dm%s\x1b[0m", prompt_row, prompt_col, r, g, b, prompt);
        } else {
            printf("\x1b[%d;%dH", prompt_row, prompt_col);
            for (int i = 0; i < prompt_len; i++) putchar(' ');
        }
        fflush(stdout);
        
        // Drastically simplified: poll for any key
        if (check_any_key_pressed()) {
            done = true;
        } else {
            usleep(420000); // 250ms blink interval
            visible = !visible;
        }
    }
    
    // Clean up: erase the prompt completely before game starts
    printf("\x1b[%d;%dH", prompt_row, prompt_col);
    for (int i = 0; i < prompt_len; i++) putchar(' ');
    fflush(stdout);
    
    usleep(500000); // Brief pause before gameplay initialization
}


// ============================================================
//  INTRO ANIMATION: Classic 80s ANSI Zooming Boxes
// ============================================================
static void play_zooming_boxes_text(void) {
    const char *CURSOR_HOME = "\x1b[H";
    const char *CLEAR_SCREEN = "\x1b[2J";
    const char *RESET_STR = "\x1b[0m";
    const char *HIDE_CURSOR = "\x1b[?25l";
    const char *SHOW_CURSOR = "\x1b[?25h";
    const char *BLACK = "\x1b[30m";
    
    static int num_colors = 10;
    static const char* colors[10] = {
        "\x1b[95m", "\x1b[91m", "\x1b[38;5;208m", "\x1b[93m", "\x1b[92m",
        "\x1b[96m", "\x1b[94m", "\x1b[34m", "\x1b[38;5;129m", "\x1b[95m"
    };
    
    int blag = 22; // The "lag" trail length
    int center_x = g_term_cols / 2;
    int center_y = g_term_rows / 2;
    int max_w = g_term_cols;
    int max_h = g_term_rows;
    
    // Calculate frames based on terminal size to ensure it fills the screen
    int numframes = (g_term_rows / 2) < (g_term_cols / 4) ? (g_term_rows / 2) : (g_term_cols / 4);
    
    typedef struct { 
        int bx1, by1, bx2, by2; 
        const char* color; 
    } TBox;
    
    TBox myboxes[128];
    int bwidth = max_w - 2;
    int bheight = max_h - 2;
    
    // Pre-calculate all box dimensions and colors
    for (int i = numframes - 1; i >= 0; i--) {
        myboxes[i].bx1 = center_x - (bwidth / 2);
        myboxes[i].by1 = center_y - (bheight / 2);
        myboxes[i].bx2 = center_x + (bwidth / 2);
        myboxes[i].by2 = center_y + (bheight / 2);
        myboxes[i].color = colors[i % num_colors];
        bwidth -= 4; 
        bheight -= 2;
    }
    
    int x, y;
    printf("%s%s%s", CLEAR_SCREEN, CURSOR_HOME, HIDE_CURSOR);
    fflush(stdout);
    
    // Animation loop
    for (int frame = 1; frame < (int)(1.89 * numframes); frame++) {
        int fr = frame; 
        if (fr > numframes - 1) fr = numframes - 1;
        
        int er = (frame - blag); 
        if (er < 0) er = 0; 
        if (er > numframes - 1) er = numframes - 1;
        
        // 1. Draw the new leading box
        TBox *box = &myboxes[fr];
        printf("%s", box->color);
        
        // Top and bottom edges
        printf("\x1b[%d;%dH", box->by1 + 1, box->bx1 + 1);
        for (x = box->bx1; x <= box->bx2; x++) printf("─");
        printf("\x1b[%d;%dH", box->by2 + 1, box->bx1 + 1);
        for (x = box->bx1; x <= box->bx2; x++) printf("─");
        
        // Left and right edges + corners
        y = box->by1; 
        printf("\x1b[%d;%dH┌", y + 1, box->bx1 + 1);
        for (y = box->by1 + 1; y <= box->by2 - 1; y++) printf("\x1b[%d;%dH│", y + 1, box->bx1 + 1);
        printf("\x1b[%d;%dH└", y + 1, box->bx1 + 1);
        
        y = box->by1; 
        printf("\x1b[%d;%dH┐", y + 1, box->bx2 + 1);
        for (y = box->by1 + 1; y <= box->by2 - 1; y++) printf("\x1b[%d;%dH│", y + 1, box->bx2 + 1);
        printf("\x1b[%d;%dH┘", y + 1, box->bx2 + 1);
        
        // 2. Erase the trailing box with black spaces
        box = &myboxes[er];
        printf("%s", BLACK);
        
        printf("\x1b[%d;%dH", box->by1 + 1, box->bx1 + 1);
        for (x = box->bx1; x <= box->bx2; x++) printf(" ");
        printf("\x1b[%d;%dH", box->by2 + 1, box->bx1 + 1);
        for (x = box->bx1; x <= box->bx2; x++) printf(" ");
        
        y = box->by1; 
        printf("\x1b[%d;%dH ", y + 1, box->bx1 + 1);
        for (y = box->by1 + 1; y <= box->by2 - 1; y++) printf("\x1b[%d;%dH ", y + 1, box->bx1 + 1);
        printf("\x1b[%d;%dH ", y + 1, box->bx1 + 1);
        
        y = box->by1; 
        printf("\x1b[%d;%dH ", y + 1, box->bx2 + 1);
        for (y = box->by1 + 1; y <= box->by2 - 1; y++) printf("\x1b[%d;%dH ", y + 1, box->bx2 + 1);
        printf("\x1b[%d;%dH ", y + 1, box->bx2 + 1);
        
        printf("%s", RESET_STR);
        fflush(stdout);
        usleep(33333); // ~30fps
    }
    
    // Cleanup: show cursor and reset attributes
    /* Reset colors only — do NOT show cursor here.
     * The cursor was hidden by init_text_mode() at game start and
     * is shown by fini_text_mode() on cleanup.  Re-showing it here
     * leaves it visible during the gameplay loop, causing a flicker
     * at the bottom-right of the screen every frame.
     * (DEC private mode: \x1b[?25l = hide, \x1b[?25h = show.) */
    printf("%s", RESET_STR);
    fflush(stdout);
}










// ============================================================
//  SPRITE WALK-TABLE EDITOR MODE  (`btk -e`)
// ============================================================
//
// Two-view interactive workbench for hand-tuning sprite walk cycles.
//
//   View 1: Entity-sprite-selector
//     - 5-cell horizontal grid, one per sprite-bearing entity
//     - Each cell shows entity name, number, and a preview of
//       frame 0 from walk_s
//     - Press 1..5 to enter View 2 with that entity
//     - ESC or Q to quit
//
//   View 2: Walktable-editor
//     - Live arena where the sprite moves under manual control
//     - Movement keys qwe/asd/zxc step the sprite by 1/2 row
//       vertically or 1 full column horizontally.  Diagonals = both.
//     - Orthogonal keys (w,a,d,x) update facing; diagonals don't;
//       s (center) is a no-op.
//     - Tab cycles facing N->S->E->W (no movement).
//     - Enter opens walk-table string editor; Enter again commits.
//     - + / - increment/decrement the per-axis stride multiplier
//       (scaleframewalk_x for E/W facing, scaleframewalk_y for N/S).
//     - S saves the active entity's edited walk tables + scale
//       values to sprites.h (with .bak backup).
//     - ESC returns to View 1.  Unsaved-to-disk edits are silently
//       discarded (Enter in the string editor commits to the
//       in-memory shadow; S persists the shadow to disk).
//
// All rendering reuses the existing text_buffer + flush_text_buffer()
// pipeline.  The live sprite uses the same render_sprite_frame() as
// the game, so what you see is what you get.

#define SPRITES_H_FILENAME "sprites.h"

/* Note: EditorWalk struct, g_editor_active, g_editor_entity,
 * g_editor_walks[][], and the editor_get_scale_x/y forward
 * declarations are defined earlier in the file (near the top, before
 * get_current_frame()) because get_current_frame() needs to see them. */

/* Map EntityType to its SpriteSet* (consults both real and stub sets). */
static const SpriteSet* editor_get_sprite_set(EntityType type) {
    switch (type) {
        case ENT_PLAYER:    return &sprite_player_set;
        case ENT_GRUNT:     return &sprite_grunt_set;
        case ENT_QUARK:     return &sprite_quark_set;
        case ENT_HULK:      return &sprite_hulk_set;
        case ENT_BRAIN:     return &sprite_brain_set;
        case ENT_SPHEROID:  return &sprite_spheroid_set;
        case ENT_ENFORCER:  return &sprite_enforcer_set;
        case ENT_HUMAN:     return &sprite_human_set;
        case ENT_LASER:     return &sprite_laser_set;
        case ENT_TERROR:    return &sprite_terror_set;
        case ENT_ELECTRODE: return &sprite_electrode_set;
        case ENT_CRUISE:    return &sprite_cruise_set;
        default:            return NULL;
    }
}

/* C-identifier prefix used in sprites.h (e.g. "grunt" for grunt_walk_n). */
static const char* editor_entity_prefix(EntityType type) {
    switch (type) {
        case ENT_PLAYER:    return "player";
        case ENT_GRUNT:     return "grunt";
        case ENT_QUARK:     return "quark";
        case ENT_HULK:      return "hulk";
        case ENT_BRAIN:     return "brain";
        case ENT_SPHEROID:  return "spheroid";
        case ENT_ENFORCER:  return "enforcer";
        case ENT_HUMAN:     return "human";
        case ENT_LASER:     return "laser";
        case ENT_TERROR:    return "terror";
        case ENT_ELECTRODE: return "electrode";
        case ENT_CRUISE:    return "cruise";
        default:            return NULL;
    }
}

/* Pretty label for the HUD. */
static const char* editor_entity_label(EntityType type) {
    switch (type) {
        case ENT_PLAYER:    return "Player";
        case ENT_GRUNT:     return "Grunt";
        case ENT_QUARK:     return "Quark";
        case ENT_HULK:      return "Hulk";
        case ENT_BRAIN:     return "Brain";
        case ENT_SPHEROID:  return "Spheroid";
        case ENT_ENFORCER:  return "Enforcer";
        case ENT_HUMAN:     return "Human";
        case ENT_LASER:     return "Laser";
        case ENT_TERROR:    return "Terror";
        case ENT_ELECTRODE: return "Electrode";
        case ENT_CRUISE:    return "Cruise";
        default:            return "?";
    }
}

/* Per-axis stride multipliers live in FacingInfo (see struct above):
 *   effective advance rate = (cells * scale) / step_period
 *      -- scale_x applies to E/W facings, scale_y to N/S
 *      -- bigger scale = FASTER animation (more frame advances per
 *         unit of travel)
 *      -- scale=1 reproduces legacy `cells / step_period` behaviour
 *         bit-for-bit
 *      -- scale values larger than step_period yield fractional
 *         effective strides (e.g. step_period=4, scale=8 -> one
 *         frame advance per half-cell, which is what the Hulk N/S
 *         walk needs to feel responsive)
 * Edited via `btk -e` (+/- keys) and persisted to sprites.h inside
 * each SpriteSet's facing[] initializer.  When the editor is active,
 * the shadow copy in EditorWalk (see below) overrides the const
 * value so the user sees live changes; on save, the shadow values
 * are written back into the SpriteSet initializer. */

/* Forward decls -- the actual definitions live further down. */
static const EntityType g_editor_entities[];
#define G_EDITOR_ENTITY_COUNT \
    (int)(sizeof(g_editor_entities)/sizeof(g_editor_entities[0]))
static const char* editor_entity_prefix(EntityType type);

/* List of entity types that appear in the selector gallery.
 * Ordered to match the hex key sequence 0..E:
 *   0=Player  1=Grunt  2=Quark  3=Hulk   4=Brain
 *   5=Spheroid 6=Enforcer 7=Human 8=Laser 9=Terror
 *   A=Electrode  B=Cruise  C-E reserved for future sprites
 * Hex key F is accepted by the input handler but currently maps to
 * no entity (returns -1, no selection). */
static const EntityType g_editor_entities[] = {
    ENT_PLAYER, ENT_GRUNT, ENT_QUARK, ENT_HULK, ENT_BRAIN,
    ENT_SPHEROID, ENT_ENFORCER, ENT_HUMAN, ENT_LASER, ENT_TERROR,
    ENT_ELECTRODE, ENT_CRUISE
};

/* Initialize shadow walk tables + scale values.
 * Walk indices come from the compiled-in const SpriteSet (these are
 * not expected to drift between compiles -- if they do, btk rebuilds).
 * Scale values, however, are read fresh from sprites.h on each editor
 * entry so that values saved by a previous `btk -e` session survive
 * even if btk hasn't been recompiled since the save.  If sprites.h
 * can't be opened or doesn't contain scale values for this entity,
 * falls back to the compiled-in const values (default 1,1). */
static void editor_init_shadow(EntityType type) {
    const SpriteSet* ss = editor_get_sprite_set(type);
    if (!ss) return;
    const char* prefix = editor_entity_prefix(type);

    /* Load current scale values from sprites.h (best-effort). */
    int file_sx[4] = {0,0,0,0}, file_sy[4] = {0,0,0,0};
    bool found_in_file = false;
    if (prefix) {
        FILE* f = fopen(SPRITES_H_FILENAME, "r");
        if (f) {
            char line[1024];
            while (fgets(line, sizeof(line), f)) {
                char wpat[64];
                snprintf(wpat, sizeof(wpat), "%s_walk_", prefix);
                char* hit = strstr(line, wpat);
                if (!hit) continue;
                char d = *(hit + strlen(wpat));
                int fidx = -1;
                if (d == 'n' || d == 'N') fidx = 0;
                else if (d == 's' || d == 'S') fidx = 1;
                else if (d == 'e' || d == 'E') fidx = 2;
                else if (d == 'w' || d == 'W') fidx = 3;
                if (fidx < 0) continue;
                /* Find the closing } and walk backward to extract
                 * the last two comma-separated integers (sx, sy). */
                char* close = strchr(line, '}');
                if (!close) continue;
                char* p = close - 1;
                int vals[2] = {0, 0};
                int vi = 1;
                bool ok = true;
                while (p > hit && vi >= 0) {
                    if (*p == ',') {
                        char* np = p + 1;
                        while (*np == ' ' || *np == '\t') np++;
                        if (*np < '0' || *np > '9') { ok = false; break; }
                        int v = 0;
                        while (*np >= '0' && *np <= '9') {
                            v = v * 10 + (*np - '0'); np++;
                        }
                        vals[vi] = v;
                        vi--;
                    }
                    p--;
                }
                if (ok) {
                    file_sx[fidx] = vals[0];
                    file_sy[fidx] = vals[1];
                    found_in_file = true;
                }
            }
            fclose(f);
        }
    }

    for (int f = 0; f < 4; f++) {
        const FacingInfo* fi = &ss->facing[f];
        EditorWalk* ew = &g_editor_walks[type][f];
        int n = fi->count;
        if (n > EDITOR_MAX_WALK_LEN) n = EDITOR_MAX_WALK_LEN;
        for (int i = 0; i < n; i++) {
            ew->indices[i] = fi->frame_indices[i];
        }
        ew->count = n;
        /* Prefer file values (most recent) over compiled-in. */
        if (found_in_file && file_sx[f] > 0 && file_sy[f] > 0) {
            ew->scale_x = file_sx[f];
            ew->scale_y = file_sy[f];
        } else {
            ew->scale_x = fi->scale_x > 0 ? fi->scale_x : 1;
            ew->scale_y = fi->scale_y > 0 ? fi->scale_y : 1;
        }
        ew->dirty = false;
    }

    /* Per-frame ox/oy shadow init: seed from compiled-in SpriteFrame
     * values, then overwrite with values parsed from sprites.h if
     * present (so saved-but-not-recompiled edits survive re-entry). */
    for (int i = 0; i < EDITOR_MAX_FRAMES; i++) {
        if (i < ss->total_frames) {
            g_editor_frame_offsets[type][i].ox = ss->frames[i].ox;
            g_editor_frame_offsets[type][i].oy = ss->frames[i].oy;
        } else {
            g_editor_frame_offsets[type][i].ox = 0;
            g_editor_frame_offsets[type][i].oy = 0;
        }
        g_editor_frame_offsets[type][i].dirty = false;
    }
    if (prefix) {
        FILE* f2 = fopen(SPRITES_H_FILENAME, "r");
        if (f2) {
            char line2[1024];
            char fpat[64];
            snprintf(fpat, sizeof(fpat),
                     "static const SpriteFrame %s_f", prefix);
            size_t fpatlen = strlen(fpat);
            while (fgets(line2, sizeof(line2), f2)) {
                if (strncmp(line2, fpat, fpatlen) != 0) continue;
                /* Parse frame index N from "<prefix>_fN = {" */
                char* p = line2 + fpatlen;
                if (*p < '0' || *p > '9') continue;
                int fidx = 0;
                while (*p >= '0' && *p <= '9') {
                    fidx = fidx * 10 + (*p - '0');
                    p++;
                }
                if (fidx < 0 || fidx >= EDITOR_MAX_FRAMES) continue;
                /* Find closing '}' and walk backward to extract last
                 * two comma-separated integers (ox, oy). */
                char* close = strchr(line2, '}');
                if (!close) continue;
                char* c = close - 1;
                int vals[2] = {0, 0};
                int vi = 1;
                bool ok = true;
                while (c > line2 && vi >= 0) {
                    if (*c == ',') {
                        char* np = c + 1;
                        while (*np == ' ' || *np == '\t') np++;
                        if (*np < '0' || *np > '9') { ok = false; break; }
                        int v = 0;
                        while (*np >= '0' && *np <= '9') {
                            v = v * 10 + (*np - '0'); np++;
                        }
                        vals[vi] = v;
                        vi--;
                    }
                    c--;
                }
                if (ok) {
                    g_editor_frame_offsets[type][fidx].ox = vals[0];
                    g_editor_frame_offsets[type][fidx].oy = vals[1];
                }
            }
            fclose(f2);
        }
    }
}

/* World-coordinate delta for one half-row of vertical movement. */
static int16_t editor_half_row_step(void) {
    return (int16_t)(((int32_t)SCREEN_HEIGHT * COORD_SCALE) /
                     ((int32_t)g_term_rows * 2));
}
/* World-coordinate delta for one full column of horizontal movement. */
static int16_t editor_full_col_step(void) {
    return (int16_t)(((int32_t)SCREEN_WIDTH * COORD_SCALE) /
                     ((int32_t)g_term_cols));
}

/* ---------- text blitting ---------- */

/* Blit an ASCII string into text_buffer at row/col (no wrapping).
 *   row, col: 0-indexed terminal coordinates.
 *   r,g,b:    foreground RGB.
 *   invert:   if true, swap fg/bg (bg = fg, fg = black).
 */
static void editor_blit_str(int row, int col, const char* s,
                            uint8_t r, uint8_t g, uint8_t b, bool invert) {
    if (!text_buffer || row < 0 || row >= g_term_rows) return;
    for (int i = 0; s[i] != '\0'; i++) {
        int c = col + i;
        if (c < 0 || c >= g_term_cols) break;
        TextCell* cell = &text_buffer[row * g_term_cols + c];
        char ch = s[i];
        if (ch < 32 || ch > 126) ch = '?';
        cell->glyph[0] = ch;
        cell->glyph[1] = '\0';
        if (invert) {
            cell->r = 0; cell->g = 0; cell->b = 0;
            cell->br = r; cell->bg = g; cell->bb = b;
        } else {
            cell->r = r; cell->g = g; cell->b = b;
            cell->br = 0; cell->bg = 0; cell->bb = 0;
        }
    }
}

/* ---------- walk-table string editor ---------- */

/* Parse a CSV string like "0, 1, 2, 3" into an EditorWalk.
 * Returns true on success (all tokens valid integers in [0, frame_count)).
 * Returns false on any error; *out is left untouched.
 */
static bool editor_parse_walk_string(const char* s, int frame_count,
                                     EditorWalk* out) {
    EditorWalk tmp;
    tmp.count = 0;
    int i = 0;
    while (s[i] != '\0') {
        /* skip separators */
        while (s[i] == ' ' || s[i] == ',' || s[i] == '\t') i++;
        if (s[i] == '\0') break;
        /* parse integer */
        if (s[i] < '0' || s[i] > '9') return false;
        int val = 0;
        while (s[i] >= '0' && s[i] <= '9') {
            val = val * 10 + (s[i] - '0');
            if (val > 1000000) return false;   /* sanity */
            i++;
        }
        if (val < 0 || val >= frame_count) return false;
        if (tmp.count >= EDITOR_MAX_WALK_LEN) return false;
        tmp.indices[tmp.count++] = val;
        /* skip trailing space before separator or end */
        while (s[i] == ' ' || s[i] == '\t') i++;
    }
    /* An empty string is allowed -- means "no walk table, use frame 0". */
    *out = tmp;
    return true;
}

/* ---------- save to sprites.h ---------- */

/* Surgical text replacement: read sprites.h, replace only the active
 * entity's four walk-table arrays and two scaleframewalk vars.
 * Writes sprites.h.bak first as a backup.  Returns true on success. */
static bool editor_save_sprites_h(EntityType type) {
    const char* prefix = editor_entity_prefix(type);
    if (!prefix) return false;

    /* 1. Rename sprites.h -> sprites.h.bak (overwrite existing .bak). */
    FILE* probe = fopen(SPRITES_H_FILENAME, "r");
    if (!probe) return false;
    fclose(probe);
    if (rename(SPRITES_H_FILENAME, SPRITES_H_FILENAME ".bak") != 0) {
        /* rename failed -- try to continue by reopening as read */
    }

    FILE* in  = fopen(SPRITES_H_FILENAME ".bak", "r");
    if (!in) return false;
    FILE* out = fopen(SPRITES_H_FILENAME, "w");
    if (!out) { fclose(in); return false; }

    char line[1024];
    bool  in_walk_block = false;
    char  walk_dir = 0;

    while (fgets(line, sizeof(line), in)) {
        if (in_walk_block) {
            /* Skip input lines until we find the closing }; for the
             * walk array we replaced.  Emit nothing. */
            if (strstr(line, "};")) {
                fprintf(out, "};\n");
                in_walk_block = false;
            }
            continue;
        }

        /* Detect walk-table header:  static const int <prefix>_walk_X[] = ... */
        char pattern[128];
        snprintf(pattern, sizeof(pattern),
                 "static const int %s_walk_", prefix);
        size_t plen = strlen(pattern);
        if (strncmp(line, pattern, plen) == 0) {
            /* Extract direction letter */
            char* p = line + plen;
            walk_dir = *p;   /* e.g. 'n' */
            in_walk_block = true;

            /* Build the new array */
            EditorWalk* ew = NULL;
            switch (walk_dir) {
                case 'n': case 'N': ew = &g_editor_walks[type][0]; break;
                case 's': case 'S': ew = &g_editor_walks[type][1]; break;
                case 'e': case 'E': ew = &g_editor_walks[type][2]; break;
                case 'w': case 'W': ew = &g_editor_walks[type][3]; break;
            }
            fprintf(out, "static const int %s_walk_%c[] = {\n",
                    prefix, walk_dir);
            if (ew && ew->count > 0) {
                fprintf(out, "    ");
                for (int i = 0; i < ew->count; i++) {
                    fprintf(out, "%d%s", ew->indices[i],
                            (i + 1 < ew->count) ? "," : "");
                }
                fprintf(out, "\n");
            }
            /* If the original line had }; on the same line, emit it now. */
            if (strstr(line, "};")) {
                fprintf(out, "};\n");
                in_walk_block = false;
            }
            continue;
        }

        /* Detect FacingInfo initializer lines inside the SpriteSet.
         * These look like:  { hulk_walk_n, 7, 4, 0, 0 }, /* N * /
         * We replace the last two numbers (scale_x, scale_y) with the
         * shadow values for that facing.  The line is matched by
         * looking for "<prefix>_walk_<dir>" as a substring. */
        {
            char wpat[64];
            snprintf(wpat, sizeof(wpat), "%s_walk_", prefix);
            char* hit = strstr(line, wpat);
            if (hit) {
                char* dirp = hit + strlen(wpat);
                char d = *dirp;
                int fidx = -1;
                if (d == 'n' || d == 'N') fidx = 0;
                else if (d == 's' || d == 'S') fidx = 1;
                else if (d == 'e' || d == 'E') fidx = 2;
                else if (d == 'w' || d == 'W') fidx = 3;
                if (fidx >= 0) {
                    EditorWalk* ew = &g_editor_walks[type][fidx];
                    int sx = ew->scale_x > 0 ? ew->scale_x : 1;
                    int sy = ew->scale_y > 0 ? ew->scale_y : 1;
                    /* Find the trailing "}" (the closing brace of the
                     * FacingInfo initializer).  We rewrite the line
                     * from the last comma before '}' onward. */
                    char* close = strchr(line, '}');
                    if (close) {
                        /* Walk backward from close to find the last
                         * two comma-separated numbers. */
                        char* c2 = close - 1;
                        int nums[2] = {0, 0};
                        int nidx = 1;
                        bool ok = true;
                        while (c2 > hit && nidx >= 0) {
                            if (*c2 == ',') {
                                /* parse the number after this comma */
                                char* np = c2 + 1;
                                while (*np == ' ' || *np == '\t') np++;
                                if (*np < '0' || *np > '9') { ok = false; break; }
                                int v = 0;
                                while (*np >= '0' && *np <= '9') {
                                    v = v * 10 + (*np - '0');
                                    np++;
                                }
                                nums[nidx] = v;
                                nidx--;
                            }
                            c2--;
                        }
                        if (ok) {
                            (void)nums;  /* discard old values */
                            /* Truncate line at the first comma that
                             * precedes the two scale values, then
                             * append our new ones.  We find the comma
                             * before 'oy' (the 5th field).  Strategy:
                             * count commas from the start of the
                             * FacingInfo block; we want to truncate
                             * after the 4th comma. */
                            /* Simpler: find the comma that starts the
                             * scale_x field.  We know the structure
                             * is { indices, count, step, ox, oy, sx, sy }.
                             * So we want to truncate at the comma
                             * after oy (the 5th value).  Count commas
                             * from hit to close: there should be 6. */
                            int comma_count = 0;
                            char* trunc_pos = NULL;
                            for (char* p = hit; p < close; p++) {
                                if (*p == ',') {
                                    comma_count++;
                                    if (comma_count == 5) {
                                        trunc_pos = p;
                                        break;
                                    }
                                }
                            }
                            if (trunc_pos) {
                                /* Write the line up to and including
                                 * trunc_pos, then append the new sx,sy. */
                                *trunc_pos = '\0';
                                fprintf(out, "%s, %d, %d }%s",
                                        line, sx, sy,
                                        close + 1);  /* rest after } */
                                continue;
                            }
                        }
                    }
                }
            }
        }

        /* Detect SpriteFrame initializer lines and rewrite the ox/oy
         * fields (the last two integers before '}') if the shadow has
         * a dirty override for that frame.  Line shape:
         *
         *   static const SpriteFrame <prefix>_f<N> = { <rows>, <w>, <h>, <ox>, <oy> };
         *
         * We parse N from the identifier, then truncate at the comma
         * preceding oy and append our new ox, oy values. */
        {
            char fpat[64];
            snprintf(fpat, sizeof(fpat),
                     "static const SpriteFrame %s_f", prefix);
            size_t fpatlen = strlen(fpat);
            if (strncmp(line, fpat, fpatlen) == 0) {
                char* p = line + fpatlen;
                if (*p >= '0' && *p <= '9') {
                    int fidx = 0;
                    while (*p >= '0' && *p <= '9') {
                        fidx = fidx * 10 + (*p - '0');
                        p++;
                    }
                    if (fidx >= 0 && fidx < EDITOR_MAX_FRAMES
                        && g_editor_frame_offsets[type][fidx].dirty) {
                        EditorFrameOffset* efo =
                            &g_editor_frame_offsets[type][fidx];
                        char* close = strchr(line, '}');
                        if (close) {
                            /* Walk backward from close to find the comma
                             * that precedes the ox field (the 4th comma
                             * from the start of the initializer). */
                            int comma_count = 0;
                            char* trunc_pos = NULL;
                            for (char* c = line; c < close; c++) {
                                if (*c == ',') {
                                    comma_count++;
                                    if (comma_count == 4) {
                                        trunc_pos = c;
                                        break;
                                    }
                                }
                            }
                            if (trunc_pos) {
                                *trunc_pos = '\0';
                                fprintf(out, "%s, %d, %d }%s",
                                        line, efo->ox, efo->oy,
                                        close + 1);
                                continue;
                            }
                        }
                    }
                }
            }
        }

        /* Default: pass through unchanged. */
        fputs(line, out);
    }

    fclose(in);
    fclose(out);
    return true;
}

/* ---------- View 1: entity selector ---------- */

/* Gallery layout: 5 columns x 3 rows = 15 visible cells.
 * Hex keys 0-9 + A-E select among the 12 active entity types
 * (currently 0-B used; C-E reserved for future sprites). */
#define SELECTOR_COLS 5
#define SELECTOR_ROWS 3
#define SELECTOR_CELL_COUNT (SELECTOR_COLS * SELECTOR_ROWS)  /* 15 */

/* Translate a pressed key into a gallery index, or -1 if not a hex key.
 * Accepts 0-9 and A-F (case-insensitive).  Valid range is 0..14 for
 * the 15 visible cells; 15 (key F) is accepted by the parser but has
 * no entity mapped to it (caller treats it as a no-op). */
static int selector_hex_key_to_index(int key) {
    if (key >= '0' && key <= '9') return key - '0';
    if (key >= 'a' && key <= 'f') return 10 + (key - 'a');
    if (key >= 'A' && key <= 'F') return 10 + (key - 'A');
    return -1;
}

/* Render the single-char hex label for a cell index (0..15). */
static char selector_index_to_hex(int i) {
    if (i < 10) return (char)('0' + i);
    if (i < 16) return (char)('A' + (i - 10));
    return '?';
}

static void editor_render_selector(void) {
    clear_text_buffer();

    /* Title + instructions */
    editor_blit_str(0, 2, "BLAPOTRON SPRITE EDITOR -- select entity",
                    255, 255, 0, false);
    editor_blit_str(1, 2, "press 0-9, A-E to select    ESC/Q to quit",
                    180, 180, 180, false);

    /* 5-column x 3-row grid.  Each cell ~ (cols-4)/5 wide,
     * (rows-6)/3 tall, with 1-row gaps between rows. */
    int cell_w = (g_term_cols - 4) / SELECTOR_COLS;
    int avail_h = g_term_rows - 4;  /* below title+instructions */
    int cell_h = (avail_h - (SELECTOR_ROWS - 1)) / SELECTOR_ROWS;
    int top0   = 3;

    if (cell_h < 6) cell_h = 6;  /* floor: header+border+1 preview row */

    for (int i = 0; i < SELECTOR_CELL_COUNT; i++) {
        int col = i % SELECTOR_COLS;
        int row = i / SELECTOR_COLS;
        int left = 2 + col * cell_w;
        int top  = top0 + row * (cell_h + 1);

        EntityType et = (i < G_EDITOR_ENTITY_COUNT)
                      ? g_editor_entities[i]
                      : ENT_PLAYER;  /* unused for empty cells */
        const SpriteSet* ss = (i < G_EDITOR_ENTITY_COUNT)
                            ? editor_get_sprite_set(et)
                            : NULL;

        /* Cell border (top + bottom horizontal lines) */
        for (int x = 0; x < cell_w - 1; x++) {
            editor_blit_str(top, left + x, "-",
                            100, 100, 100, false);
            editor_blit_str(top + cell_h - 1, left + x, "-",
                            100, 100, 100, false);
        }
        /* Side borders */
        for (int y = 0; y < cell_h; y++) {
            editor_blit_str(top + y, left, "|",
                            100, 100, 100, false);
            editor_blit_str(top + y, left + cell_w - 1, "|",
                            100, 100, 100, false);
        }

        /* Hex key + entity name (or "reserved" placeholder). */
        char num[8];
        snprintf(num, sizeof(num), "[%c]",
                 selector_index_to_hex(i));
        editor_blit_str(top + 1, left + 2, num,
                        255, 255, 0, false);

        if (i < G_EDITOR_ENTITY_COUNT) {
            const char* label = editor_entity_label(et);
            editor_blit_str(top + 1, left + 6, label,
                            255, 255, 255, false);
        } else {
            editor_blit_str(top + 1, left + 6, "(reserved)",
                            80, 80, 80, false);
            continue;  /* no preview for empty cells */
        }

        /* Preview: frame 0 of walk_s (or frame 0 if none). */
        if (ss && ss->total_frames > 0) {
            int fi = 0;
            const FacingInfo* ws = &ss->facing[1];   /* S */
            if (ws && ws->count > 0 && ws->frame_indices) {
                fi = ws->frame_indices[0];
                if (fi < 0 || fi >= ss->total_frames) fi = 0;
            }
            const SpriteFrame* sf = &ss->frames[fi];
            /* Center the preview in the cell, leaving room for header
             * (top +1) and frame label (bottom -2). */
            int preview_top    = top + 2;
            int preview_bottom = top + cell_h - 2;
            int preview_h      = preview_bottom - preview_top;
            int px = left + (cell_w - sf->w) / 2;
            int py = preview_top + (preview_h - sf->h) / 2;
            if (py < preview_top) py = preview_top;
            render_sprite_frame(sf, px, py);

            /* Frame label */
            char fl[32];
            snprintf(fl, sizeof(fl), "frame %d (%dx%d)",
                     fi, sf->w, sf->h);
            editor_blit_str(top + cell_h - 2, left + 2, fl,
                            120, 180, 120, false);
        } else {
            editor_blit_str(top + cell_h - 2, left + 2, "no sprite",
                            160, 80, 80, false);
        }
    }

    flush_text_buffer();
}

/* Returns the chosen EntityType, or -1 if user pressed ESC/Q. */
static int editor_view_selector(void) {
    for (;;) {
        poll_input();
        if (g_keys[27] || g_keys['Q'] || g_keys['q']) return -1;
        for (int k = 0; k < 256; k++) {
            if (!g_keys[k]) continue;
            int idx = selector_hex_key_to_index(k);
            if (idx < 0) continue;
            /* F (idx 15) has no entity yet; ignore silently. */
            if (idx >= G_EDITOR_ENTITY_COUNT) continue;
            return g_editor_entities[idx];
        }
        editor_render_selector();
        usleep(16000);
    }
}

/* ---------- View 2: walktable editor ---------- */

/* Render the live sprite + HUD + thumbnails + optional edit string. */
static void editor_render_walktable(int16_t wx, int16_t wy, int facing,
                                    const char* edit_str, int edit_cursor,
                                    bool edit_active, bool edit_error) {
    const SpriteSet* ss = editor_get_sprite_set(g_editor_entity);
    if (!ss) return;

    clear_text_buffer();

    /* 1. Arena = full screen.  Live sprite at wx,wy. */
    int tx = (int)((int32_t)wx * g_term_cols /
                   ((int32_t)COORD_SCALE * SCREEN_WIDTH));
    int ty = (int)((int32_t)wy * g_term_rows /
                   ((int32_t)COORD_SCALE * SCREEN_HEIGHT));

    /* Use the universal frame lookup (which consults the shadow when
     * g_editor_active is true).
     *
     * For the Spheroid we need a temporal anim_phase input -- but the
     * editor preview doesn't have a real Entity* (the sprite is just
     * being rendered at an arbitrary wx,wy with no spawned entity).
     * We synthesize the phase from g_frame_count so the spheroid's
     * within-bank sub-frame still cycles while you're tuning offsets.
     *
     * Math: g_frame_count advances by 1 per game loop iteration.  We
     * divide by SPHEROID_WALK_FRAMES_PER_ADVANCE so the sub-frame
     * advances at the same cadence as it would in gameplay (assuming
     * tick_period=1 here -- in real gameplay tick_period=2 means the
     * spheroid ticks every 2nd frame, but in the editor we want
     * visible motion at full speed so we use 1).  The modulo by 3
     * mirrors what entity_select_frame()'s ENT_SPHEROID case does. */
    int editor_anim_phase = 0;
    if (g_editor_entity == ENT_SPHEROID) {
        editor_anim_phase = (g_frame_count / SPHEROID_WALK_FRAMES_PER_ADVANCE) % 3;
    }
    const SpriteFrame* sf = get_current_frame(ss, facing, wx, wy,
                                              (EntityType)g_editor_entity,
                                              editor_anim_phase);

    /* Compute the current frame_idx early so the live preview and the
     * HUD agree on which frame's ox/oy shadow entry to consult.  This
     * duplicates a small slice of the HUD's step_idx/frame_idx math
     * below, but keeps the render path self-contained. */
    int frame_idx = 0;
    {
        EditorWalk* ew = &g_editor_walks[g_editor_entity][facing];
        if (ew->count > 0) {
            int step_idx = 0;
            /* Half-row index computed in one shot from wy (see
             * entity_select_frame() for the rationale -- the old
             * two-step (wy/16 then *2*term_rows/600) formula had
             * compounding truncation dead zones).  This must match
             * entity_select_frame() so the live preview and the HUD
             * agree on which frame is showing. */
            int32_t half_rows = (int32_t)wy * ((int32_t)g_term_rows * 2)
                              / ((int32_t)SCREEN_HEIGHT * COORD_SCALE);
            if (g_editor_entity == ENT_GRUNT
                || g_editor_entity == ENT_ENFORCER) {
                /* Half-row parity picker (2-frame toggle). */
                step_idx = (int)(half_rows & 1);
            } else if (g_editor_entity == ENT_SPHEROID) {
                /* Spheroid hybrid picker -- must mirror
                 * entity_select_frame()'s ENT_SPHEROID case exactly
                 * so the HUD's "step N/M -> frame_idx K" readout
                 * matches the live blit.  Bank from half-row parity,
                 * sub-frame from the same g_frame_count-derived
                 * anim_phase we passed to get_current_frame() above. */
                int bank = (int)(half_rows & 1);
                int sub  = editor_anim_phase % 3;
                if (sub < 0) sub += 3;
                step_idx = bank * 3 + sub;
            } else {
                /* Default: stride cycle along axis (Hulk etc.). */
                int base_stride = ss->facing[facing].step_period;
                int scale = (facing == DIR_UP || facing == DIR_DOWN)
                          ? ew->scale_y : ew->scale_x;
                if (base_stride <= 0) base_stride = 1;
                int axis = (facing == DIR_UP || facing == DIR_DOWN) ? 1 : 0;
                int16_t pos = axis ? wy : wx;
                step_idx = stride_cycle(pos, base_stride, scale,
                                        ew->count, axis);
            }
            if (step_idx < 0 || step_idx >= ew->count) step_idx = 0;
            frame_idx = ew->indices[step_idx];
        }
    }

    if (sf) {
        const FacingInfo* fi = get_facing_info(ss, facing);
        if (fi) { tx += fi->offset_x; ty += fi->offset_y; }
        /* Per-frame placement offset.  When the editor is active,
         * consult the shadow so live i/j/k/l edits take effect
         * immediately.  Mirrors the in-game render path (which
         * uses sf->ox/sf->oy directly, since the editor is not
         * active during gameplay).  See render_sprite_frame()
         * callers in update_all() for the same fix. */
        int ox = sf->ox, oy = sf->oy;
        if (g_editor_active
            && g_editor_entity >= 0 && g_editor_entity < NUM_ENTITY_TYPES
            && frame_idx >= 0 && frame_idx < EDITOR_MAX_FRAMES) {
            EditorFrameOffset* efo =
                &g_editor_frame_offsets[g_editor_entity][frame_idx];
            ox = efo->ox;
            oy = efo->oy;
        }
        tx += ox; ty += oy;
        render_sprite_frame(sf, tx, ty);
    }

    /* 2. HUD (top-left) */
    const char* face_label = "?";
    switch (facing) {
        case DIR_UP:    face_label = "N"; break;
        case DIR_DOWN:  face_label = "S"; break;
        case DIR_LEFT:  face_label = "W"; break;
        case DIR_RIGHT: face_label = "E"; break;
    }

    char buf[128];
    int  row = 0;

    snprintf(buf, sizeof(buf), "Entity: %s   Facing: %s",
             editor_entity_label(g_editor_entity), face_label);
    editor_blit_str(row++, 0, buf, 255, 255, 0, false);

    snprintf(buf, sizeof(buf), "wx=%d  wy=%d", (int)wx, (int)wy);
    editor_blit_str(row++, 0, buf, 200, 200, 200, false);

    /* Compute screen row/col + fractional half-row indicator.
     * Half-row math in one shot from wy (see entity_select_frame()
     * for rationale -- the old two-step (wy/16 then *2*term_rows/600)
     * formula had compounding truncation dead zones that made the
     * "row=" readout disagree with the actual half-row at certain
     * wy values).  Col math stays two-step since the analogous
     * dead-zone bug is much rarer for the X axis (no half-col
     * picker consumes it). */
    int32_t half_rows = (int32_t)wy * ((int32_t)g_term_rows * 2)
                      / ((int32_t)SCREEN_HEIGHT * COORD_SCALE);
    int whole_row = half_rows / 2;
    int frac      = half_rows & 1;
    int32_t raw_pixels_x = (int32_t)wx / COORD_SCALE;
    int32_t cols = raw_pixels_x * g_term_cols / SCREEN_WIDTH;
    snprintf(buf, sizeof(buf), "screen: row=%d%s  col=%d",
             whole_row, frac ? "+1/2" : "   ", (int)cols);
    editor_blit_str(row++, 0, buf, 200, 200, 200, false);

    /* Current step + frame index -- frame_idx is computed above (in
     * the render section) so the live preview and HUD agree on which
     * frame's ox/oy shadow entry is active.  We still recompute
     * step_idx here for the walk-array highlight below. */
    EditorWalk* ew = &g_editor_walks[g_editor_entity][facing];
    int step_idx = 0;
    if (ew->count > 0) {
        /* Recompute the step index using the same logic as
         * entity_select_frame().  For Grunt and Enforcer this is the
         * half-row bit; for Spheroid it's the hybrid bank+sub math
         * (must match the render path's computation exactly so the
         * "step N/M" readout matches the live blit); for everything
         * else it's stride_cycle along the axis. */
        if (g_editor_entity == ENT_GRUNT
            || g_editor_entity == ENT_ENFORCER) {
            step_idx = (int)(half_rows & 1);
        } else if (g_editor_entity == ENT_SPHEROID) {
            /* Hybrid: bank from half-row parity, sub from the same
             * g_frame_count-derived phase used in the render path.
             * See the matching block in the render path above for
             * the full rationale. */
            int bank = (int)(half_rows & 1);
            int sub  = editor_anim_phase % 3;
            if (sub < 0) sub += 3;
            step_idx = bank * 3 + sub;
        } else {
            int base_stride = ss->facing[facing].step_period;
            int scale = (facing == DIR_UP || facing == DIR_DOWN)
                       ? ew->scale_y : ew->scale_x;
            if (base_stride <= 0) base_stride = 1;
            int axis = (facing == DIR_UP || facing == DIR_DOWN) ? 1 : 0;
            int16_t pos = axis ? wy : wx;
            /* Mirrors get_current_frame(): pass stride + scale
             * separately so stride_cycle's inverted math (bigger
             * scale = faster) applies in the editor preview too. */
            step_idx = stride_cycle(pos, base_stride, scale, ew->count, axis);
        }
        if (step_idx < 0 || step_idx >= ew->count) step_idx = 0;
    }
    snprintf(buf, sizeof(buf), "step %d/%d  ->  frame_idx %d",
             step_idx, ew->count, frame_idx);
    editor_blit_str(row++, 0, buf, 255, 255, 255, false);

    /* Per-frame placement offset (read from the shadow for the
     * currently displayed frame).  i/j/k/l tunes this live; S saves
     * it back to the SpriteFrame initializer in sprites.h. */
    if (g_editor_entity >= 0 && g_editor_entity < NUM_ENTITY_TYPES
        && frame_idx >= 0 && frame_idx < EDITOR_MAX_FRAMES) {
        EditorFrameOffset* efo =
            &g_editor_frame_offsets[g_editor_entity][frame_idx];
        snprintf(buf, sizeof(buf),
                 "frame %d: ox=%d  oy=%d   (i/k oy, j/l ox)",
                 frame_idx, efo->ox, efo->oy);
        editor_blit_str(row++, 0, buf, 255, 200, 100, false);
    }

    /* Scale values (read from the shadow for the active facing).
     * Inverted semantics: bigger scale = FASTER animation.
     * scale=1 is the legacy default; raise it to advance frames
     * more often per unit of travel. */
    snprintf(buf, sizeof(buf),
             "scale_x=%d  scale_y=%d   (+/- tunes %s,  bigger=faster)",
             ew->scale_x, ew->scale_y,
             (facing == DIR_UP || facing == DIR_DOWN) ? "Y" : "X");
    editor_blit_str(row++, 0, buf, 180, 180, 255, false);

    /* Full walk array for current facing */
    char arr[EDITOR_MAX_WALK_LEN * 6 + 32];
    int  pos = 0;
    pos += snprintf(arr + pos, sizeof(arr) - pos, "walk_%c: [",
                    (facing == DIR_UP)   ? 'n' :
                    (facing == DIR_DOWN) ? 's' :
                    (facing == DIR_LEFT) ? 'w' : 'e');
    for (int i = 0; i < ew->count; i++) {
        bool hot = (i == step_idx);
        pos += snprintf(arr + pos, sizeof(arr) - pos,
                        "%s%d%s",
                        hot ? ">" : " ",
                        ew->indices[i],
                        (i + 1 < ew->count) ? "," : " ");
    }
    pos += snprintf(arr + pos, sizeof(arr) - pos, "]");
    editor_blit_str(row++, 0, arr, 220, 220, 220, false);

    /* Help line */
    editor_blit_str(row++, 0,
        "qwe/asd/zxc move  Tab facing  Enter edit  +/- scale  i/j/k/l offset  S save  ESC back",
        120, 120, 120, false);

    /* 3. Thumbnail grid (right side).
     * 3 columns, each cell ~ (right_width/3) wide.
     * Show frame index above each thumbnail.  Highlight the currently
     * selected frame_idx (the one the live sprite is showing). */
    int thumb_left = g_term_cols - 32;
    if (thumb_left < 50) thumb_left = 50;
    int thumb_cols = 3;
    int thumb_cell_w = (g_term_cols - thumb_left) / thumb_cols;
    int thumb_cell_h = 8;   /* 1 label + 1 spacing + up to 6 sprite rows */
    int thumb_top = 0;
    int max_thumbs = thumb_cols * (g_term_rows / thumb_cell_h);
    int total = ss->total_frames;
    if (total > max_thumbs) total = max_thumbs;

    for (int i = 0; i < total; i++) {
        int cx = i % thumb_cols;
        int cy = i / thumb_cols;
        int tl = thumb_left + cx * thumb_cell_w;
        int tt = thumb_top + cy * thumb_cell_h;
        char lab[16];
        snprintf(lab, sizeof(lab), "[%d]", i);
        bool hot = (i == frame_idx);
        editor_blit_str(tt, tl, lab,
                        hot ? 0 : 180,
                        hot ? 255 : 180,
                        hot ? 0 : 180,
                        hot);
        const SpriteFrame* tf = &ss->frames[i];
        if (tf) render_sprite_frame(tf, tl, tt + 1);
    }

    /* 4. Edit string line (bottom). */
    int edit_row = g_term_rows - 2;
    if (edit_active) {
        editor_blit_str(edit_row - 1, 0,
            edit_error
              ? "EDIT (invalid -- fix and press Enter, or Esc to cancel):"
              : "EDIT (Enter to commit, Esc to cancel):",
            edit_error ? 255 : 200,
            edit_error ? 100 : 200,
            edit_error ? 100 : 200,
            false);
        /* Blit the edit string */
        editor_blit_str(edit_row, 0, edit_str,
                        255, 255, 255, false);
        /* Cursor */
        editor_blit_str(edit_row, edit_cursor, "_",
                        255, 255, 255, true);
    } else {
        editor_blit_str(edit_row, 0,
            "Press Enter to edit walk table    S to save to sprites.h",
            100, 100, 100, false);
    }

    flush_text_buffer();
}

/* The walktable-editor main loop.  Returns when user presses ESC. */
static void editor_view_walktable(EntityType etype) {
    g_editor_entity = etype;
    g_editor_active = true;
    editor_init_shadow(etype);

    /* Start the sprite in the middle of the arena, facing S. */
    const SpriteSet* ss = editor_get_sprite_set(etype);
    int16_t wx = SCREEN_TO_FIXED(SCREEN_WIDTH / 2);
    int16_t wy = SCREEN_TO_FIXED(SCREEN_HEIGHT / 2);
    int     facing = DIR_DOWN;

    /* Edit-string state */
    char  edit_str[256];
    int   edit_cursor = 0;
    bool  edit_active = false;
    bool  edit_error  = false;
    edit_str[0] = '\0';

    int16_t dy_step = editor_half_row_step();
    int16_t dx_step = editor_full_col_step();

    for (;;) {
        poll_input();

        /* ESC: leave View 2 (back to selector). */
        if (g_keys[27]) {
            /* Drain any trailing escape-sequence bytes. */
            usleep(20000);
            poll_input();
            g_editor_active = false;
            g_editor_entity = -1;
            return;
        }

        if (edit_active) {
            /* Edit-mode key handling. */
            bool changed = false;
            edit_error = false;
            for (int ch = 32; ch < 127; ch++) {
                if (g_keys[ch]) {
                    if (edit_cursor < (int)sizeof(edit_str) - 1) {
                        edit_str[edit_cursor++] = (char)ch;
                        edit_str[edit_cursor] = '\0';
                        changed = true;
                    }
                }
            }
            if (g_keys[127] || g_keys[8]) {   /* Backspace */
                if (edit_cursor > 0) {
                    edit_str[--edit_cursor] = '\0';
                    changed = true;
                }
            }
            if (g_keys[10]) {   /* Enter -- commit */
                EditorWalk parsed;
                if (editor_parse_walk_string(edit_str, ss->total_frames, &parsed)) {
                    g_editor_walks[etype][facing] = parsed;
                    g_editor_walks[etype][facing].dirty = true;
                    edit_active = false;
                    edit_str[0] = '\0';
                    edit_cursor = 0;
                } else {
                    edit_error = true;
                }
            }
            /* In edit mode, ESC is handled above (returns).  Movement
             * keys are ignored. */
            (void)changed;
        } else {
            /* Normal-mode key handling. */
            EditorWalk* ew_active = &g_editor_walks[etype][facing];

            /* Tab cycles facing N->S->E->W->N. */
            if (g_keys[9]) {
                facing = (facing + 1) % 4;
                ew_active = &g_editor_walks[etype][facing];
            }

            /* Enter opens the edit string with current walk array. */
            if (g_keys[10]) {
                EditorWalk* ew = &g_editor_walks[etype][facing];
                edit_str[0] = '\0';
                int p = 0;
                for (int i = 0; i < ew->count; i++) {
                    if (i > 0) {
                        edit_str[p++] = ',';
                        edit_str[p++] = ' ';
                    }
                    if (p < (int)sizeof(edit_str) - 12) {
                        p += snprintf(edit_str + p,
                                      sizeof(edit_str) - p,
                                      "%d", ew->indices[i]);
                    }
                }
                edit_str[p] = '\0';
                edit_cursor = p;
                edit_active = true;
                edit_error  = false;
            }

            /* +/- tune scale on the active axis (modifies the shadow
             * for the current facing only -- per-facing granularity). */
            if (g_keys['+'] || g_keys['=']) {
                int* p = (facing == DIR_UP || facing == DIR_DOWN)
                       ? &ew_active->scale_y
                       : &ew_active->scale_x;
                if (*p < 32) (*p)++;
                ew_active->dirty = true;
            }
            if (g_keys['-']) {
                int* p = (facing == DIR_UP || facing == DIR_DOWN)
                       ? &ew_active->scale_y
                       : &ew_active->scale_x;
                if (*p > 1) (*p)--;
                ew_active->dirty = true;
            }

            /* S saves to sprites.h. */
            if (g_keys['S']) {
                bool ok = editor_save_sprites_h(etype);
                (void)ok;
            }

            /* i/j/k/l tune the per-frame placement offset (ox, oy) of
             * the currently displayed frame.  Vim-style: i=up, k=down,
             * j=left, l=right.  Affects only the active frame's shadow
             * entry; S writes it back to sprites.h.
             *
             * We need the current frame_idx (which frame the live
             * sprite is showing) so we update the right shadow entry.
             * This mirrors the math in editor_render_walktable(). */
            if (g_keys['i'] || g_keys['j'] || g_keys['k'] || g_keys['l']) {
                const SpriteSet* ss_local = editor_get_sprite_set(etype);
                if (ss_local && ew_active->count > 0) {
                    int step_idx = 0;
                    /* Half-row index computed in one shot from wy
                     * (matches entity_select_frame()).  See the long
                     * comment there for the truncation-dead-zone
                     * rationale; this must stay in sync with the
                     * picker so i/j/k/l always edit the frame the
                     * user actually sees. */
                    int32_t half_rows = (int32_t)wy * ((int32_t)g_term_rows * 2)
                                      / ((int32_t)SCREEN_HEIGHT * COORD_SCALE);
                    if (etype == ENT_GRUNT || etype == ENT_ENFORCER) {
                        step_idx = (int)(half_rows & 1);
                    } else if (etype == ENT_SPHEROID) {
                        /* Spheroid hybrid: bank + sub, mirroring
                         * entity_select_frame()'s ENT_SPHEROID case.
                         * We recompute the same editor_anim_phase
                         * formula here (no Entity* to read from in
                         * the editor key handler -- the live entity
                         * doesn't exist; we're rendering a phantom
                         * sprite at the cursor wx,wy).  Must match
                         * the render path's computation exactly so
                         * i/j/k/l edits target the frame the user
                         * sees, not a stale or off-by-one frame. */
                        int bank = (int)(half_rows & 1);
                        int sub  = (g_frame_count / SPHEROID_WALK_FRAMES_PER_ADVANCE) % 3;
                        if (sub < 0) sub += 3;
                        step_idx = bank * 3 + sub;
                    } else {
                        int base_stride = ss_local->facing[facing].step_period;
                        int scale = (facing == DIR_UP || facing == DIR_DOWN)
                                  ? ew_active->scale_y : ew_active->scale_x;
                        if (base_stride <= 0) base_stride = 1;
                        int axis = (facing == DIR_UP || facing == DIR_DOWN) ? 1 : 0;
                        int16_t pos = axis ? wy : wx;
                        step_idx = stride_cycle(pos, base_stride, scale,
                                                ew_active->count, axis);
                    }
                    if (step_idx < 0 || step_idx >= ew_active->count) step_idx = 0;
                    int fidx = ew_active->indices[step_idx];
                    if (fidx >= 0 && fidx < EDITOR_MAX_FRAMES) {
                        EditorFrameOffset* efo =
                            &g_editor_frame_offsets[etype][fidx];
                        if (g_keys['i']) efo->oy--;   /* up    = oy-- */
                        if (g_keys['k']) efo->oy++;   /* down  = oy++ */
                        if (g_keys['j']) efo->ox--;   /* left  = ox-- */
                        if (g_keys['l']) efo->ox++;   /* right = ox++ */
                        efo->dirty = true;
                    }
                }
            }

            /* Movement keys: qwe / asd / zxc.
             *   q w e    =  up-left, up, up-right
             *   a s d    =  left, (noop), right
             *   z x c    =  down-left, down, down-right
             * Orthogonal keys (w,a,d,x) update facing; diagonals don't. */
            int16_t dx = 0, dy = 0;
            if (g_keys['w']) { dy -= dy_step; facing = DIR_UP; }
            if (g_keys['x']) { dy += dy_step; facing = DIR_DOWN; }
            if (g_keys['a']) { dx -= dx_step; facing = DIR_LEFT; }
            if (g_keys['d']) { dx += dx_step; facing = DIR_RIGHT; }
            if (g_keys['q']) { dx -= dx_step; dy -= dy_step; }
            if (g_keys['e']) { dx += dx_step; dy -= dy_step; }
            if (g_keys['z']) { dx -= dx_step; dy += dy_step; }
            if (g_keys['c']) { dx += dx_step; dy += dy_step; }
            /* 's' = no-op (per spec). */

            wx += dx;
            wy += dy;
            /* Clamp to arena. */
            int16_t margin_x = SCREEN_TO_FIXED(8);
            int16_t margin_y = SCREEN_TO_FIXED(8);
            if (wx < margin_x) wx = margin_x;
            if (wx > SCREEN_TO_FIXED(SCREEN_WIDTH) - margin_x)
                wx = SCREEN_TO_FIXED(SCREEN_WIDTH) - margin_x;
            if (wy < margin_y) wy = margin_y;
            if (wy > SCREEN_TO_FIXED(SCREEN_HEIGHT) - margin_y)
                wy = SCREEN_TO_FIXED(SCREEN_HEIGHT) - margin_y;
        }

        editor_render_walktable(wx, wy, facing,
                                edit_str, edit_cursor,
                                edit_active, edit_error);
        usleep(16000);
    }
}

/* Top-level editor entry point.  Called from main() when invoked as
 * `btk -e`.  Runs until the user quits from the selector view. */
static void editor_mode(void) {
    for (;;) {
        int chosen = editor_view_selector();
        if (chosen < 0) break;
        editor_view_walktable((EntityType)chosen);
    }
}


// ============================================================
//  MAIN
// ============================================================
int main(int argc, char** argv) {
    setlocale(LC_ALL, "en_US.utf8");
    srand((unsigned)time(NULL));

    init_terminal_size();
    init_text_buffer();
    init_text_mode();

    /* `btk -e` -- enter sprite walk-table editor mode (skips game). */
    bool editor_mode_requested = false;
    for (int i = 1; i < argc; i++) {
        if (argv[i] && argv[i][0] == '-' && argv[i][1] == 'e' && argv[i][2] == '\0') {
            editor_mode_requested = true;
        }
    }
    if (editor_mode_requested) {
        editor_mode();
        fini_text_mode();
        /* Reset colors + restore cursor visibility (editor branch
         * returns early, skipping the post-cleanup printf in the
         * normal game path). */
        printf("\x1b[0m\x1b[?25h\n");
        fini_text_buffer();
        return 0;
    }

    play_intro_screen();

    restart_game(1);

    play_zooming_boxes_text();

    // 3. Gameplay Loop
    printf("\x1b[2J\x1b[H");
    int running = 1;
    while (running) {
        poll_input(); // Using your g_keys[] polling
        if (g_keys[27]) running = 0;
        
        update_all(); // <-- No arguments
        render_all(); // <-- No arguments
        if (g_show_game_over) { usleep(500000); break; }
        usleep(16000); 
    }



cleanup:
    // Restore terminal
    fini_text_mode();
    // Reset colors and show cursor (but DO NOT clear the screen)
    printf("\x1b[0m\x1b[?25h");  // Reset colors, show cursor
    
    // Print final score persistently
    printf("\n\n");
    printf("  \x1b[38;2;255;255;0mGAME OVER\x1b[0m\n\n");
    printf("  \x1b[38;2;0;255;0mYOUR SCORE: %07d\x1b[0m\n\n", g_score);
    printf("  Wave reached: %d\n", g_level);
    printf("  Humans rescued: %d\n\n", g_rescue_count);
    
    // Print the final score persistently to stdout
    printf("\n\n  YOUR SCORE: %07d\n\n", g_score);
    
    // Clean up the text buffer
    fini_text_buffer();
    
    return 0;
}
