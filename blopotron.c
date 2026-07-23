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
/*
// ============================================================
//  Blapotron 2024 terminal robotron-like game: ver. 0.60
//  Pure Terminal Mode - No SDL Dependencies
//  gcc -O2 -Wall bta.c -o bta -lm
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
} Player;

typedef struct {
    int grunts, electrodes, hulks, brains, spheroids, quarks, mommies, daddies, mikeys;
} LevelWave;

typedef struct {
    char glyph[4];             // UTF-8 character (up to 3 bytes + null terminator)
    unsigned char r, g, b;     // Foreground RGB
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

// Sprites
static const SpriteData sprite_player    = {20, 20, 255, 255, 255, 255, "Player"};
static const SpriteData sprite_human     = {12, 18, 255, 200, 200, 255, "Human"};
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
static void play_level_intro_text(void);
static void process_player_vs_entities(void);
static void init_text_mode(void);
static void fini_text_mode(void);
static void poll_input(void);
static void render_all(void);

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
    for (int y = 0; y < g_term_rows; y++) {
        for (int x = 0; x < g_term_cols; x++) {
            TextCell cell = text_buffer[y * g_term_cols + x];
            if (cell.r != last_r || cell.g != last_g || cell.b != last_b) {
                printf("\x1b[38;2;%d;%d;%dm", cell.r, cell.g, cell.b);
                last_r = cell.r; last_g = cell.g; last_b = cell.b;
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
    int max_x = SCREEN_TO_FIXED(SCREEN_WIDTH) - SCREEN_TO_FIXED(e->sprite->width);
    int max_y = SCREEN_TO_FIXED(SCREEN_HEIGHT) - SCREEN_TO_FIXED(e->sprite->height);
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
            clamp_to_screen(&tx, &ty, sprite_player.width, sprite_player.height);
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
    int16_t e_min_x = enemy->wx, e_max_x = enemy->wx + SCREEN_TO_FIXED(enemy->sprite->width);
    int16_t e_min_y = enemy->wy, e_max_y = enemy->wy + SCREEN_TO_FIXED(enemy->sprite->height);
    for (int i = 0; i <= 8; i++) {
        int16_t cx = laser->wx + (laser->vx * i) / 8;
        int16_t cy = laser->wy + (laser->vy * i) / 8;
        if (cx >= e_min_x && cx <= e_max_x && cy >= e_min_y && cy <= e_max_y) return true;
    }
    return false;
}

static bool check_player_enemy_collision(Player* p, Entity* e) {
    int16_t p_min_x = p->wx, p_max_x = p->wx + SCREEN_TO_FIXED(p->sprite->width);
    int16_t p_min_y = p->wy, p_max_y = p->wy + SCREEN_TO_FIXED(p->sprite->height);
    int16_t e_min_x = e->wx, e_max_x = e->wx + SCREEN_TO_FIXED(e->sprite->width);
    int16_t e_min_y = e->wy, e_max_y = e->wy + SCREEN_TO_FIXED(e->sprite->height);
    return (p_min_x < e_max_x && p_max_x > e_min_x && p_min_y < e_max_y && p_max_y > e_min_y);
}

static bool check_entity_entity_collision(Entity* a, Entity* b) {
    int16_t a_min_x = a->wx, a_max_x = a->wx + SCREEN_TO_FIXED(a->sprite->width);
    int16_t a_min_y = a->wy, a_max_y = a->wy + SCREEN_TO_FIXED(a->sprite->height);
    int16_t b_min_x = b->wx, b_max_x = b->wx + SCREEN_TO_FIXED(b->sprite->width);
    int16_t b_min_y = b->wy, b_max_y = b->wy + SCREEN_TO_FIXED(b->sprite->height);
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
    e->attitude_counter = 0; e->anim_frame = 0; e->facing_dir = DIR_DOWN; e->move_dir = DIR_DOWN;
    e->age = 0; e->pushback_timer = 0; e->human_type = 0; e->spawn_count = 0; e->fire_phase = 0;
    switch (type) {
        case ENT_HUMAN: e->sprite = &sprite_human; e->tick_period = 2; e->attitude_period = 128; e->attitude_counter = 128; e->human_type = rand() % 3; break;
        case ENT_GRUNT: e->sprite = &sprite_grunt; e->tick_period = 23; e->tick_phase = rand() % e->tick_period; break;
        case ENT_HULK: e->sprite = &sprite_hulk; e->tick_period = 4; e->attitude_period = 45; e->attitude_counter = rand() % e->attitude_period; e->target_entity = -1; e->target_period = 8; e->target_counter = 0; break;
        case ENT_SPHEROID: e->sprite = &sprite_spheroid; e->tick_period = 2; e->attitude_period = 4; e->attitude_counter = rand() % 4; e->fire_period = 128; e->fire_counter = 128; e->spawn_count = 0; e->spawn_max = 4; e->state = SPHEROID_STATE_MOVE; e->target_counter = 0; e->target_period = SPHEROID_WINDUP_TICKS; { int corner = rand() % 4; switch (corner) { case 0: e->mtgx = 0; e->mtgy = 0; break; case 1: e->mtgx = SCREEN_TO_FIXED(SCREEN_WIDTH); e->mtgy = 0; break; case 2: e->mtgx = 0; e->mtgy = SCREEN_TO_FIXED(SCREEN_HEIGHT); break; case 3: e->mtgx = SCREEN_TO_FIXED(SCREEN_WIDTH); e->mtgy = SCREEN_TO_FIXED(SCREEN_HEIGHT); break; } int16_t dx = e->mtgx - e->wx, dy = e->mtgy - e->wy; int32_t dist = abs(dx) + abs(dy); if (dist == 0) dist = 1; int16_t sspd = SPHEROID_SPEED * COORD_SCALE; e->vx = (int16_t)(((int32_t)dx * sspd) / dist); e->vy = (int16_t)(((int32_t)dy * sspd) / dist); } break;
        case ENT_ENFORCER: e->sprite = &sprite_enforcer; e->attitude_period = 20; e->attitude_counter = 20; e->fire_period = 40; e->fire_counter = 40; e->tick_period = 2; e->tick_phase = 0; { int mask = get_wall_bitmask(e); int16_t speed = ENFORCER_SPEED * COORD_SCALE; e->vx = speed + rand() % speed * (((mask & WALL_LEFT) != 0) - ((mask & WALL_RIGHT) != 0)); e->vy = speed + rand() % speed * (((mask & WALL_TOP) != 0) - ((mask & WALL_BOTTOM) != 0)); } break;
        case ENT_BRAIN: e->sprite = &sprite_brain; e->tick_period = 4; e->tick_phase = rand() % e->tick_period; e->tick_counter = e->tick_period; e->attitude_period = 32; e->attitude_counter = e->attitude_period; e->fire_period = 32 + (rand() % 32); break;
        case ENT_CRUISE: e->sprite = &sprite_cruise; e->tick_period = 1; e->attitude_counter = 20; break;
        case ENT_TERROR: e->sprite = &sprite_terror; e->tick_period = 1; break;
        case ENT_LASER: e->sprite = &sprite_laser; e->tick_period = 1; break;
        case ENT_ELECTRODE: e->sprite = &sprite_electrode; e->tick_period = 9999999; break;
        case ENT_QUARK: e->sprite = &sprite_quark; e->tick_period = 4; break;
        default: e->active = false; free_entity(idx); return NULL;
    }
    link_entity(idx); return e;
}

static void spawn_player(int16_t wx, int16_t wy) {
    if (g_player_count >= MAX_PLAYERS) return;
    Player* p = &g_players[g_player_count++];
    p->wx = wx; p->wy = wy; p->vx = 0; p->vy = 0; p->type = ENT_PLAYER;
    p->sprite = &sprite_player; p->active = true; p->lives = PLAYER_LIVES;
    p->death_timer = 0; p->invulnerable_timer = 0; p->ghost_x = 0; p->ghost_y = 0;
    p->ghost_timer = 0; p->shot_buffer_timer = 0; p->shot_pending_vx = 0; p->shot_pending_vy = 0;
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

    // 2. Firing Input (Autofire via Numpad / Number Row)
    // 7 8 9
    // 4 5 6
    // 1 2 3
    int fire_vx = 0, fire_vy = 0;
    bool fire_pressed = false;
    
    #define CHECK_FIRE(key, dx, dy) \
        if (g_keys[(unsigned char)(key)]) { fire_vx = dx; fire_vy = dy; fire_pressed = true; }
    
    CHECK_FIRE('7', -1, -1)
    CHECK_FIRE('8',  0, -1)
    CHECK_FIRE('9',  1, -1)
    CHECK_FIRE('4', -1,  0)
    CHECK_FIRE('6',  1,  0)
    CHECK_FIRE('1', -1,  1)
    CHECK_FIRE('2',  0,  1)
    CHECK_FIRE('3',  1,  1)
    
    // 5 turns off autofire
    if (g_keys['5']) {
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
            spawn_laser(p->wx + SCREEN_TO_FIXED(sprite_player.width/2) - SCREEN_TO_FIXED(sprite_laser.width/2),
                        p->wy + SCREEN_TO_FIXED(sprite_player.height/2) - SCREEN_TO_FIXED(sprite_laser.height/2),
                        pvx, pvy, dir);
            fire_cooldown = FIRE_COOLDOWN;
        }
    }
    
    p->wx += p->vx; p->wy += p->vy;
    clamp_to_screen(&p->wx, &p->wy, p->sprite->width, p->sprite->height);
}

static void update_entities(void) {
    for (int16_t idx = g_list_head; idx != -1; ) {
        int16_t next_idx = g_next[idx]; Entity* e = &g_entities[idx];
        if (!e->active) { idx = next_idx; continue; }
        if (e->type == ENT_HULK && e->pushback_timer > 0) { e->pushback_timer--; e->wx += e->vx; e->wy += e->vy; clamp_to_screen(&e->wx, &e->wy, e->sprite->width, e->sprite->height); idx = next_idx; continue; }
        bool tick_ready = ((g_frame_count - e->tick_phase) % e->tick_period) == 0;
        if (tick_ready) {
            switch (e->type) {
                case ENT_HUMAN: ai_human(e); break; case ENT_GRUNT: ai_grunt(e); break;
                case ENT_SPHEROID: ai_spheroid(e); break; case ENT_ENFORCER: ai_enforcer(e); break;
                case ENT_HULK: ai_hulk(e); break;
                default: break;
            }
            e->wx += e->vx; e->wy += e->vy;
        }
        if (e->type == ENT_LASER || e->type == ENT_TERROR) {
            e->age++;
            if (e->wx < -SCREEN_TO_FIXED(e->sprite->width) || e->wx > SCREEN_TO_FIXED(SCREEN_WIDTH) || e->wy < -SCREEN_TO_FIXED(e->sprite->height) || e->wy > SCREEN_TO_FIXED(SCREEN_HEIGHT)) {
                if (e->type == ENT_LASER) g_laser_count--;
                remove_entity(idx); idx = next_idx; continue;
            }
        } else {
            clamp_to_screen(&e->wx, &e->wy, e->sprite->width, e->sprite->height);
            int new_dir = get_dir_from_vel(e->vx, e->vy);
            if (new_dir != DIR_NONE) { e->move_dir = new_dir; e->facing_dir = new_dir; }
        }
        e->anim_frame = (e->anim_frame + 1) % 2; idx = next_idx;
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
    int points = (g_rescue_count <= 5) ? (g_rescue_count * 1000) : 5000;
    spawn_decal(DECAL_SCORE_BONUS, e->wx, e->wy, 60, points, DECAL_LAYER_OVERLAY);
    g_remove_enemy[g_grid_nodes[nidx].entity_idx] = true; 
    g_rescue_count++; 
    g_score += points; 
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
                        clamp_to_screen(&b->wx, &b->wy, b->sprite->width, b->sprite->height);
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
    float tw = (float)p->sprite->width * g_term_cols / SCREEN_WIDTH;
    float th = (float)p->sprite->height * g_term_rows / SCREEN_HEIGHT;
    uint8_t cr = p->sprite->r, cg = p->sprite->g, cb = p->sprite->b;
    if (p->invulnerable_timer > 0 && (g_frame_count/6)%2==0) { cr=0; cg=255; cb=255; }
    if (p->death_timer > 0) { cr = 255; cg = 0; cb = 0; }
    draw_text_rect(tx, ty, tw, th, cr, cg, cb);
}

static void draw_entity_text(Entity* e) {
    float raw_tx = ((float)e->wx / COORD_SCALE) * g_term_cols / SCREEN_WIDTH;
    float raw_ty = ((float)e->wy / COORD_SCALE) * g_term_rows / SCREEN_HEIGHT;
    float tx = roundf(raw_tx * 2.0f) / 2.0f;
    float ty = roundf(raw_ty * 2.0f) / 2.0f;
    float tw = (float)e->sprite->width * g_term_cols / SCREEN_WIDTH;
    float th = (float)e->sprite->height * g_term_rows / SCREEN_HEIGHT;
    uint8_t cr = e->sprite->r, cg = e->sprite->g, cb = e->sprite->b;
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

    char score_str[8];
    snprintf(score_str, sizeof(score_str), "%07d", g_score);

    for (int i = 0; i < 7; i++) {
        int digit = score_str[i] - '0';
        int col_offset = i * 3;  // Each digit is 3 cols wide

        for (int row = 0; row < 3; row++) {
            for (int col = 0; col < 3; col++) {
                int buf_idx = row * g_term_cols + (col_offset + col);
                if (buf_idx >= 0 && buf_idx < g_term_rows * g_term_cols) {
                    
                    // Safely copy the single-character string (max 3 bytes + '\0')
                    snprintf(text_buffer[buf_idx].glyph, 4, "%s", 
                             digit_sprites[digit][row * 3 + col]);
                    
                    text_buffer[buf_idx].r = 0;    // Green
                    text_buffer[buf_idx].g = 255;
                    text_buffer[buf_idx].b = 0;
                }
            }
        }
    }
}



//
//------------RENDER ALL------------------------------
//
// Update render_all() to call draw_score_text()
static void render_all(void) {
    // 1. Clear the offscreen text buffer
    clear_text_buffer();
    
    // 2. Draw FLOOR layer decals (under entities, e.g., squished human 'X's)
    draw_floor_decals();
    
    // 3. Draw Player (with half-step quantization)
    if (g_players[0].active || g_players[0].death_timer > 0) {
        float raw_tx = ((float)g_players[0].wx / COORD_SCALE) * g_term_cols / SCREEN_WIDTH;
        float raw_ty = ((float)g_players[0].wy / COORD_SCALE) * g_term_rows / SCREEN_HEIGHT;
        float tx = roundf(raw_tx * 2.0f) / 2.0f;
        float ty = roundf(raw_ty * 2.0f) / 2.0f;
        float tw = (float)g_players[0].sprite->width * g_term_cols / SCREEN_WIDTH;
        float th = (float)g_players[0].sprite->height * g_term_rows / SCREEN_HEIGHT;
        
        uint8_t cr = 255, cg = 255, cb = 255;
        if (g_players[0].invulnerable_timer > 0 && (g_frame_count/6)%2==0) { 
            cr=0; cg=255; cb=255; 
        }
        if (g_players[0].death_timer > 0) { 
            cr = 255; cg = 0; cb = 0; 
        }
        draw_text_rect(tx, ty, tw, th, cr, cg, cb);
    }
    
    // 4. Draw Entities (with half-step quantization)
    for (int16_t idx = g_list_head; idx != -1; idx = g_next[idx]) {
        Entity* e = &g_entities[idx];
        if (!e->active) continue;
        
        float raw_tx = ((float)e->wx / COORD_SCALE) * g_term_cols / SCREEN_WIDTH;
        float raw_ty = ((float)e->wy / COORD_SCALE) * g_term_rows / SCREEN_HEIGHT;
        float tx = roundf(raw_tx * 2.0f) / 2.0f;
        float ty = roundf(raw_ty * 2.0f) / 2.0f;
        float tw = (float)e->sprite->width * g_term_cols / SCREEN_WIDTH;
        float th = (float)e->sprite->height * g_term_rows / SCREEN_HEIGHT;
        
        uint8_t cr = e->sprite->r, cg = e->sprite->g, cb = e->sprite->b;
        if (e->type == ENT_HUMAN) {
            static const uint8_t hc[3][3] = {{255,100,200},{100,200,255},{255,255,100}};
            int ht = e->human_type; 
            if (ht<0||ht>2) ht=0;
            cr=hc[ht][0]; cg=hc[ht][1]; cb=hc[ht][2];
        }
        draw_text_rect(tx, ty, tw, th, cr, cg, cb);
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
    printf("%s%s", SHOW_CURSOR, RESET_STR);
    fflush(stdout);
}

// ============================================================
//  MAIN
// ============================================================
int main(void) {
    setlocale(LC_ALL, "en_US.utf8");
    srand((unsigned)time(NULL));
    
    init_terminal_size();
    init_text_buffer();
    init_text_mode();
    
    printf("\x1b[2J\x1b[H");
    printf("\x1b[38;2;255;255;0m");
    printf("  BLOPOTRON 2024\n\n");
    printf("\x1b[38;2;255;255;255m");
    printf("  MOVE: w/a/d/x  DIAGONALS: q/e/z/c  STOP: s\n");
    printf("  FIRE: numpad 7-9/4/6/1-3 (autofire)  5=stop fire\n");
    printf("  INSERT COIN (Press '1' or ENTER)\n");
    printf("  Press ESC to quit\n");
    fflush(stdout);

    int waiting = 1;
    while (waiting) {
        fd_set set;
        struct timeval timeout = {0, 50000};
        FD_ZERO(&set);
        FD_SET(STDIN_FILENO, &set);
        if (select(STDIN_FILENO + 1, &set, NULL, NULL, &timeout) > 0) {
            char ch;
            if (read(STDIN_FILENO, &ch, 1) == 1) {
                if (ch == '1' || ch == '\n' || ch == '\r') waiting = 0;
                else if (ch == 27) goto cleanup;
            }
        }
    }


    play_level_intro_text();
    restart_game(1);

    // 3. Gameplay Loop
    printf("\x1b[2J\x1b[H");
    int running = 1;
    while (running) {
        poll_input(); // Using your g_keys[] polling
        if (g_keys[27]) running = 0;
        
        update_all(); // <-- No arguments
        render_all(); // <-- No arguments
        
        usleep(16000); 
    }



cleanup:
    // 1. Restore terminal to normal canonical mode first
    fini_text_mode();
    
    // 2. Reset colors and show cursor (but DO NOT clear the screen)
    printf("\x1b[0m\x1b[?25h");
    
    // 3. Print the final score persistently to stdout
    printf("\n\n  YOUR SCORE: %07d\n\n", g_score);
    
    // 4. Clean up the text buffer
    fini_text_buffer();
    
    return 0;
}
}
