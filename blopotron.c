// ============================================================
//  Blapotron 2024 terminal robotron-like 1-2p game: ver. 0.41
// 
//  gcc -O2 -Wall robotron40.c -o robotron -lSDL2 -lm
// 
//  ./robotron  or ./robotron -n to start at level n
// ============================================================
/* main()
  ├── SDL_Init + create window/renderer (800×600)
  ├── Parse argv[1] → start_level (clamped 1–40, default 1)
  ├── restart_game(start_level)
  │     └── reset globals, spawn_player(center), g_level=start-1
  │           └── reset_level()
  │                 ├── g_level++, find_safe_spawn() for player position
  │                 ├── Wave lookup: g_waves[(g_level−1) % 40]
  │                 ├── Spawn entities via SPAWN_WITH_SAFETY (up to 50 attempts, is_position_safe)
  │                 └── Spawns grunts, electrodes, hulks, brains, spheroids, quarks, humans(mommy/daddy/mikey)
  ├── play_level_intro(renderer)  // 90 frames of expanding nested colored rectangles
  └── Main loop (60fps target):
        ├── Poll events: SDL_QUIT → exit; ESCAPE → exit; game over key → exit
        ├── update_all()
        │     ├── Early return if g_show_game_over
        │     ├── Handle death_timer / respawn via find_safe_spawn(), set invulnerable + ghost timers
        │     ├── Decrement invulnerable_timer, ghost_timer
        │     └── If !game_over:
        │           ├── update_player(): WASD move (diagonal norm), IJKL shoot (buffer+cooldown)
        │           ├── rebuild_grid()  ← NEW: rebuild spatial grid each frame
        │           ├── update_entities(): walk linked list
        │           │     ├── Hulks: pushback timer special case
        │           │     ├── Grunts: phase-offset tick → ai_grunt()
        │           │     └── Others: normal tick counter dispatch
        │           ├── process_collisions()  ← NOW USES GRID
        │           │     ├── Laser vs Enemy: swept collision → hulk pushback / kill+score
        │           │     ├── Hulk vs Human: kills human
        │           │     ├── Terror vs Player: damage player, set ghost position
        │           │     ├── Player vs Entity: rescue humans (score) or take damage from enemies
        │           │     └── Backward pass removes marked entities via remove_entity()
        │           └── check_level_complete(): if no killable enemies left → play_level_intro + reset_level
        └── render_all()
              ├── Clear black, draw_hud (lives squares, game over overlay)
              ├── Draw player: death shrink animation or normal with invuln flash
              └── Walk linked list: draw each active entity

  Utility helpers used throughout:
    clamp_to_screen(), dist_sq_world(), get_wall_bitmask(),
    fixed_mul(), find_safe_spawn(), spawn_entity()/spawn_player()/spawn_laser()

  Linked-list entity pool + spatial grid (FIXED):
    - g_entities[]: entity pool (MAX_ENTITIES slots)
    - g_next[], g_prev[]: linked list pointers (single source of truth)
    - g_grid_heads[]: per-cell head pointers (GRID_COLS × GRID_ROWS)
    - g_grid_nodes[]: grid node pool (one per entity)
    - g_grid_free: grid node free list head
    - Entity struct has NO next/prev fields — arrays are the truth
*/

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

// ============================================================
//  BLOPOTRON - Text Mode (sprite_bridge) support
// ============================================================

// --- Text Mode Globals ---
//Use `\n` and `\033` in C string literals. They compile to embedded newline and ESC bytes, 
//stay on one physical source line, and your bridge already strips `\n` harmlessly.
/* One physical line. \n marks visual rows. \033 is POSIX C standard. */
//static const char fish_cmd[] =
//    "SPRITE,1,4,3," UL_ON RED "####\n####\n####" UL_OFF RESET "\n";

// ============================================================
//  1. TEXT MODE GLOBALS & HELPERS
// ============================================================

static bool g_text_mode = false;
static FILE* g_text_out = NULL;
static int g_text_cols = 80;
static int g_text_rows = 24;

// ============================================================
//  2. COLOR AND ANSI DEFINITIONS
// ============================================================

#define UL_ON  "\033[4m"
#define UL_OFF "\033[24m"
#define RED    "\033[31m"
#define WHT    "\033[97m"
#define RST    "\033[0m"
#define COLOR_BLACK   "\033[30m"
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_WHITE   "\033[37m"

#define COLOR_BRIGHT_BLACK   "\033[90m"
#define COLOR_BRIGHT_RED     "\033[91m"
#define COLOR_BRIGHT_GREEN   "\033[92m"
#define COLOR_BRIGHT_YELLOW  "\033[93m"
#define COLOR_BRIGHT_BLUE    "\033[94m"
#define COLOR_BRIGHT_MAGENTA "\033[95m"
#define COLOR_BRIGHT_CYAN    "\033[96m"
#define COLOR_BRIGHT_WHITE   "\033[97m"
#define SCORE_COLOR          "\x1b[92m" // lime green
#define TEXT_COLOR           "\x1b[93m" // Bright yellow for instructions

// ============================================================
//  SPRITE DEFINITIONS AND MAPPING
// ============================================================

// Define sprite indices
typedef enum {
    SPRITE_PLAYER = 0,
    SPRITE_GRUNT = 1,
    SPRITE_HUMAN = 2,
    SPRITE_HULK = 3,
    SPRITE_SPHEROID = 4,
    SPRITE_LASER = 5,
    SPRITE_TERROR = 6,
    SPRITE_BRAIN = 7,
    SPRITE_ELECTRODE = 8,
    SPRITE_QUARK = 9,
    SPRITE_ENFORCER = 10,
    SPRITE_CRUISE = 11,
    NUM_SPRITES
} SpriteIndex;


// Digit sprites 500-509 (0-9)
// NOTE: No internal newlines (\n)! The bridge automatically wraps to the 
// next row when the 'cols' limit (3) is reached. Internal newlines would 
// split the command in the line-based stdin reader.
// Digit sprites 500-509 (0-9), 3x3, no internal newlines

static const char* digit_sprites[] = {
    // 0:
    "SPRITE,500,3,3,%s┏━┓┃┃┃┗━┛%s\n",
    "SPRITE,501,3,3,%s╺┓  ┃ ╺┻╸%s\n",
    "SPRITE,502,3,3,%s┏━┓┏━┛┗━╸%s\n",
    "SPRITE,503,3,3,%s┏━┓╺━┫┗━┛%s\n",
    "SPRITE,504,3,3,%s╻ ╻┗━┫  ╹%s\n",
    "SPRITE,505,3,3,%s┏━╸┗━┓┗━┛%s\n",
    "SPRITE,506,3,3,%s┏━┓┣━┓┗━┛%s\n",
    "SPRITE,507,3,3,%s┏━┓  ┃  ╹%s\n",
    "SPRITE,508,3,3,%s┏━┓┣━┫┗━┛%s\n",
    "SPRITE,509,3,3,%s┏━┓┗━┫┗━┛%s\n"
};
static const char* press_1_sprite = "SPRITE,510,32,1,%sPress keyboard 1 to insert coin%s\n";

// Sprite definitions - all enemy entities are 4x2, shots are 1x1
static const char* g_text_sprites =
    // Player (4x2)
    "SPRITE,0,4,2," UL_ON WHT "########" UL_OFF RST "\n"
    // Grunt (6x3)
    "SPRITE,1,6,3," UL_ON RED "######################" UL_OFF RST "\n"
    // Human (2x3) - pink/magenta
    "SPRITE,2,2,3," UL_ON "\x1b[35m||||||" UL_OFF RST "\n"
    // Hulk (4x2) - green
    "SPRITE,3,4,2," UL_ON "\x1b[32mHHHHHHHH" UL_OFF RST "\n"
    // Spheroid (3x2) - orange
    "SPRITE,4,3,2," UL_ON "\x1b[33mOOOOOO" UL_OFF RST "\n"
    // Laser (1x1) - yellow
    "SPRITE,5,1,1," UL_ON "\x1b[33m*" UL_OFF RST "\n"
    // Terror (1x1) - red
    "SPRITE,6,1,1," UL_ON "\x1b[95m*" UL_OFF RST "\n"
    // Brain (4x2) - cyan
    "SPRITE,7,4,2," UL_ON "\x1b[36mBBBBBBBB" UL_OFF RST "\n"
    // Electrode (4x2) - yellow
    "SPRITE,8,2,2," UL_ON "\x1b[33m++++" UL_OFF RST "\n"
    // Quark (4x2) - purple
    "SPRITE,9,4,2," UL_ON "\x1b[35mQQQQQQQQ" UL_OFF RST "\n"
    // Enforcer (4x2) - red
    "SPRITE,10,4,2," UL_ON "\x1b[31mEEEEEEEE" UL_OFF RST "\n"
    // Cruise (1x1) - cyan
    "SPRITE,11,1,1," UL_ON "\x1b[36mC" UL_OFF RST "\n";

// --- Constants ---
#define WALL_LEFT   (1 << 0)
#define WALL_RIGHT  (1 << 1)
#define WALL_TOP    (1 << 2)
#define WALL_BOTTOM (1 << 3)

#define COORD_SCALE 16     // Scaling factor between world representation and screen pixels
#define SCREEN_WIDTH 792   // (132 cols * 6 pix)
#define SCREEN_HEIGHT 600  // (50 rows * 12 pix)
#define WALL_MARGIN 24      // Distance from edge to be considered "near wall" (screen pixels)

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
#define SPHEROID_STATE_WINDUP    1
#define SPHEROID_STATE_SPAWNED   2
#define SPHEROID_WINDUP_TICKS    42   // ~2.4 sec at 15 ticks/sec
#define SPHEROID_SPAWN_PAUSE     10   // ~0.7 sec pause after spawning
#define SPHEROID_CORNER_DIST     78   // px from any corner to trigger spawn mode
#define SPHEROID_DECEL_FACTOR    8    // div 10 slowdown multiply by 3/4 each tick
#define SPHEROID_NUDGE_RANGE     4    // random nudge  per tick
#define SPHEROID_RAMP_TIME       30   // after spawn we prevent wall-drag
#define SPHEROID_STATE_MOVE      0  // Moving, nudging, steering
#define SPHEROID_STATE_PAUSE     1  // Stopped, counting down to launch
#define SPHEROID_REST_TIME       40


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
#define CRUISE_ZAG_FRAMES   6
#define CRUISE_RETARGET_TIM 8

static int16_t g_cruise_body_x[MAX_ENTITIES][CRUISE_BODY_LEN];
static int16_t g_cruise_body_y[MAX_ENTITIES][CRUISE_BODY_LEN];
static int g_cruise_body_len[MAX_ENTITIES];

typedef enum {
    DIR_UP = 0, DIR_DOWN = 1, DIR_LEFT = 2, DIR_RIGHT = 3, DIR_NONE = 4
} Direction;

typedef struct {
    int rwid, rhgt;
    int rwx, rwy;
} RenderWindow;

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

// Map EntityType to sprite index
static int entity_to_sprite(EntityType type) {
    switch(type) {  // Changed from 't' to 'type'
        case ENT_PLAYER:    return SPRITE_PLAYER;
        case ENT_GRUNT:     return SPRITE_GRUNT;
        case ENT_HUMAN:     return SPRITE_HUMAN;
        case ENT_HULK:      return SPRITE_HULK;
        case ENT_SPHEROID:  return SPRITE_SPHEROID;
        case ENT_LASER:     return SPRITE_LASER;
        case ENT_TERROR:    return SPRITE_TERROR;
        case ENT_BRAIN:     return SPRITE_BRAIN;
        case ENT_ELECTRODE: return SPRITE_ELECTRODE;
        case ENT_QUARK:     return SPRITE_QUARK;
        case ENT_ENFORCER:  return SPRITE_ENFORCER;
        case ENT_CRUISE:    return SPRITE_CRUISE;
        default:            return -1;
    }
}

// ENTITY DEF
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
    const SpriteData* sprite;
    bool active;
    bool onscreen;
    // --- Text-mode sprite lifecycle tracking ---
    int  text_sprite_type;      // SPRITE_GRUNT, SPRITE_HUMAN, etc.
    int  text_sprite_instance;  // unique instance ID (= pool idx)
    bool text_sprite_bound;     // true once bridge has a backing store for us
} Entity;

typedef struct {
    int16_t wx, wy;
    int16_t vx, vy;
    int lives;
    bool active;
    EntityType type;
    const SpriteData* sprite;
    int death_timer;
    int invulnerable_timer;
    int16_t ghost_x, ghost_y;
    int ghost_timer;
    int shot_buffer_timer;
    int shot_pending_vx, shot_pending_vy;
    // --- Text-mode sprite lifecycle tracking ---
    int  text_sprite_type;
    int  text_sprite_instance;  // fixed at 0 for the player
    bool text_sprite_bound;
} Player;

typedef struct {
    int grunts, electrodes, hulks, brains, spheroids, quarks, mommies, daddies, mikeys;
} LevelWave;

// ==================================================================================
//  G_WAVES table: Number of entities to spawn at each level.
//  {grunts, electrodes, hulks, brains, spheroids, quarks, mommies, daddies, mikeys}
// ==================================================================================
//    {15,  5,  0,  0, 0, 0, 1, 1, 0},  // 1
static const LevelWave g_waves[40] = {
    { 5,  2,  0,  0, 0, 0, 1, 1, 0},  // 1
    {17, 15,  5,  0, 1, 0, 1, 1, 1},  // 2
    {22, 25,  6,  0, 3, 0, 2, 2, 2},  // 3
    {34, 25,  7,  0, 4, 0, 2, 2, 2},  // 4
    {20, 20,  0, 15, 1, 0,15, 0, 1},  // 5
    {32, 25,  7,  0, 4, 0, 3, 3, 3},  // 6
    { 0,  0, 12,  0, 0,10, 4, 4, 4},  // 7
    {35, 25,  8,  0, 5, 0, 3, 3, 3},  // 8
    {60,  0,  4,  0, 5, 0, 3, 3, 3},  // 9
    {25, 20,  0, 20, 1, 0, 0,22, 0},  // 10
    {35, 25,  8,  0, 5, 0, 3, 3, 3},  // 11
    { 0,  0, 13,  0, 0,12, 3, 3, 3},  // 12
    {35, 25,  8,  0, 5, 0, 3, 3, 3},  // 13
    {27,  5, 20,  0, 2, 0, 5, 5, 5},  // 14
    {25, 20,  2, 20, 1, 0, 0, 0,22},  // 15
    {35, 25,  3,  0, 5, 0, 3, 3, 3},  // 16
    { 0,  0, 14,  0, 0,12, 3, 3, 3},  // 17
    {35, 25,  8,  0, 5, 0, 3, 3, 3},  // 18
    {70,  0,  3,  0, 5, 0, 3, 3, 3},  // 19
    {25, 20,  2, 20, 2, 0, 8, 8, 8},  // 20
    {35, 25,  8,  0, 5, 0, 3, 3, 3},  // 21
    { 0,  0, 15,  0, 0,12, 3, 3, 3},  // 22
    {35, 25,  8,  0, 5, 0, 3, 3, 3},  // 23
    { 0,  0, 13,  0, 6, 7, 3, 3, 3},  // 24
    {25, 20,  1, 21, 1, 0,25, 0, 1},  // 25
    {35, 25,  8,  0, 5, 0, 3, 3, 3},  // 26
    { 0,  0, 16,  0, 0,12, 3, 3, 3},  // 27
    {35, 25,  8,  0, 5, 1, 3, 3, 3},  // 28
    {75,  0,  4,  0, 5, 1, 3, 3, 3},  // 29
    {25, 20,  1, 22, 1, 1, 0,25, 0},  // 30
    {35, 25,  8,  0, 5, 1, 3, 3, 3},  // 31
    { 0,  0, 16,  0, 0,13, 3, 3, 3},  // 32
    {35, 25,  8,  0, 5, 1, 3, 3, 3},  // 33
    {30,  0, 25,  0, 2, 2, 3, 3, 3},  // 34
    {27, 15,  2, 23, 1, 2, 0, 0,25},  // 35
    {35, 25,  8,  0, 5, 2, 3, 3, 3},  // 36
    { 0,  0, 16,  0, 0,14, 3, 3, 3},  // 37
    {35, 25,  8,  0, 5, 2, 3, 3, 3},  // 38
    {80,  0,  6,  0, 5, 1, 3, 3, 3},  // 39
    { 0,  0,  0,  0, 6, 0, 0, 0, 0},  // 40
};

// --- Globals ---
static Entity g_entities[MAX_ENTITIES];
static int16_t g_next[MAX_ENTITIES];
static int16_t g_prev[MAX_ENTITIES];
static int16_t g_list_head;
static int16_t g_list_tail;
static int16_t g_free_head;

static Player g_players[MAX_PLAYERS];
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

// --- Spatial Grid ---
#define GRID_CELL_SIZE 512
#define GRID_COLS 25
#define GRID_ROWS 19

typedef struct {
    int16_t entity_idx;
    int16_t next;
} GridNode;

static GridNode g_grid_nodes[MAX_ENTITIES];
static int16_t g_grid_heads[GRID_COLS * GRID_ROWS];
static int16_t g_grid_free;

// --- Sprite Definitions ---
static const SpriteData sprite_player    = {20, 20, 255, 255, 255, 255, "Player"};
static const SpriteData sprite_human     = {12, 18, 255, 200, 200, 255, "Human"};
static const SpriteData sprite_daddy     = {12, 18, 255, 200, 200, 255, "Daddy"};
static const SpriteData sprite_mommy     = {12, 18, 255, 200, 200, 255, "Mommy"};
static const SpriteData sprite_mikey     = {12, 18, 255, 200, 200, 255, "Mikey"};
static const SpriteData sprite_grunt     = {18, 18, 220,  55,  55, 255, "Grunt"};
static const SpriteData sprite_hulk      = {24, 24,   0, 255,   0, 255, "Hulk"};
static const SpriteData sprite_spheroid  = {18, 18, 255, 127,   0, 255, "Spheroid"};
static const SpriteData sprite_enforcer  = {20, 20, 255,   0,   0, 255, "Enforcer"};
static const SpriteData sprite_quark     = {20, 20, 255,   0, 255, 255, "Quark"};
static const SpriteData sprite_brain     = {22, 22, 255, 255,   0, 255, "Brain"};
static const SpriteData sprite_laser     = { 8,  8, 255, 255,   0, 255, "Laser"};
static const SpriteData sprite_terror    = {10, 10, 255,   0, 255, 255, "Terror"};
static const SpriteData sprite_cruise    = { 4,  4,   0, 235, 255, 255, "Cruise"};
static const SpriteData sprite_electrode = {16, 16, 255, 200,   0, 255, "Electrode"};

// Forward declarations
static Entity* spawn_entity(int16_t x, int16_t y, EntityType type);
static void spawn_player(int16_t x, int16_t y);
static void spawn_laser(int16_t x, int16_t y, int16_t vx, int16_t vy, Direction dir);
static void clamp_to_screen(int16_t* x, int16_t* y, int sprite_w, int sprite_h);
static int32_t dist_sq_world(int16_t x1, int16_t y1, int16_t x2, int16_t y2);
static void play_level_intro(SDL_Renderer* r);
static void play_level_intro_text(void);  
static void render_insert_coin_text(void);

// ============================================================
//  LINKED LIST MANAGEMENT — g_next[]/g_prev[] are the single source of truth
// ============================================================

static void init_lists(void) {
    g_free_head = -1;
    g_list_head = -1;
    g_list_tail = -1;
    // Build free list in reverse so allocation is in order
    for (int i = MAX_ENTITIES - 1; i >= 0; i--) {
        g_entities[i].active = false;
        g_next[i] = g_free_head;
        g_prev[i] = -1;
        g_free_head = i;
    }
    // Init grid free list
    g_grid_free = -1;
    for (int i = MAX_ENTITIES - 1; i >= 0; i--) {
        g_grid_nodes[i].next = g_grid_free;
        g_grid_free = i;
    }
    // Clear grid heads
    for (int i = 0; i < GRID_COLS * GRID_ROWS; i++) {
        g_grid_heads[i] = -1;
    }
}

static int16_t alloc_entity(void) {
    if (g_free_head == -1) return -1;
    int16_t idx = g_free_head;
    g_free_head = g_next[idx];
    return idx;
}

static void free_entity(int16_t idx) {
    Entity* e = &g_entities[idx];

    // --- Text-mode: release the bridge's backing store for this instance ---
    if (g_text_mode && g_text_out && e->text_sprite_bound) {
        fprintf(g_text_out, "ERASE,%d,%d\n",
                e->text_sprite_type, e->text_sprite_instance);
        e->text_sprite_bound = false;
    }

    e->active = false;
    g_next[idx] = g_free_head;
    g_prev[idx] = -1;
    g_free_head = idx;
}

// Unlink entity from global doubly-linked list
static void unlink_entity(int16_t idx) {
    int16_t p = g_prev[idx];
    int16_t n = g_next[idx];
    if (p != -1) g_next[p] = n;
    else g_list_head = n;
    if (n != -1) g_prev[n] = p;
    else g_list_tail = p;
    g_next[idx] = -1;
    g_prev[idx] = -1;
}

// Append entity to tail of global doubly-linked list
static void link_entity(int16_t idx) {
    g_next[idx] = -1;
    g_prev[idx] = g_list_tail;
    if (g_list_tail != -1) g_next[g_list_tail] = idx;
    else g_list_head = idx;
    g_list_tail = idx;
}

// ============================================================
//  SPATIAL GRID
// ============================================================

static int16_t alloc_grid_node(void) {
    if (g_grid_free == -1) return -1;
    int16_t idx = g_grid_free;
    g_grid_free = g_grid_nodes[idx].next;
    return idx;
}

static void free_grid_node(int16_t idx) {
    g_grid_nodes[idx].next = g_grid_free;
    g_grid_free = idx;
}

// Rebuild grid from scratch each frame — NO (uint16_t) cast on coords
static void rebuild_grid(void) {
    // Free all grid nodes
    g_grid_free = -1;
    for (int i = 0; i < MAX_ENTITIES; i++) {
        g_grid_nodes[i].next = g_grid_free;
        g_grid_free = i;
    }
    // Clear all cell heads
    for (int i = 0; i < GRID_COLS * GRID_ROWS; i++) {
        g_grid_heads[i] = -1;
    }
    // Insert all active entities
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
//  HELPER FUNCS
// ============================================================

static void clamp_to_screen(int16_t* x, int16_t* y, int sprite_w, int sprite_h) {
    int16_t max_x = SCREEN_TO_FIXED(SCREEN_WIDTH  - sprite_w);
    int16_t max_y = SCREEN_TO_FIXED(SCREEN_HEIGHT - sprite_h);
    if (*x < 0)     *x = 0;
    if (*y < 0)     *y = 0;
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
    int max_x = SCREEN_TO_FIXED(SCREEN_WIDTH) - SCREEN_TO_FIXED(e->sprite->width);
    int max_y = SCREEN_TO_FIXED(SCREEN_HEIGHT) - SCREEN_TO_FIXED(e->sprite->height);
    int margin = SCREEN_TO_FIXED(WALL_MARGIN);
    if (e->wx <= margin)              mask |= WALL_LEFT;
    if (e->wx >= max_x - margin)      mask |= WALL_RIGHT;
    if (e->wy <= margin)              mask |= WALL_TOP;
    if (e->wy >= max_y - margin)      mask |= WALL_BOTTOM;
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
    int16_t cx = SCREEN_TO_FIXED(SCREEN_WIDTH  / 2);
    int16_t cy = SCREEN_TO_FIXED(SCREEN_HEIGHT / 2);
    bool safe = true;
    for (int16_t idx = g_list_head; idx != -1; idx = g_next[idx]) {
        Entity* e = &g_entities[idx];
        if (!e->active) continue;
        if (e->type == ENT_HUMAN || e->type == ENT_LASER || e->type == ENT_TERROR) continue;
        int32_t d = dist_sq_world(cx, cy, e->wx, e->wy);
        int32_t min_d = RESPAWN_SAFE_DIST;
        if (d < min_d * min_d) {
            safe = false;
            break;
        }
    }
    if (safe) {
        *out_x = cx;
        *out_y = cy;
        return true;
    }
    for (int step = 1; step <= 10; step++) {
        for (int dir = 0; dir < 8; dir++) {
            int16_t tx = cx + (int16_t)(((step * 48) * COORD_SCALE) *
                         ((dir == 0 || dir == 1 || dir == 7) ? 1 :
                          (dir == 3 || dir == 4 || dir == 5) ? -1 : 0));
            int16_t ty = cy + (int16_t)(((step * 48) * COORD_SCALE) *
                         ((dir == 1 || dir == 2 || dir == 3) ? 1 :
                          (dir == 5 || dir == 6 || dir == 7) ? -1 : 0));
            clamp_to_screen(&tx, &ty, sprite_player.width, sprite_player.height);
            safe = true;
            for (int16_t idx = g_list_head; idx != -1; idx = g_next[idx]) {
                Entity* e = &g_entities[idx];
                if (!e->active) continue;
                if (e->type == ENT_HUMAN || e->type == ENT_LASER || e->type == ENT_TERROR) continue;
                int32_t d = dist_sq_world(tx, ty, e->wx, e->wy);
                int32_t min_d = RESPAWN_SAFE_DIST;
                if (d < min_d * min_d) {
                    safe = false;
                    break;
                }
            }
            if (safe) {
                *out_x = tx;
                *out_y = ty;
                return true;
            }
        }
    }
    *out_x = cx;
    *out_y = cy;
    return false;
}

static void remove_entity(int16_t idx) {
    if (idx < 0) return;
    unlink_entity(idx);
    free_entity(idx);
}

static int get_dir_from_vel(int16_t vx, int16_t vy) {
    if (abs(vx) > abs(vy)) {
        return (vx > 0) ? DIR_RIGHT : DIR_LEFT;
    }
    return (vy > 0) ? DIR_DOWN : DIR_UP;
}

static int16_t fixed_mul(int16_t a, int16_t b) {
    int32_t result = (int32_t)a * (int32_t)b;
    return (int16_t)(result / 16384);
}

static bool check_swept_collision(Entity* laser, Entity* enemy) {
    int16_t e_min_x = enemy->wx;
    int16_t e_max_x = enemy->wx + SCREEN_TO_FIXED(enemy->sprite->width);
    int16_t e_min_y = enemy->wy;
    int16_t e_max_y = enemy->wy + SCREEN_TO_FIXED(enemy->sprite->height);
    int steps = 8;
    int16_t dx = laser->vx;
    int16_t dy = laser->vy;
    for (int i = 0; i <= steps; i++) {
        int16_t cx = laser->wx + (dx * i) / steps;
        int16_t cy = laser->wy + (dy * i) / steps;
        if (cx >= e_min_x && cx <= e_max_x && cy >= e_min_y && cy <= e_max_y) {
            return true;
        }
    }
    return false;
}

static bool check_player_enemy_collision(Player* p, Entity* e) {
    int16_t p_min_x = p->wx;
    int16_t p_max_x = p->wx + SCREEN_TO_FIXED(p->sprite->width);
    int16_t p_min_y = p->wy;
    int16_t p_max_y = p->wy + SCREEN_TO_FIXED(p->sprite->height);
    int16_t e_min_x = e->wx;
    int16_t e_max_x = e->wx + SCREEN_TO_FIXED(e->sprite->width);
    int16_t e_min_y = e->wy;
    int16_t e_max_y = e->wy + SCREEN_TO_FIXED(e->sprite->height);
    return (p_min_x < e_max_x && p_max_x > e_min_x &&
            p_min_y < e_max_y && p_max_y > e_min_y);
}

static bool check_entity_entity_collision(Entity* a, Entity* b) {
    int16_t a_min_x = a->wx;
    int16_t a_max_x = a->wx + SCREEN_TO_FIXED(a->sprite->width);
    int16_t a_min_y = a->wy;
    int16_t a_max_y = a->wy + SCREEN_TO_FIXED(a->sprite->height);
    int16_t b_min_x = b->wx;
    int16_t b_max_x = b->wx + SCREEN_TO_FIXED(b->sprite->width);
    int16_t b_min_y = b->wy;
    int16_t b_max_y = b->wy + SCREEN_TO_FIXED(b->sprite->height);
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
        } else {
            e->vx = 0;
            e->vy = 0;
        }
        e->attitude_counter = e->attitude_period + (rand() % e->attitude_period/4) - e->attitude_period/8;
    }
}

static void ai_grunt(Entity* e) {
    if (e->target_idx < 0 || e->target_idx >= g_player_count) {
        e->target_idx = 0;
    }
    Player* p = &g_players[e->target_idx];
    if (!p->active) return;

    int16_t target_x, target_y;
    if (p->ghost_timer > 0) {
        target_x = p->ghost_x;
        target_y = p->ghost_y;
    } else {
        target_x = p->wx;
        target_y = p->wy;
    }
    int16_t dx = target_x - e->wx;
    int16_t dy = target_y - e->wy;
    int16_t step = GRUNT_SPEED;
    int16_t ndx = dx + (rand() % 32) - 16;
    int16_t ndy = dy + (rand() % 32) - 16;
    e->vx = (ndx > 0) ? step : (ndx < 0) ? -step : 0;
    e->vy = (ndy > 0) ? step : (ndy < 0) ? -step : 0;
    if (rand() % 100 < 5) {
        int dir = rand() % 4;
        e->vx = (dir == DIR_LEFT) ? -step : (dir == DIR_RIGHT) ? step : 0;
        e->vy = (dir == DIR_UP) ? -step : (dir == DIR_DOWN) ? step : 0;
    }

    // Probabilistic separation from other grunts (use grid)
    if (rand() % 100 < GRUNT_SHYNESS) {
        int16_t shy_x = 0, shy_y = 0;
        int cell_x = (int)e->wx / GRID_CELL_SIZE;
        int cell_y = (int)e->wy / GRID_CELL_SIZE;
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
                    if (sd < SCREEN_TO_FIXED(60) && sd > 0) {
                        shy_x += (sdx * COORD_SCALE) / sd;
                        shy_y += (sdy * COORD_SCALE) / sd;
                    }
                }
            }
        }
        e->vx += shy_x;
        e->vy += shy_y;
    }
}

static void ai_hulk(Entity* e) {
    // --- Validate current target ---
    if (e->target_entity >= 0) {
        Entity* target = &g_entities[e->target_entity];
        if (!target->active || target->type != ENT_HUMAN) {
            e->target_entity = -1;
        }
    }

    // --- Chase mode: walk toward human, retarget on timer ---
    if (e->target_entity >= 0) {
        Entity* target = &g_entities[e->target_entity];

        // Tick down the retarget timer
        if (e->target_counter > 0) {
            e->target_counter--;
            return;  // keep walking current direction
        }

        // Timer expired — recalculate direction toward human
        int16_t dx = target->wx - e->wx;
        int16_t dy = target->wy - e->wy;
        if (abs(dx) > abs(dy)) {
            e->vx = (dx > 0) ? HULK_SPEED : -HULK_SPEED;
            e->vy = 0;
        } else {
            e->vx = 0;
            e->vy = (dy > 0) ? HULK_SPEED : -HULK_SPEED;
        }
        e->target_counter = e->target_period;
        return;
    }

    // --- Wander mode: random NSEW, attitude timer ---
    e->attitude_counter--;
    if (e->attitude_counter <= 0) {
        int dir = rand() % 4;
        int16_t spd = HULK_SPEED;
        e->vx = (dir == DIR_LEFT) ? -spd : (dir == DIR_RIGHT) ? spd : 0;
        e->vy = (dir == DIR_UP)   ? -spd : (dir == DIR_DOWN)  ? spd : 0;
        e->attitude_counter = e->attitude_period
                            + (rand() % (e->attitude_period / 4))
                            - (e->attitude_period / 8);
    }

    // --- Scan 7×7 grid cells for humans ---
    // | Detection range | Scan area | Distance threshold |
    // |----------------|-----------|-------------------|
    // | ±3 cells | 7×7 = 49 cells    | 192 pixels |
    // | ±4 cells | 9×9 = 81 cells    | 256 pixels |
    // | ±5 cells | 11×11 = 121 cells | 320 pixels |
    // | ±6 cells | 13×13 = 169 cells | 384 pixels |

    int cell_x = (int)e->wx / GRID_CELL_SIZE;
    int cell_y = (int)e->wy / GRID_CELL_SIZE;
    for (int dy2 = -3; dy2 <= 3; dy2++) {
        for (int dx2 = -3; dx2 <= 3; dx2++) {
            int cx = cell_x + dx2;
            int cy = cell_y + dy2;
            if (cx < 0 || cx >= GRID_COLS || cy < 0 || cy >= GRID_ROWS) continue;
            int cell = cy * GRID_COLS + cx;
            for (int nidx = g_grid_heads[cell]; nidx != -1; nidx = g_grid_nodes[nidx].next) {
                Entity* other = &g_entities[g_grid_nodes[nidx].entity_idx];
                if (!other->active || other->type != ENT_HUMAN) continue;
                int32_t dist = abs((int32_t)other->wx - (int32_t)e->wx)
                              + abs((int32_t)other->wy - (int32_t)e->wy);
                if (dist < SCREEN_TO_FIXED(192)) { // manhattan distance threshold to begin chase
                    e->target_entity = g_grid_nodes[nidx].entity_idx;
                    e->target_counter = e->target_period;
                    // Set initial chase direction
                    int16_t dx = other->wx - e->wx;
                    int16_t dy = other->wy - e->wy;
                    if (abs(dx) > abs(dy)) {
                        e->vx = (dx > 0) ? HULK_SPEED : -HULK_SPEED;
                        e->vy = 0;
                    } else {
                        e->vx = 0;
                        e->vy = (dy > 0) ? HULK_SPEED : -HULK_SPEED;
                    }
                    return;
                }
            }
        }
    }
}

static void ai_hulk1(Entity* e) {
    // --- Check if current target is still alive ---
    if (e->target_entity >= 0) {
        Entity* target = &g_entities[e->target_entity];
        if (!target->active || target->type != ENT_HUMAN) {
            e->target_entity = -1;   // target gone, resume wandering
        }
    }

    // --- Chase mode: follow target human ---
    if (e->target_entity >= 0) {
        Entity* target = &g_entities[e->target_entity];
        int16_t dx = target->wx - e->wx;
        int16_t dy = target->wy - e->wy;

        // Rectilinear movement: pick dominant axis
        if (abs(dx) > abs(dy)) {
            e->vx = (dx > 0) ? HULK_SPEED : -HULK_SPEED;
            e->vy = 0;
        } else {
            e->vx = 0;
            e->vy = (dy > 0) ? HULK_SPEED : -HULK_SPEED;
        }
        return;
    }

    // --- Wander mode: timer-based direction changes ---
    e->attitude_counter--;
    if (e->attitude_counter <= 0) {
        int dir = rand() % 4;
        int16_t spd = HULK_SPEED;
        e->vx = (dir == DIR_LEFT) ? -spd : (dir == DIR_RIGHT) ? spd : 0;
        e->vy = (dir == DIR_UP)   ? -spd : (dir == DIR_DOWN)  ? spd : 0;
        e->attitude_counter = e->attitude_period
                            + (rand() % (e->attitude_period / 4))
                            - (e->attitude_period / 8);
    }

    // --- Scan nearby cells for humans ---
    int cell_x = (int)e->wx / GRID_CELL_SIZE;
    int cell_y = (int)e->wy / GRID_CELL_SIZE;
    for (int dy2 = -1; dy2 <= 1; dy2++) {
        for (int dx2 = -1; dx2 <= 1; dx2++) {
            int cx = cell_x + dx2;
            int cy = cell_y + dy2;
            if (cx < 0 || cx >= GRID_COLS || cy < 0 || cy >= GRID_ROWS) continue;
            int cell = cy * GRID_COLS + cx;
            for (int nidx = g_grid_heads[cell]; nidx != -1; nidx = g_grid_nodes[nidx].next) {
                Entity* other = &g_entities[g_grid_nodes[nidx].entity_idx];
                if (!other->active || other->type != ENT_HUMAN) continue;
                int32_t dist = abs((int32_t)other->wx - (int32_t)e->wx)
                              + abs((int32_t)other->wy - (int32_t)e->wy);
                if (dist < SCREEN_TO_FIXED(128)) {
                    e->target_entity = g_grid_nodes[nidx].entity_idx;
                    return;
                }
            }
        }
    }
}

static void ai_quark(Entity* e) {
    int mask = get_wall_bitmask(e);
    {
        int32_t margin = SCREEN_TO_FIXED(100);
        if (mask & WALL_LEFT) {
            int32_t dist = e->wx;
            if (dist < margin) {
                e->vx += (int16_t)((margin - dist) * COORD_SCALE / margin)*10;
            }
        }
        if (mask & WALL_RIGHT) {
            int32_t dist = SCREEN_TO_FIXED(SCREEN_WIDTH) - e->wx;
            if (dist < margin) {
                e->vx -= (int16_t)((margin - dist) * COORD_SCALE / margin)*10;
            }
        }
        if (mask & WALL_TOP) {
            int32_t dist = e->wy;
            if (dist < margin) {
                e->vy += (int16_t)((margin - dist) * COORD_SCALE / margin)*10;
            }
        }
        if (mask & WALL_BOTTOM) {
            int32_t dist = SCREEN_TO_FIXED(SCREEN_HEIGHT) - e->wy;
            if (dist < margin) {
                e->vy -= (int16_t)((margin - dist) * COORD_SCALE / margin)*10;
            }
        }
    }
    int16_t max_speed = QUARK_SPEED * COORD_SCALE;
    if (abs(e->vx) > max_speed) e->vx = (e->vx > 0) ? max_speed : -max_speed;
    if (abs(e->vy) > max_speed) e->vy = (e->vy > 0) ? max_speed : -max_speed;
}

// Spheroid has three states 0: move, 1: windup to spawn, 2: spawned (wait and cycle to move)
static void ai_spheroid(Entity* e) {
    // ─── PAUSE STATE (Resting/Stopped) ───
    if (e->state == SPHEROID_STATE_PAUSE) {
        e->target_counter--;
        if (e->target_counter <= 0) {
            // ─── LAUNCH SEQUENCE ───
            e->state = SPHEROID_STATE_MOVE;
            e->fire_counter = 0;

            int mask = get_wall_bitmask(e);
            int16_t spd = SPHEROID_SPEED * COORD_SCALE;

            // Arbitrary direction vector
            int16_t dx = (rand() % 2000) - 1000;
            int16_t dy = (rand() % 2000) - 1000;

            // Clamp signs to point away from screen center (guarantees outward)
            int16_t hw = SCREEN_TO_FIXED(SCREEN_WIDTH / 2);
            int16_t hh = SCREEN_TO_FIXED(SCREEN_HEIGHT / 2);
            if (e->wx < hw) dx = abs(dx); else dx = -abs(dx);
            if (e->wy < hh) dy = abs(dy); else dy = -abs(dy);

            // Normalize to speed (preserves exact angle)
            int16_t mag = (abs(dx) > abs(dy)) ? abs(dx) : abs(dy);
            if (mag == 0) { dx = 500; dy = 800; mag = 800; }
            e->vx = (dx * spd) / mag;
            e->vy = (dy * spd) / mag;

            // LOG EVERY LAUNCH + ERROR FLAG
            bool error = false;
            if ((mask & WALL_LEFT) && e->vx <= 0) error = true;
            if ((mask & WALL_RIGHT) && e->vx >= 0) error = true;
            if ((mask & WALL_TOP) && e->vy <= 0) error = true;
            if ((mask & WALL_BOTTOM) && e->vy >= 0) error = true;

            //fprintf(stderr, "%sLAUNCH id=%d wx=%d wy=%d vx=%d vy=%d wall=%d\n",
            //        error ? "ERROR: " : "",
            //        (int)(e - g_entities), e->wx, e->wy, e->vx, e->vy, mask);
        }
        return;
    }

    // ─── MOVE STATE ───
    if (e->vx == 0 && e->vy == 0) {
        int16_t spd = SPHEROID_SPEED * COORD_SCALE / 2;
        e->vx = (rand() % 2) ? spd : -spd;
        e->vy = (rand() % 2) ? spd : -spd;
        e->fire_counter = 0;
    }

    if (e->fire_counter < SPHEROID_RAMP_TIME) {
        e->vx = (e->vx * 11) / 10;
        e->vy = (e->vy * 11) / 10;
        e->fire_counter++;
    } else {
        e->vx += (rand() % (SPHEROID_NUDGE_RANGE * 2 + 1)) - SPHEROID_NUDGE_RANGE;
        e->vy += (rand() % (SPHEROID_NUDGE_RANGE * 2 + 1)) - SPHEROID_NUDGE_RANGE;

        int16_t wall_x_min = SCREEN_TO_FIXED(SCREEN_WIDTH  * 12 / 100);
        int16_t wall_x_max = SCREEN_TO_FIXED(SCREEN_WIDTH  * 88 / 100);
        int16_t wall_y_min = SCREEN_TO_FIXED(SCREEN_HEIGHT * 12 / 100);
        int16_t wall_y_max = SCREEN_TO_FIXED(SCREEN_HEIGHT * 88 / 100);

        if (e->wx < wall_x_min || e->wx > wall_x_max) {
            e->vy = (e->vy * 10) / 9;
            e->vx = (e->vx * 8) / 10;
        }
        if (e->wy < wall_y_min || e->wy > wall_y_max) {
            e->vx = (e->vx * 10) / 9;
            e->vy = (e->vy * 8) / 10;
        }
    }

    int16_t max_speed = SPHEROID_SPEED * COORD_SCALE;
    int32_t mag = (int32_t)sqrt((double)e->vx * e->vx + (double)e->vy * e->vy);
    if (mag > max_speed && mag > 0) {
        e->vx = (e->vx * max_speed) / mag;
        e->vy = (e->vy * max_speed) / mag;
    }

    // Corner detection
    int cd = SCREEN_TO_FIXED(SPHEROID_CORNER_DIST);
    bool in_corner = (e->wx < cd && e->wy < cd) ||
                     (e->wx > SCREEN_TO_FIXED(SCREEN_WIDTH) - cd && e->wy < cd) ||
                     (e->wx < cd && e->wy > SCREEN_TO_FIXED(SCREEN_HEIGHT) - cd) ||
                     (e->wx > SCREEN_TO_FIXED(SCREEN_WIDTH) - cd && e->wy > SCREEN_TO_FIXED(SCREEN_HEIGHT) - cd);

    if (in_corner) {
        // SOFT STOP: Decelerate gradually, don't kill velocity instantly
        e->vx = (e->vx * 9) / 10;
        e->vy = (e->vy * 9) / 10;

        // Check if velocity is effectively zero
        if (abs(e->vx) < 2 && abs(e->vy) < 2) {
            e->vx = 0;
            e->vy = 0;

            // Spawn enforcers if capacity remains
            if (e->spawn_count < e->spawn_max) {
                int count = 1 + (rand() % 4);
                for (int i = 0; i < count && e->spawn_count < e->spawn_max; i++) {
                    Entity* enf = spawn_entity(e->wx, e->wy, ENT_ENFORCER);
                    if (enf) e->spawn_count++;
                }
            }

            // Transition to REST/PAUSE state
            e->state = SPHEROID_STATE_PAUSE;
            e->target_counter = SPHEROID_REST_TIME;
        }
    }
}

static void ai_enforcer(Entity* e) {
    Player* p = &g_players[0];
    if (!p->active) return;

    // Move toward player slowly with slight orbital drift
    int16_t dx = p->wx - e->wx;
    int16_t dy = p->wy - e->wy;
    int32_t dist = abs(dx) + abs(dy);
    if (dist == 0) dist = 1;

    // Perpendicular drift (orbital bias)
    int16_t perp_x = -dy;
    int16_t perp_y = dx;
    int32_t perp_dist = abs(perp_x) + abs(perp_y);
    if (perp_dist == 0) perp_dist = 1;
    perp_x = (perp_x * 256) / perp_dist;
    perp_y = (perp_y * 256) / perp_dist;

    int16_t nudge = COORD_SCALE;
    e->vx += (perp_x * nudge) / 256;
    e->vy += (perp_y * nudge) / 256;

    // Clamp speed
    int16_t max_speed = ENFORCER_SPEED * COORD_SCALE;
    if (abs(e->vx) > max_speed) e->vx = (e->vx > 0) ? max_speed : -max_speed;
    if (abs(e->vy) > max_speed) e->vy = (e->vy > 0) ? max_speed : -max_speed;

    // Fire terror projectiles
    e->fire_counter--;
    if (e->fire_counter <= 0) {
        int count = 1 + (rand() % 4);
        for (int i = 0; i < count; i++) {
            Entity* terror = spawn_entity(e->wx, e->wy, ENT_TERROR);
            if (terror) {
                int16_t target_x = (p->ghost_timer > 0) ? p->ghost_x : p->wx;
                int16_t target_y = (p->ghost_timer > 0) ? p->ghost_y : p->wy;
                int16_t tdx = target_x - terror->wx;
                int16_t tdy = target_y - terror->wy;
                int16_t tdist = abs(tdx) + abs(tdy);
                int16_t speed = 6 * COORD_SCALE;
                if (tdist == 0) tdist = 1;
                terror->vx = (tdx * speed) / tdist + (rand() % 7 - 3);
                terror->vy = (tdy * speed) / tdist + (rand() % 7 - 3);
            }
        }
        e->fire_counter = e->fire_period + (rand() % 30);
    }

    // Separation from other enforcers (use grid)
    int cell_x = (int)e->wx / GRID_CELL_SIZE;
    int cell_y = (int)e->wy / GRID_CELL_SIZE;
    for (int dy2 = -1; dy2 <= 1; dy2++) {
        for (int dx2 = -1; dx2 <= 1; dx2++) {
            int cx = cell_x + dx2, cy = cell_y + dy2;
            if (cx < 0 || cx >= GRID_COLS || cy < 0 || cy >= GRID_ROWS) continue;
            int cell = cy * GRID_COLS + cx;
            for (int nidx = g_grid_heads[cell]; nidx != -1; nidx = g_grid_nodes[nidx].next) {
                Entity* other = &g_entities[g_grid_nodes[nidx].entity_idx];
                if (other == e || !other->active || other->type != ENT_ENFORCER) continue;
                int32_t sdx = (int32_t)e->wx - (int32_t)other->wx;
                int32_t sdy = (int32_t)e->wy - (int32_t)other->wy;
                int32_t sd = abs(sdx) + abs(sdy);
                if (sd < SCREEN_TO_FIXED(80) && sd > 0) {
                    int16_t repel = COORD_SCALE / 2;
                    e->vx += (sdx * repel) / sd;
                    e->vy += (sdy * repel) / sd;
                }
            }
        }
    }
}



// ============================================================
// --- Spawning ---
// ============================================================
static Entity* spawn_entity(int16_t wx, int16_t wy, EntityType type) {
    int16_t idx = alloc_entity();
    if (idx == -1) {
        fprintf(stderr, "BLOCKED: entity pool full\n");
        return NULL;
    }
    Entity* e = &g_entities[idx];
    // Default values — ensure no uninitialized variables
    e->wx = wx;
    e->wy = wy;
    e->tx = 0;
    e->ty = 0;
    e->vx = 0;
    e->vy = 0;
    e->target_idx = -1;
    e->target_period = 0;
    e->target_counter = 0;
    e->type = type;
    e->tick_period = 1;
    e->tick_counter = 0;
    e->tick_phase = 0;
    e->active = true;
    e->attitude_counter = 0;
    e->anim_frame = 0;
    e->facing_dir = DIR_DOWN;
    e->move_dir = DIR_DOWN;
    e->age = 0;
    e->pushback_timer = 0;
    e->human_type = 0;
    e->spawn_count = 0;
    e->fire_phase = 0;
    e->fire_phase = 0;
    // --- Text-mode sprite lifecycle init ---
    e->text_sprite_type      = -1;
    e->text_sprite_instance  = idx;   // pool index IS the instance ID
    e->text_sprite_bound     = false; // becomes true on first DRAW

    switch (type) {
        case ENT_HUMAN:
            e->sprite = &sprite_human;
            e->tick_period = 2;
            e->attitude_period = 128;
            e->attitude_counter = 128;
            e->human_type = rand() % 3;
            break;
        case ENT_GRUNT:
            e->sprite = &sprite_grunt;
            e->tick_period = 23;
            e->tick_phase = rand() % e->tick_period;
            break;
        case ENT_HULK:
            e->sprite = &sprite_hulk;
            e->tick_period = 4;
            e->attitude_period = 45;    // wander: ~3 sec between direction changes
            e->attitude_counter = rand() % e->attitude_period;
            e->target_entity = -1;
            e->target_period = 8;       // chase: 0.5 sec between retargets (8 ticks at ~15/sec)
            e->target_counter = 0;
            break;
        case ENT_SPHEROID:
            e->sprite = &sprite_spheroid;
            e->tick_period = 2;
            e->attitude_period = 4;
            e->attitude_counter = rand() % 4;
            e->fire_period = 128;
            e->fire_counter = 128;
            e->spawn_count = 0;
            e->spawn_max = 4;
            e->state = SPHEROID_STATE_MOVE;
            e->target_counter = 0;
            e->target_period = SPHEROID_WINDUP_TICKS;
            {
                int corner = rand() % 4;
                switch (corner) {
                    case 0: e->mtgx = 0;                        e->mtgy = 0;                        break;
                    case 1: e->mtgx = SCREEN_TO_FIXED(SCREEN_WIDTH);  e->mtgy = 0;                        break;
                    case 2: e->mtgx = 0;                        e->mtgy = SCREEN_TO_FIXED(SCREEN_HEIGHT); break;
                    case 3: e->mtgx = SCREEN_TO_FIXED(SCREEN_WIDTH);  e->mtgy = SCREEN_TO_FIXED(SCREEN_HEIGHT); break;
                }
                int16_t dx = e->mtgx - e->wx;
                int16_t dy = e->mtgy - e->wy;
                int32_t dist = abs(dx) + abs(dy);
                if (dist == 0) dist = 1;
                int16_t sspd = SPHEROID_SPEED * COORD_SCALE;
                e->vx = (int16_t)(((int32_t)dx * sspd) / dist);
                e->vy = (int16_t)(((int32_t)dy * sspd) / dist);
            }
            break;
        case ENT_ENFORCER:
            e->sprite = &sprite_enforcer;
            e->attitude_period  = 20;
            e->attitude_counter = 20;
            e->attitude_phase = (rand() % 20);
            e->fire_period  = 40;
            e->fire_counter = 40;
            e->fire_phase = (rand() % 40);
            e->tick_period = 2;
            e->tick_phase = 0;
            {
                int mask = get_wall_bitmask(e);
                int16_t speed = ENFORCER_SPEED * COORD_SCALE;
                e->vx = speed + rand() % speed * (((mask & WALL_LEFT) != 0) - ((mask & WALL_RIGHT) != 0));
                e->vy = speed + rand() % speed * (((mask & WALL_TOP) != 0) - ((mask & WALL_BOTTOM) != 0));
            }
            break;
        case ENT_BRAIN:
            e->sprite = &sprite_brain;
            e->tick_period = 4;
            e->tick_phase = rand() % e->tick_period;
            e->tick_counter = e->tick_period;
            e->attitude_period = 32;
            e->attitude_phase = rand() % e->attitude_period;
            e->attitude_counter = e->attitude_period;
            e->fire_period = 32 + (rand() % 32);
            e->fire_phase = rand() % e->fire_period;
            break;
        case ENT_CRUISE:
            e->sprite = &sprite_cruise;
            e->tick_period = 1;
            e->attitude_counter = 20;
            break;
        case ENT_TERROR:
            e->sprite = &sprite_terror;
            e->tick_period = 1;
            break;
        case ENT_LASER:
            e->sprite = &sprite_laser;
            e->tick_period = 1;
            break;
        case ENT_ELECTRODE:
            e->sprite = &sprite_electrode;
            e->tick_period = 9999999;
            break;
        case ENT_QUARK:
            e->sprite = &sprite_quark;
            e->tick_period = 4;
            break;
        default:
            e->active = false;
            free_entity(idx);
            return NULL;
    }

    // Insert into linked list
    link_entity(idx);
    return e;
}

static void spawn_player(int16_t wx, int16_t wy) {
    if (g_player_count >= MAX_PLAYERS) return;
    Player* p = &g_players[g_player_count++];
    p->wx = wx;
    p->wy = wy;
    p->vx = 0;
    p->vy = 0;
    p->type = ENT_PLAYER;
    p->sprite = &sprite_player;
    p->active = true;
    p->lives = PLAYER_LIVES;
    p->death_timer = 0;
    p->invulnerable_timer = 0;
    p->ghost_x = 0;
    p->ghost_y = 0;
    p->ghost_timer = 0;
    p->shot_buffer_timer = 0;
    p->shot_pending_vx = 0;
    p->shot_pending_vy = 0;
    p->shot_pending_vx = 0;
    p->shot_pending_vy = 0;
    // --- Text-mode sprite lifecycle init ---
    p->text_sprite_type      = -1;
    p->text_sprite_instance  = 0;     // player is always instance 0
    p->text_sprite_bound     = false;
}

static void spawn_laser(int16_t x, int16_t y, int16_t vx, int16_t vy, Direction dir) {
    (void)dir;
    if (g_laser_count >= MAX_LASERS) return;
    Entity* l = spawn_entity(x, y, ENT_LASER);
    if (!l) return;
    l->vx = vx;
    l->vy = vy;
    g_laser_count++;
}

// ============================================================
// --- Level Management ---
// ============================================================

static void reset_level(void) {
    // Unlink and free all entities
    while (g_list_head != -1) {
        int16_t idx = g_list_head;
        unlink_entity(idx);
        free_entity(idx);
    }
    g_laser_count = 0;

    int16_t spawn_x, spawn_y;
    find_safe_spawn(&spawn_x, &spawn_y);
    g_players[0].wx = spawn_x;
    g_players[0].wy = spawn_y;
    g_players[0].vx = 0;
    g_players[0].vy = 0;
    g_players[0].death_timer = 0;
    g_level++;

    int wave_idx = (g_level - 1) % 40;
    const LevelWave* w = &g_waves[wave_idx];

    #define SPAWN_WITH_SAFETY(count, type) \
        for (int _i = 0; _i < (count); _i++) { \
            int _attempts = 0; \
            int16_t _x, _y; \
            do { \
                _x = rand() % SCREEN_TO_FIXED(SCREEN_WIDTH); \
                _y = rand() % SCREEN_TO_FIXED(SCREEN_HEIGHT); \
                _attempts++; \
                if (_attempts > 50) break; \
            } while (!is_position_safe(_x, _y)); \
            spawn_entity(_x, _y, (type)); \
        }

    SPAWN_WITH_SAFETY(w->grunts, ENT_GRUNT)
    SPAWN_WITH_SAFETY(w->electrodes, ENT_ELECTRODE)
    SPAWN_WITH_SAFETY(w->hulks, ENT_HULK)
    SPAWN_WITH_SAFETY(w->brains, ENT_BRAIN)
    SPAWN_WITH_SAFETY(w->spheroids, ENT_SPHEROID)
    SPAWN_WITH_SAFETY(w->quarks, ENT_QUARK)
    #undef SPAWN_WITH_SAFETY

    // Spawn humans by type
    for (int i = 0; i < w->mommies; i++) {
        int attempts = 0;
        int16_t x, y;
        do {
            x = rand() % SCREEN_TO_FIXED(SCREEN_WIDTH);
            y = rand() % SCREEN_TO_FIXED(SCREEN_HEIGHT);
            attempts++;
            if (attempts > 50) break;
        } while (!is_position_safe(x, y));
        Entity* h = spawn_entity(x, y, ENT_HUMAN);
        if (h) h->human_type = 0;
    }
    for (int i = 0; i < w->daddies; i++) {
        int attempts = 0;
        int16_t x, y;
        do {
            x = rand() % SCREEN_TO_FIXED(SCREEN_WIDTH);
            y = rand() % SCREEN_TO_FIXED(SCREEN_HEIGHT);
            attempts++;
            if (attempts > 50) break;
        } while (!is_position_safe(x, y));
        Entity* h = spawn_entity(x, y, ENT_HUMAN);
        if (h) h->human_type = 1;
    }
    for (int i = 0; i < w->mikeys; i++) {
        int attempts = 0;
        int16_t x, y;
        do {
            x = rand() % SCREEN_TO_FIXED(SCREEN_WIDTH);
            y = rand() % SCREEN_TO_FIXED(SCREEN_HEIGHT);
            attempts++;
            if (attempts > 50) break;
        } while (!is_position_safe(x, y));
        Entity* h = spawn_entity(x, y, ENT_HUMAN);
        if (h) h->human_type = 2;
    }
}

static void restart_game(int start_level) {
    init_lists();
    g_laser_count = 0;
    g_player_count = 0;
    g_level = start_level - 1;
    g_game_over = false;
    g_show_game_over = false;
    g_frame_count = 0;
    g_score = 0;
    g_rescue_count = 0;
    spawn_player(SCREEN_TO_FIXED(SCREEN_WIDTH / 2), SCREEN_TO_FIXED(SCREEN_HEIGHT / 2));
    reset_level();
}

static void draw_welcome(SDL_Renderer* r) {
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);

    int box_w = 60, box_h = 60, gap = 20;
    int start_x = (SCREEN_WIDTH - (3 * box_w + 2 * gap)) / 2;
    int y = (SCREEN_HEIGHT - box_h) / 2;
    SDL_Rect rect = { 0, y, box_w, box_h };

    rect.x = start_x;
    SDL_SetRenderDrawColor(r, 255, 60, 60, 255);
    SDL_RenderFillRect(r, &rect);
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    SDL_RenderDrawRect(r, &rect);

    rect.x = start_x + box_w + gap;
    SDL_SetRenderDrawColor(r, 60, 255, 60, 255);
    SDL_RenderFillRect(r, &rect);
    SDL_RenderDrawRect(r, &rect);

    rect.x = start_x + 2 * (box_w + gap);
    SDL_SetRenderDrawColor(r, 60, 60, 255, 255);
    SDL_RenderFillRect(r, &rect);
    SDL_RenderDrawRect(r, &rect);

    SDL_RenderPresent(r);
}

static void welcome_screen(SDL_Renderer* r) {
    // 1. Only call SDL drawing if we are NOT in text mode and renderer is valid
    if (!g_text_mode && r) {
        draw_welcome(r);
    }

    int waiting = 1;
    while (waiting) {
        SDL_Event ev;
        
        // 2. Poll events (works fine even in text mode if a dummy/hidden SDL window exists)
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_KEYDOWN) {
                if (ev.key.keysym.scancode == SDL_SCANCODE_1) {
                    waiting = 0;
                }
                if (ev.key.keysym.sym == SDLK_ESCAPE) {
                    SDL_Quit();
                    exit(0);
                }
            } else if (ev.type == SDL_QUIT) {
                waiting = 0;
            } else if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_EXPOSED) {
                if (!g_text_mode && r) {
                    draw_welcome(r);
                }
            }
        }

        // 3. Render based on active mode
        if (g_text_mode) {
            render_insert_coin_text();
            
            // Sleep ~33ms for a smooth ~30fps in text mode
            struct timespec ts = {0, 33000000}; 
            nanosleep(&ts, NULL);
        } else {
            // Existing SDL rendering for the 3 colored boxes
            draw_welcome(r); 
            SDL_Delay(16); // ~60fps for SDL
        }
    }
}

static void play_level_intro(SDL_Renderer* r) {
    static const int EXPAND_FRAMES = 90;
    static const int RECT_THICKNESS = 5;
    static const SDL_Color colors[12] = {
        {255, 255, 255, 255}, {255, 0,   255, 255}, {255, 0,   0,   255},
        {255, 127, 0,   255}, {255, 255, 0,   255}, {0,   255, 0,   255},
        {0,   255, 255, 255}, {0,   127, 255, 255}, {0,   0,   255, 255},
        {127, 0,   255, 255}, {255, 0,   255, 255}, {255, 255, 255, 255}
    };
    int center_x = SCREEN_WIDTH / 2;
    int center_y = SCREEN_HEIGHT / 2;
    int max_w = SCREEN_WIDTH + 220;
    int max_h = SCREEN_HEIGHT + 180;
    for (int frame = 0; frame < EXPAND_FRAMES; frame++) {
        SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
        SDL_RenderClear(r);
        int progress = (frame * 100) / EXPAND_FRAMES;
        int ease_progress = (progress * progress) / 100;
        int current_w = (ease_progress * max_w) / 100;
        int current_h = (ease_progress * max_h) / 100;
        for (int i = 11; i >= 0; i--) {
            int offset = i * RECT_THICKNESS;
            int w = current_w - (offset * 2);
            int h = current_h - (offset * 2);
            if (w > 0 && h > 0) {
                SDL_Rect rect = { center_x - w / 2, center_y - h / 2, w, h };
                SDL_SetRenderDrawColor(r, colors[i].r, colors[i].g, colors[i].b, 255);
                SDL_RenderDrawRect(r, &rect);
            }
        }
        SDL_RenderPresent(r);
        SDL_Delay(16);
    }
}

// ============================================================
// --- Update ---
// ============================================================
static void update_player(Player* p) {
    if (!p->active || g_game_over) return;
    const Uint8* keystate = SDL_GetKeyboardState(NULL);
    p->vx = 0;
    p->vy = 0;
    if (keystate[SDL_SCANCODE_W]) p->vy = -PLAYER_SPEED;
    if (keystate[SDL_SCANCODE_S]) p->vy = PLAYER_SPEED;
    if (keystate[SDL_SCANCODE_A]) p->vx = -PLAYER_SPEED;
    if (keystate[SDL_SCANCODE_D]) p->vx = PLAYER_SPEED;
    if (p->vx != 0 && p->vy != 0) {
        p->vx = fixed_mul(p->vx, FIXED_0_707);
        p->vy = fixed_mul(p->vy, FIXED_0_707);
    }
    static int fire_cooldown = 0;
    if (fire_cooldown > 0) fire_cooldown--;
    bool any_fire = keystate[SDL_SCANCODE_I] || keystate[SDL_SCANCODE_K] ||
                    keystate[SDL_SCANCODE_J] || keystate[SDL_SCANCODE_L];
    if (any_fire && fire_cooldown == 0) {
        if (p->shot_buffer_timer == 0) {
            p->shot_buffer_timer = INPUT_BUFFER_FRAMES;
        }
        int dx = 0, dy = 0;
        if (keystate[SDL_SCANCODE_I]) dy = -1;
        if (keystate[SDL_SCANCODE_K]) dy = 1;
        if (keystate[SDL_SCANCODE_J]) dx = -1;
        if (keystate[SDL_SCANCODE_L]) dx = 1;
        if (dx != 0 && dy != 0) {
            int16_t diag = fixed_mul(LASER_SPEED, FIXED_0_707);
            p->shot_pending_vx = diag * dx;
            p->shot_pending_vy = diag * dy;
        } else if (dx != 0) {
            p->shot_pending_vx = LASER_SPEED * dx;
            p->shot_pending_vy = 0;
        } else if (dy != 0) {
            p->shot_pending_vx = 0;
            p->shot_pending_vy = LASER_SPEED * dy;
        }
        if (p->shot_buffer_timer > 0) {
            p->shot_buffer_timer--;
            if (p->shot_buffer_timer == 0) {
                Direction dir = get_dir_from_vel(p->shot_pending_vx, p->shot_pending_vy);
                spawn_laser(p->wx + SCREEN_TO_FIXED(sprite_player.width/2)  - SCREEN_TO_FIXED(sprite_laser.width/2),
                    p->wy + SCREEN_TO_FIXED(sprite_player.height/2) - SCREEN_TO_FIXED(sprite_laser.height/2),
                    p->shot_pending_vx, p->shot_pending_vy, dir);
                fire_cooldown = FIRE_COOLDOWN;
            }
        }
    } else if (!any_fire) {
        p->shot_buffer_timer = 0;
    }
    p->wx += p->vx;
    p->wy += p->vy;
    clamp_to_screen(&p->wx, &p->wy, p->sprite->width, p->sprite->height);
}

static void update_entities(void) {
    for (int16_t idx = g_list_head; idx != -1; ) {
        int16_t next_idx = g_next[idx];
        Entity* e = &g_entities[idx];
        if (!e->active) { idx = next_idx; continue; }
        // Handle hulk pushback
        if (e->type == ENT_HULK && e->pushback_timer > 0) {
            e->pushback_timer--;
            e->wx += e->vx;
            e->wy += e->vy;
            clamp_to_screen(&e->wx, &e->wy, e->sprite->width, e->sprite->height);
            idx = next_idx;
            continue;
        }
        bool tick_ready = ((g_frame_count - e->tick_phase) % e->tick_period) == 0;
        if (tick_ready) {
            switch (e->type) {
                case ENT_HUMAN:    ai_human(e);    break;
                case ENT_GRUNT:    ai_grunt(e);    break;
                case ENT_SPHEROID: ai_spheroid(e); break;
                case ENT_ENFORCER: ai_enforcer(e); break;
                case ENT_HULK:     ai_hulk(e);     break;
                case ENT_QUARK:    break;
                case ENT_BRAIN:    break;
                case ENT_CRUISE:   break;
                case ENT_LASER:    break;
                case ENT_TERROR:   break;
                default: break;
            }
            e->wx += e->vx;
            e->wy += e->vy;
        }
        if (e->type == ENT_LASER || e->type == ENT_TERROR) {
            e->age++;
            if (e->wx < -SCREEN_TO_FIXED(e->sprite->width) ||
                e->wx > SCREEN_TO_FIXED(SCREEN_WIDTH) ||
                e->wy < -SCREEN_TO_FIXED(e->sprite->height) ||
                e->wy > SCREEN_TO_FIXED(SCREEN_HEIGHT)) {
                if (e->type == ENT_LASER) g_laser_count--;
                remove_entity(idx);
                idx = next_idx;
                continue;
            }
        } else {
            clamp_to_screen(&e->wx, &e->wy, e->sprite->width, e->sprite->height);
            int new_dir = get_dir_from_vel(e->vx, e->vy);
            if (new_dir != DIR_NONE) {
                e->move_dir = new_dir;
                e->facing_dir = new_dir;
            }
        }
        e->anim_frame = (e->anim_frame + 1) % 2;
        idx = next_idx;
    }
}

// ============================================================
//  COLLISION DETECTION — uses spatial grid
// ============================================================

static void process_player_vs_entities(void) {
    for (int p = 0; p < g_player_count; p++) {
        Player* pl = &g_players[p];
        if (!pl->active || g_game_over || pl->invulnerable_timer > 0) continue;

        int cell_x = (int)pl->wx / GRID_CELL_SIZE;
        int cell_y = (int)pl->wy / GRID_CELL_SIZE;
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
                        case ENT_HUMAN:
                            g_remove_enemy[g_grid_nodes[nidx].entity_idx] = true;
                            g_rescue_count++;
                            {
                                int points = (g_rescue_count <= 5) ? (g_rescue_count * 1000) : 5000;
                                g_score += points;
                            }
                            break;
                        case ENT_TERROR:
                        case ENT_CRUISE:
                        case ENT_GRUNT:
                        case ENT_HULK:
                        case ENT_SPHEROID:
                        case ENT_ENFORCER:
                        case ENT_BRAIN:
                        case ENT_QUARK:
                        case ENT_ELECTRODE:
                            g_remove_enemy[g_grid_nodes[nidx].entity_idx] = true;
                            pl->lives--;
                            pl->ghost_x = pl->wx;
                            pl->ghost_y = pl->wy;
                            pl->active = false;
                            pl->death_timer = 30;
                            if (pl->lives <= 0) {
                                g_game_over = true;
                                g_show_game_over = true;
                            }
                            break;
                    }
                }
            }
        }
    }

    // Removal pass for entities marked by player-vs-entity collisions
    for (int16_t idx = g_list_head; idx != -1; ) {
        int16_t next_idx = g_next[idx];
        if (g_remove_enemy[idx]) {
            remove_entity(idx);
        }
        idx = next_idx;
    }
}


static void process_entity_vs_entity(void) {
    // Reset removal flags
    for (int i = 0; i < MAX_ENTITIES; i++) {
        g_remove_laser[i] = false;
        g_remove_enemy[i] = false;
    }

    // Walk all lasers and check nearby entities via grid
    for (int16_t idx = g_list_head; idx != -1; idx = g_next[idx]) {
        Entity* a = &g_entities[idx];
        if (!a->active || a->type != ENT_LASER) continue;

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
                    if (b->type == ENT_HUMAN || b->type == ENT_LASER) continue;

                    if (b->type == ENT_HULK) {
                        int16_t dist = SCREEN_TO_FIXED(HULK_PUSHBACK);
                        if (a->vx > 0) b->wx += dist;
                        else if (a->vx < 0) b->wx -= dist;
                        if (a->vy > 0) b->wy += dist;
                        else if (a->vy < 0) b->wy -= dist;
                        clamp_to_screen(&b->wx, &b->wy, b->sprite->width, b->sprite->height);
                        g_remove_laser[idx] = true;
                        continue;
                    }
                    if (check_swept_collision(a, b)) {
                        g_remove_laser[idx] = true;
                        g_remove_enemy[g_grid_nodes[nidx].entity_idx] = true;
                        switch (b->type) {
                            case ENT_GRUNT:     g_score += 100; break;
                            case ENT_BRAIN:     g_score += 500; break;
                            case ENT_SPHEROID:  g_score += 250; break;
                            case ENT_ENFORCER:  g_score += 200; break;
                            case ENT_ELECTRODE: g_score += 50;  break;
                            case ENT_TERROR:    break;
                            case ENT_CRUISE:    g_score += 300; break;
                            default: break;
                        }
                    }
                }
            }
        }
    }

    // Walk all hulks and check nearby humans via grid
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
                            g_remove_enemy[g_grid_nodes[nidx].entity_idx] = true;
                        }
                    }
                }
            }
        }
    }

    // Removal pass
    for (int16_t idx = g_list_head; idx != -1; ) {
        int16_t next_idx = g_next[idx];
        if (g_remove_laser[idx] || g_remove_enemy[idx]) {
            if (g_entities[idx].type == ENT_LASER) g_laser_count--;
            remove_entity(idx);
        }
        idx = next_idx;
    }
}

static void check_level_complete(SDL_Renderer* r) {
    if (r == NULL) return;  // text mode - skip intro
    int enemy_count = 0;
    for (int16_t idx = g_list_head; idx != -1; idx = g_next[idx]) {
        Entity* e = &g_entities[idx];
        if (e->active &&
            e->type != ENT_LASER &&
            e->type != ENT_TERROR &&
            e->type != ENT_HULK &&
            e->type != ENT_HUMAN &&
            e->type != ENT_ELECTRODE &&
            e->type != ENT_CRUISE) {
              enemy_count++;
            }
    }
    if (enemy_count == 0 && !g_game_over) {
        if (g_text_mode) {
            play_level_intro_text();
        } else {
            play_level_intro(r);
        } 
        reset_level();
    }
}

static void update_all(SDL_Renderer* r) {
    // Add NULL check for text mode
    if (g_show_game_over) return;

    if (!g_players[0].active && g_players[0].death_timer > 0) {
        g_players[0].death_timer--;
        if (g_players[0].death_timer == 0) {
            if (g_players[0].lives > 0) {
                int16_t spawn_x, spawn_y;
                find_safe_spawn(&spawn_x, &spawn_y);
                g_players[0].active = true;
                g_players[0].wx = spawn_x;
                g_players[0].wy = spawn_y;
                g_players[0].vx = 0;
                g_players[0].vy = 0;
                g_players[0].ghost_timer = GHOST_TIMER;
                g_players[0].invulnerable_timer = INVULNERABLE_FRAMES;
            }
        }
    }

    if (g_players[0].invulnerable_timer > 0) {
        g_players[0].invulnerable_timer--;
    }
    if (g_players[0].ghost_timer > 0) g_players[0].ghost_timer--;

        if (!g_game_over) {
        update_player(&g_players[0]);
        rebuild_grid();
        update_entities();
        process_entity_vs_entity();
        process_player_vs_entities();
        check_level_complete(r);  // will be NULL-safe
        }
    g_frame_count++;

}

// ============================================================
// --- Rendering TEXT MODE ---
// ============================================================
// Helper to get terminal dimensions via tput
//
static void init_term_dims(void) {
    char buf[16];
    FILE* f;

    f = popen("tput cols", "r");
    if (f && fgets(buf, sizeof(buf), f)) g_text_cols = atoi(buf);
    pclose(f);
    if (g_text_cols <= 0) g_text_cols = 80;

    f = popen("tput lines", "r");
    if (f && fgets(buf, sizeof(buf), f)) g_text_rows = atoi(buf);
    pclose(f);
    if (g_text_rows <= 0) g_text_rows = 24;

    snprintf(buf, sizeof(buf), "%d", g_text_cols);
    setenv("COLUMNS", buf, 1);
    snprintf(buf, sizeof(buf), "%d", g_text_rows);
    setenv("LINES", buf, 1);
}

// ============================================================
//  2. SPRITE DEFINITIONS
// ============================================================


static void text_mode_initOLD(void) {
    init_term_dims();
    g_text_out = popen("python3 sprite_bridge.py 2>sprite_bridge.log", "w");
    if (!g_text_out) {
        fprintf(stderr, "ERROR: Failed to spawn sprite_bridge.py\n");
        g_text_mode = false;
        return;
    }
    
    // Send existing sprite definitions
    fprintf(g_text_out, "%s", g_text_sprites);
    
    // Send digit sprites for score display
    for (int i = 0; i < 10; i++) {
        fprintf(g_text_out, digit_sprites[i], 
                SCORE_COLOR, "\x1b[0m",
                SCORE_COLOR, "\x1b[0m",
                SCORE_COLOR, "\x1b[0m");
    }
    
    // Initial screen clear
    fprintf(g_text_out, "CFLUSH\n");
    fflush(g_text_out);
}
static void text_mode_init(void) {
    init_term_dims();
    g_text_out = popen("python3 sprite_bridge.py 2>sprite_bridge.log", "w");
    if (!g_text_out) {
        fprintf(stderr, "ERROR: Failed to spawn sprite_bridge.py\n");
        g_text_mode = false;
        return;
    }
    
    // 1. Send existing game sprites
    fprintf(g_text_out, "%s", g_text_sprites);
    
    // 2. Send digit sprites (500-509)
    for (int i = 0; i < 10; i++) {
        fprintf(g_text_out, digit_sprites[i], SCORE_COLOR, "\x1b[0m");
    }
    
    // 3. Send instruction sprite (510)
    fprintf(g_text_out, press_1_sprite, TEXT_COLOR, "\x1b[0m");
    
    fflush(g_text_out);
}

static void draw_score_text(int score) {
    if (!g_text_out) return;
    
    // Convert score to string
    char score_str[8];
    snprintf(score_str, sizeof(score_str), "%d", score);
    int len = strlen(score_str);
    
    // Right-align in 7-digit field
    // Each digit is 3 chars wide, so 7 digits = 21 chars total
    // Start position: column 6 + (7 - len) * 3
    int start_col = 6 + (7 - len) * 3;
    
    // Draw each digit
    for (int i = 0; i < len; i++) {
        int digit = score_str[i] - '0';
        int col = start_col + i * 3;
        int row = 0;  // Top of screen
        
        // Sprite ID 500 + digit
        fprintf(g_text_out, "DRAW,%d,900,%d,%d,0\n", 
                500 + digit, col, row);
    }
}

static void play_level_intro_text(void) {
    if (!g_text_out) return;

    // Bright ANSI colors matching the SDL palette
    static const char* colors[12] = {
        "\x1b[97m",          // White
        "\x1b[95m",          // Magenta
        "\x1b[91m",          // Red
        "\x1b[38;5;208m",    // Orange
        "\x1b[93m",          // Yellow
        "\x1b[92m",          // Green
        "\x1b[96m",          // Cyan
        "\x1b[94m",          // Light Blue
        "\x1b[34m",          // Blue
        "\x1b[38;5;129m",    // Purple
        "\x1b[95m",          // Magenta
        "\x1b[97m"           // White
    };

    int cx = g_text_cols / 2;
    int cy = g_text_rows / 2;
    
    // Expand slightly beyond screen bounds so it cleanly wipes the edges
    int max_w = g_text_cols + 4;
    int max_h = g_text_rows + 4;
    
    // 45 frames at ~30fps = 1.5 second animation
    int frames = 45;
    int num_boxes = 8; // 8 boxes with 1-char spacing looks great in text mode

    for (int frame = 0; frame < frames; frame++) {
        // 1. Clear the offscreen buffer (no terminal output yet)
        fprintf(g_text_out, "CLEAR\n");

        // 2. Calculate quadratic ease-in progress
        int progress = (frame * 100) / frames;
        int ease_progress = (progress * progress) / 100;
        int current_w = (ease_progress * max_w) / 100;
        int current_h = (ease_progress * max_h) / 100;

        // 3. Draw concentric boxes from outside-in
        for (int i = num_boxes - 1; i >= 0; i--) {
            int offset = i; // 1 character spacing between boxes
            int w = current_w - (offset * 2);
            int h = current_h - (offset * 2);

            if (w > 2 && h > 2) {
                int x1 = cx - w / 2;
                int y1 = cy - h / 2;
                int x2 = x1 + w - 1;
                int y2 = y1 + h - 1;

                // Clamp to screen bounds
                if (x1 < 0) x1 = 0;
                if (y1 < 0) y1 = 0;
                if (x2 >= g_text_cols) x2 = g_text_cols - 1;
                if (y2 >= g_text_rows) y2 = g_text_rows - 1;

                // Only draw if valid dimensions remain
                if (x1 < x2 && y1 < y2) {
                    // Type 1 = single line box, with ANSI color prefix
                    fprintf(g_text_out, "BOX,%d,%d,%d,%d,1,%s\n", 
                            x1, y1, x2, y2, colors[i % 12]);
                }
            }
        }

        // 4. Emit the frame to the terminal
        fprintf(g_text_out, "FLUSH\n");
        fflush(g_text_out);

        // 5. Pace the animation (~32ms per frame = ~30fps)
        struct timespec ts = {0, 32000000}; 
        nanosleep(&ts, NULL);
    }

    // Final clear to transition cleanly into the actual gameplay frame
    fprintf(g_text_out, "CLEAR\n");
    fprintf(g_text_out, "FLUSH\n");
    fflush(g_text_out);
}

static void render_insert_coin_text(void) {
    if (!g_text_out) return;
    
    // 1. Clear buffer (Mode A: safe for frame skipping)
    fprintf(g_text_out, "CLEAR\n");
    
    // 2. Draw digits 0-9 centered
    // 10 digits * 3 cols = 30 cols wide, 3 rows high
    int digit_w = 30;
    int digit_h = 3;
    int start_x = (g_text_cols - digit_w) / 2;
    int start_y = (g_text_rows - digit_h) / 2;
    
    for (int i = 0; i < 10; i++) {
        // Instance ID 900 reserved for score/digit display
        fprintf(g_text_out, "DRAW,%d,900,%d,%d,0\n", 500 + i, start_x + (i * 3), start_y);
    }
    
    // 3. Draw instruction text in lower middle
    // 32 cols wide, 1 row high
    int text_w = 32;
    int text_x = (g_text_cols - text_w) / 2;
    int text_y = g_text_rows - 4; // 4 rows from the bottom
    
    // Instance ID 901 reserved for this instruction
    fprintf(g_text_out, "DRAW,510,901,%d,%d,0\n", text_x, text_y);
    
    // 4. Commit frame
    fprintf(g_text_out, "FLUSH\n");
    fflush(g_text_out);
}

static void render_text_mode(void) {
    if (!g_text_out) return;
    
    // 1. Clear the offscreen buffer ONLY. 
    // This resets the bridge's internal state without emitting to the terminal.
    fprintf(g_text_out, "CLEAR\n");

    // 2. Draw player
    if (g_players[0].active && g_players[0].death_timer == 0) {
        Player* p = &g_players[0];
        int sprite_id = entity_to_sprite(ENT_PLAYER);
        if (sprite_id >= 0) {
            int tx = (FIXED_TO_SCREEN(p->wx) * g_text_cols) / SCREEN_WIDTH;
            int ty = (FIXED_TO_SCREEN(p->wy) * g_text_rows) / SCREEN_HEIGHT;
            if (tx < 0) tx = 0; 
            if (ty < 0) ty = 0;

            // Instance ID 0 is reserved for the player
            fprintf(g_text_out, "DRAW,%d,0,%d,%d,0\n", sprite_id, tx, ty);
        }
    }

    // 3. Draw all active entities
    for (int16_t idx = g_list_head; idx != -1; idx = g_next[idx]) {
        Entity* e = &g_entities[idx];
        if (!e->active) continue;

        int sprite_id = entity_to_sprite(e->type);
        if (sprite_id < 0) continue;

        int tx = (FIXED_TO_SCREEN(e->wx) * g_text_cols) / SCREEN_WIDTH;
        int ty = (FIXED_TO_SCREEN(e->wy) * g_text_rows) / SCREEN_HEIGHT;
        if (tx < 0) tx = 0;
        if (ty < 0) ty = 0;

        // The entity's pool index (idx) is its unique instance ID
        fprintf(g_text_out, "DRAW,%d,%d,%d,%d,0\n", sprite_id, idx, tx, ty);
    }

    // 4. Draw score display (HUD OVERLAY)
    // Unconditional: renders every time this function is called.
    // Placed LAST so it overwrites any entities flying through the top-left corner.
    {
        char score_str[16];
        snprintf(score_str, sizeof(score_str), "%d", g_score);
        int len = strlen(score_str);
        
        // Right-align in 7-digit field (7 digits * 3 chars = 21 chars total width)
        // Starting column: 6 + (7 - len) * 3
        int start_col = 6 + (7 - len) * 3;
        int row = 0;  // Top of screen
        
        // Draw each digit. Instance ID 900 is reserved for the score display.
        // Sprite IDs 500-509 correspond to digits 0-9.
        for (int i = 0; i < len; i++) {
            int digit = score_str[i] - '0';
            int col = start_col + i * 3;
            fprintf(g_text_out, "DRAW,%d,900,%d,%d,0\n", 500 + digit, col, row);
        }
    }

    // 5. Commit the frame to the terminal. 
    // THIS IS THE ONLY EMIT POINT. The bridge's skip logic hinges on this.
    fprintf(g_text_out, "FLUSH\n");
    fflush(g_text_out);
}

static void text_mode_fini(void) {
    if (g_text_out) {
        pclose(g_text_out);
        g_text_out = NULL;
    }
}





// NORMAL SDL RENDERING BELOW
static void draw_entity(SDL_Renderer* r, Entity* e) {
    SDL_Rect dst;
    dst.x = FIXED_TO_SCREEN(e->wx);
    dst.y = FIXED_TO_SCREEN(e->wy);
    dst.w = e->sprite->width;
    dst.h = e->sprite->height;

    if (e->type == ENT_ELECTRODE) {
        int cx = dst.x + dst.w / 2;
        int cy = dst.y + dst.h / 2;
        int bar = 4;
        SDL_Rect vbar = { cx - bar / 2, dst.y, bar, dst.h };
        SDL_SetRenderDrawColor(r, 255, 50, 0, 255);
        SDL_RenderFillRect(r, &vbar);
        SDL_Rect hbar = { dst.x, cy - bar / 2, dst.w, bar };
        SDL_SetRenderDrawColor(r, 255, 255, 0, 255);
        SDL_RenderFillRect(r, &hbar);
        return;
    }

    if (e->type == ENT_CRUISE) {
        int idx = -1;
        for (int16_t i = g_list_head; i != -1; i = g_next[i]) {
            if (&g_entities[i] == e) { idx = i; break; }
        }
        if (idx >= 0) {
            for (int i = g_cruise_body_len[idx] - 1; i >= 0; i--) {
                SDL_Rect seg;
                seg.x = FIXED_TO_SCREEN(g_cruise_body_x[idx][i]);
                seg.y = FIXED_TO_SCREEN(g_cruise_body_y[idx][i]);
                seg.w = e->sprite->width;
                seg.h = e->sprite->height;
                uint8_t shade = (i == 0) ? 255 : 150;
                SDL_SetRenderDrawColor(r, 0, shade, 255, 255);
                SDL_RenderFillRect(r, &seg);
            }
        }
        return;
    }

    // FIXED: set color BEFORE fill for all entity types
    if (e->type == ENT_HUMAN) {
        static const uint8_t human_colors[3][3] = {
            {255, 100, 200},  // mommy
            {100, 200, 255},  // daddy
            {255, 255, 100}   // mikey
        };
        int ht = e->human_type;
        if (ht < 0 || ht > 2) ht = 0;
        SDL_SetRenderDrawColor(r, human_colors[ht][0], human_colors[ht][1], human_colors[ht][2], 255);
    } else {
        // Grunt, Brain, Hulk, Spheroid, Enforcer, Quark, Terror, Laser, Player
        SDL_SetRenderDrawColor(r, e->sprite->r, e->sprite->g, e->sprite->b, 255);
    }

    SDL_RenderFillRect(r, &dst);
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    SDL_RenderDrawRect(r, &dst);
}


static void draw_player(SDL_Renderer* r, Player* p) {
    if (p->death_timer > 0) {
        int shrink_factor = (p->death_timer * 20) / 30;
        if (shrink_factor < 2) shrink_factor = 2;
        SDL_Rect dst;
        dst.x = FIXED_TO_SCREEN(p->wx) + (p->sprite->width - shrink_factor) / 2;
        dst.y = FIXED_TO_SCREEN(p->wy) + (p->sprite->height - shrink_factor) / 2;
        dst.w = shrink_factor;
        dst.h = shrink_factor;
        SDL_SetRenderDrawColor(r, 255, 0, 0, 255);
        SDL_RenderFillRect(r, &dst);
        return;
    }

    SDL_Rect dst;
    dst.x = FIXED_TO_SCREEN(p->wx);
    dst.y = FIXED_TO_SCREEN(p->wy);
    dst.w = p->sprite->width;
    dst.h = p->sprite->height;

    if (p->invulnerable_timer > 0 && (g_frame_count / 6) % 2 == 0) {
        SDL_SetRenderDrawColor(r, 0, 255, 255, 255);
    } else {
        SDL_SetRenderDrawColor(r, p->sprite->r, p->sprite->g, p->sprite->b, 255);
    }

    SDL_RenderFillRect(r, &dst);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderDrawRect(r, &dst);
}

static void draw_hud(SDL_Renderer* r) {
    SDL_Rect level_rect = {10, 10, 120, 20};
    SDL_SetRenderDrawColor(r, 50, 50, 50, 255);
    SDL_RenderFillRect(r, &level_rect);

    int lives_start_x = SCREEN_WIDTH - 30;
    for (int i = 0; i < g_players[0].lives; i++) {
        SDL_Rect life_rect = {lives_start_x - (i * 25), 10, 20, 20};
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        SDL_RenderFillRect(r, &life_rect);
        SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
        SDL_RenderDrawRect(r, &life_rect);
    }

    if (g_show_game_over) {
        SDL_Rect go_rect = {SCREEN_WIDTH/2 - 100, SCREEN_HEIGHT/2 - 20, 200, 40};
        SDL_SetRenderDrawColor(r, 100, 0, 0, 255);
        SDL_RenderFillRect(r, &go_rect);
        SDL_Rect text_rect = {SCREEN_WIDTH/2 - 60, SCREEN_HEIGHT/2 - 10, 120, 20};
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        SDL_RenderFillRect(r, &text_rect);
    }
}

static void render_all(SDL_Renderer* r) {
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    draw_hud(r);
    if (g_players[0].active || g_players[0].death_timer > 0) {
        draw_player(r, &g_players[0]);
    }
    for (int16_t idx = g_list_head; idx != -1; idx = g_next[idx]) {
        if (g_entities[idx].active) {
            draw_entity(r, &g_entities[idx]);
        }
    }
    SDL_RenderPresent(r);
}

// ============================================================
// --- Main ---
// ============================================================

int main(int argc, char* argv[]) {
    srand((unsigned)time(NULL));

    // Parse args
    int start_level = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0) {
            g_text_mode = true;
        } else if (isdigit(argv[i][0])) {
            start_level = atoi(argv[i]);
            if (start_level < 1) start_level = 1;
            if (start_level > 40) start_level = 40;
        }
    }

    restart_game(start_level);

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL Init Error: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("Robotron", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_WIDTH, SCREEN_HEIGHT, SDL_WINDOW_SHOWN);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!window || !renderer) {
        SDL_Quit();
        return 1;
    }

    // Initialize text mode after SDL window is created
    if (g_text_mode) {
        text_mode_init();
    }

    welcome_screen(renderer);
    if (g_text_mode) {
        play_level_intro_text();
    } else {
        play_level_intro(renderer);
    } 
    reset_level();

    int running = 1;
    Uint32 target_ms = 16;
    while (running) {
        Uint32 start = SDL_GetTicks();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = 0;
            else if (ev.type == SDL_KEYDOWN) {
                if (ev.key.keysym.sym == SDLK_ESCAPE) running = 0;
                else if (g_show_game_over) running = 0;
            }
        }

        update_all(renderer);
        render_all(renderer);  // Always render SDL window

        if (g_text_mode) {
            render_text_mode();  // Render to text bridge after SDL
        }

        Uint32 elapsed = SDL_GetTicks() - start;
        if (elapsed < target_ms) SDL_Delay(target_ms - elapsed);
    }

    if (g_text_mode) {
        text_mode_fini();
    }
    
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    printf("\nYour score: %d\n",g_score);
    return 0;
}
