/*
 * ============================================================
 *  build_sprites7.c  --  ANSI .ans  ->  sprites.h  converter
 * ============================================================
 *
 * Conforms to the sprite loader in btg.c / bth.c:
 *
 *   - Emits SpriteFrame / FacingInfo / SpriteSet structures
 *     (those structs are already typedef'd in btg.c -- this tool
 *      does NOT redefine them, so the generated header can be
 *      #include'd directly without conflict).
 *
 *   - Per-row uint8_t arrays: 10 bytes per cell
 *       glyph[4]  +  fr fg fb  +  br bg bb
 *
 *   - Auto-toggle in btg.c checks `frames[0].w > 1` to detect
 *     real sprite data vs. the 1x1 stubs.  All sprites emitted
 *     by this tool have w >= 2 (assuming the .ans art is at
 *     least 2 cells wide), so the toggle activates automatically.
 *
 *  Character-name -> entity-type mapping is CASE-INSENSITIVE
 *  and PLURAL-AWARE.  Examples that all map to sprite_grunt_set
 *  / ENT_GRUNT:
 *
 *      "grunt"   "Grunt"   "GRUNT"   "grunts"  "Grunts"  "GRUNTS"
 *
 *  Same rule applies to:  player, quark, hulk, brain, spheroid,
 *  enforcer, human, laser, terror, electrode, cruise.
 *
 *  MERGE BEHAVIOUR
 *  ---------------
 *  If <existing_sprites.h> exists (default: "sprites.h" in CWD),
 *  the tool parses it for previously-generated sprite blocks
 *  (delimited by  slash-star === <name> === star-slash  markers)
 *  and retains them.  New sprites from <input.ans> are added; if
 *  a name clashes with an existing block, the NEW data overwrites
 *  the old block.  Non-clashing existing blocks are preserved
 *  verbatim.
 *
 *  USAGE
 *  -----
 *      build_sprites7 <input.ans> <output.h> [existing_sprites.h]
 *
 *  If the third argument is omitted, defaults to "sprites.h".
 *
 *  BUILD
 *  -----
 *      gcc -O2 -Wall build_sprites7.c -o build_sprites7
 *
 *  INTEGRATION WITH btg.c / bth.c
 *  ------------------------------
 *  After generating sprites.h, the user must remove (or wrap in
 *  an ifdef guard) the stub definitions in btg.c:
 *      - stub_cell_blank, stub_frame_rows, stub_frames[]
 *      - sprite_*_set  definitions
 *  See README-sprites.md for full integration steps.
 *
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdbool.h>

/* ---------- Limits ---------- */
#define MAX_LINE            8192
#define MAX_NAME            32
#define MAX_FRAMES          64
#define MAX_ROWS            64
#define MAX_COLS            64
#define MAX_SPRITES         32
#define MAX_EXISTING_BLOCK  (256 * 1024)

/* ---------- Entity-type table ----------
 * Order follows the canonical game ordering:
 *   Player, Grunt, Quark, Hulk, Brain, Spheroid, Enforcer,
 *   Human, Mommy, Daddy, Mikey, Laser, Terror, Electrode,
 *   Cruise, Tank
 *
 * HUMAN SUB-VARIANTS (Mommy, Daddy, Mikey):
 *   These three are "themes" on the single ENT_HUMAN entity type.
 *   The game uses Entity.human_type (0/1/2) to pick which
 *   sub-variant's SpriteSet to bind at spawn time.  Each sub-variant
 *   has its own .ans file (e.g. ENT_HUMAN_Daddy.ans) and its own
 *   SpriteSet symbol (sprite_mommy_set, sprite_daddy_set,
 *   sprite_mikey_set).
 *
 *   The legacy "human" entry is retained for backward compatibility
 *   -- if a user generates only ENT_HUMAN.ans, the build still
 *   succeeds and produces sprite_human_set.  The game no longer
 *   consults sprite_human_set at runtime (it always picks one of the
 *   three sub-variants), but the symbol exists so older references
 *   don't break the link.
 *
 * The cident field is the lowercase C-identifier prefix used
 * in symbol names (e.g.  sprite_grunt_set, grunt_f0_r0).
 * The display field is the human-readable name put in the
 * SpriteSet's .name string.
 * step_period is the per-facing animation tick count
 * (matches the recommended values in sprites.h's doc comment).
 */
typedef struct {
    const char* name;       /* canonical lowercase singular */
    const char* cident;     /* C identifier prefix           */
    const char* display;    /* display name string           */
    int         step_period;/* default animation step        */
} EntityMap;

static const EntityMap ENTITY_MAP[] = {
    { "player",    "player",    "Player",    1 },
    { "grunt",     "grunt",     "Grunt",     8 },
    { "quark",     "quark",     "Quark",     4 },
    { "hulk",      "hulk",      "Hulk",      4 },
    { "brain",     "brain",     "Brain",     4 },
    { "spheroid",  "spheroid",  "Spheroid",  2 },
    { "enforcer",  "enforcer",  "Enforcer",  2 },
    { "human",     "human",     "Human",     4 },
    { "mommy",     "mommy",     "Mommy",     4 },
    { "daddy",     "daddy",     "Daddy",     4 },
    { "mikey",     "mikey",     "Mikey",     4 },
    { "laser",     "laser",     "Laser",     1 },
    { "terror",    "terror",    "Terror",    1 },
    { "electrode", "electrode", "Electrode", 1 },
    { "cruise",    "cruise",    "Cruise",    1 },
    { "tank",      "tank",      "Tank",      4 },
};
#define NUM_ENTITY_MAP  (int)(sizeof(ENTITY_MAP) / sizeof(ENTITY_MAP[0]))

/* Facing codes (must be before Sprite struct which uses NUM_FACE_SLOTS).
 * Array index constants (0-3) match Direction enum: N=0,S=1,W=2,E=3.
 * Bitmask constants for multi-direction Facing: tags in .ans files.
 *   NOTE: W and E are swapped in array index vs. compass order to match
 *   DIR_LEFT=2 (W), DIR_RIGHT=3 (E) in the game's Direction enum. */
#define FACE_N_IDX 0
#define FACE_S_IDX 1
#define FACE_W_IDX 2
#define FACE_E_IDX 3
#define FACE_WH_IDX 4
#define FACE_EH_IDX 5
#define NUM_FACE_SLOTS 6

#define FACE_NONE 0
#define FACE_N_BIT 1   /* bit 0 */
#define FACE_S_BIT 2   /* bit 1 */
#define FACE_W_BIT 4   /* bit 2 */
#define FACE_E_BIT 8   /* bit 3 */
#define FACE_WH_BIT 16  /* bit 4 -- W at odd row */
#define FACE_EH_BIT 32  /* bit 5 -- E at odd row */

static const int FACE_BIT[NUM_FACE_SLOTS] = { FACE_N_BIT, FACE_S_BIT, FACE_W_BIT, FACE_E_BIT, FACE_WH_BIT, FACE_EH_BIT };
static const char* FACE_SUFFIX[NUM_FACE_SLOTS] = { "n", "s", "w", "e", "wh", "eh" };
static const char* FACE_NAME[NUM_FACE_SLOTS]   = { "N", "S", "W", "E", "WH", "EH" };

/* ---------- Parsed sprite data ---------- */
typedef struct {
    uint8_t glyph[4];   /* UTF-8 bytes, null-padded                */
    uint8_t fr, fg, fb; /* foreground RGB                          */
    uint8_t br, bg, bb; /* background RGB                          */
} Cell;

typedef struct {
    Cell rows[MAX_ROWS][MAX_COLS];
    int  w, h;          /* detected dimensions                    */
    int  ox, oy;        /* offset (from OffsetX/Y)                */
    int  hx, hy;        /* hotspot  (from HotspotX/Y)             */
    int  frame_no;      /* informational FrameNo: field           */
    int  facing;        /* bitmask: FACE_N=1, FACE_S=2, FACE_E=4, FACE_W=8 */
} Frame;


typedef struct {
    const EntityMap* map;
    Frame  frames[MAX_FRAMES];
    int    frame_count;
    /* Walk tables: one per facing direction, parsed from WalkTable: sections.
     * If a direction has no WalkTable: in the .ans source, walk_count[d] stays 0.
     * After all .ans files are parsed, empty WH is filled from W, empty EH from E. */
    int    walk_table[NUM_FACE_SLOTS][MAX_FRAMES];
    int    walk_count[NUM_FACE_SLOTS];
} Sprite;

static Sprite g_sprites[MAX_SPRITES];
static int    g_sprite_count = 0;

/* Parse a Facing: field value ('N'/'S'/'E'/'W', case-insensitive) ->
 * FACE_N/S/E/W; anything else returns FACE_NONE. */
static int parse_facing_token(const char* s) {
    /* Parse facing string into bitmask.
     * Supports compound multi-char tokens: "WHEH" = WH+EH, "NSWE" = N+S+W+E.
     * Also single tokens: "EH", "WH", or single-char "N","S","E","W".
     *
     * Strategy: greedy scan -- consume "WH" or "EH" when the next two chars
     * match, otherwise consume one char at a time ('N','S','E','W').
     * Unknown chars are silently skipped.
     */
    int mask = FACE_NONE;
    while (*s) {
        char c0 = (char)toupper((unsigned char)s[0]);
        char c1 = (char)toupper((unsigned char)s[1]);
        if (c0 == 'W' && c1 == 'H') { mask |= FACE_WH_BIT; s += 2; }
        else if (c0 == 'E' && c1 == 'H') { mask |= FACE_EH_BIT; s += 2; }
        else {
            switch (c0) {
                case 'N': mask |= FACE_N_BIT; break;
                case 'S': mask |= FACE_S_BIT; break;
                case 'E': mask |= FACE_E_BIT; break;
                case 'W': mask |= FACE_W_BIT; break;
            }
            s++;
        }
    }
    return mask;
}

/* ---------- Existing-block cache (for merge) ---------- */
typedef struct {
    char  name[MAX_NAME];   /* lowercase entity name               */
    char* text;             /* malloc'd block text (incl. marker)   */
    size_t len;
} ExistingBlock;

static ExistingBlock g_existing[MAX_SPRITES];
static int           g_existing_count = 0;

/* ============================================================
 *  Case-insensitive, plural-aware name resolution
 * ============================================================
 *  Input examples that all resolve to ENT_GRUNT:
 *    "Grunt", "grunt", "GRUNT", "Grunts", "grunts", "GRUNTS"
 *
 *  Algorithm:
 *    1. lowercase, strip spaces/underscores/hyphens
 *    2. exact match against canonical names
 *    3. if input ends in 's', strip it and try again (handles
 *       "grunts" -> "grunt", "electrodes" -> "electrode",
 *       "cruises" -> "cruise", etc.)
 */
static const EntityMap* resolve_entity(const char* raw_name)
{
    char buf[MAX_NAME];
    int  n = 0;
    for (int i = 0; raw_name[i] && n < MAX_NAME - 1; i++) {
        char c = raw_name[i];
        if (c == ' ' || c == '_' || c == '-') continue;
        buf[n++] = (char)tolower((unsigned char)c);
    }
    buf[n] = '\0';

    /* Pass 1: exact match */
    for (int i = 0; i < NUM_ENTITY_MAP; i++)
        if (strcmp(buf, ENTITY_MAP[i].name) == 0)
            return &ENTITY_MAP[i];

    /* Pass 2: strip trailing 's' (plural) and retry */
    if (n > 1 && buf[n - 1] == 's') {
        buf[n - 1] = '\0';
        for (int i = 0; i < NUM_ENTITY_MAP; i++)
            if (strcmp(buf, ENTITY_MAP[i].name) == 0)
                return &ENTITY_MAP[i];
    }

    return NULL;
}

/* ============================================================
 *  ANSI row parser
 * ============================================================
 *  Walks one line of ANSI-art text and emits a flat row of Cells.
 *  Handles BOTH proper ANSI escapes ("\x1b[38;2;R;G;Bm") and
 *  "mangled" escapes where the ESC byte has been stripped
 *  ("[38;2;R;G;Bm").  The .ans files produced by common ANSI
 *  editors often lack the ESC byte, so we must cope with both.
 *
 *  SGR codes recognised:
 *    38;2;R;G;Bm   set foreground RGB
 *    48;2;R;G;Bm   set background RGB
 *    0m  (or empty m)  reset to default (fg=white, bg=black)
 *
 *  All other SGR codes (bold, underline, etc.) are silently
 *  ignored -- they don't affect the cell payload.
 *
 *  Returns the number of cells emitted (0 if the line has no
 *  printable characters).
 */
static int parse_ansi_row(const char* line, Cell* out, int max_cells)
{
    int      n = 0;
    uint8_t  fr = 255, fg = 255, fb = 255;   /* default fg: white */
    uint8_t  br = 0,   bg = 0,   bb = 0;     /* default bg: black */
    const char* p = line;

    while (*p && n < max_cells) {
        /* ---- Detect ANSI escape: \x1b[... or [... (mangled) ---- */
        const char* seq = NULL;
        if ((unsigned char)*p == 0x1B && *(p + 1) == '[') {
            seq = p + 2;
            p  += 2;
        } else if (*p == '[' && isdigit((unsigned char)*(p + 1))) {
            seq = p + 1;
            p  += 1;
        }

        if (seq) {
            /* Parse SGR params until 'm' */
            int  params[16];
            int  nparam = 0;
            int  cur = 0;
            bool have_cur = false;
            while (*p && *p != 'm' && *p != '\n' && *p != '\r') {
                if (*p == ';') {
                    if (nparam < 16) params[nparam++] = cur;
                    cur = 0;
                    have_cur = false;
                } else if (isdigit((unsigned char)*p)) {
                    cur = cur * 10 + (*p - '0');
                    have_cur = true;
                }
                p++;
            }
            if (*p == 'm') p++;
            if (have_cur && nparam < 16) params[nparam++] = cur;

            /* Interpret SGR */
            if (nparam == 0) {
                fr = 255; fg = 255; fb = 255;
                br = 0;   bg = 0;   bb = 0;
            } else if (nparam >= 5 && params[0] == 38 && params[1] == 2) {
                fr = (uint8_t)params[2];
                fg = (uint8_t)params[3];
                fb = (uint8_t)params[4];
            } else if (nparam >= 5 && params[0] == 48 && params[1] == 2) {
                br = (uint8_t)params[2];
                bg = (uint8_t)params[3];
                bb = (uint8_t)params[4];
            } else if (nparam == 1 && params[0] == 0) {
                fr = 255; fg = 255; fb = 255;
                br = 0;   bg = 0;   bb = 0;
            }
            /* Other SGR codes ignored */
            continue;
        }

        /* ---- Skip CR/LF ---- */
        if (*p == '\r' || *p == '\n') { p++; continue; }

        /* ---- Printable char: read UTF-8 sequence ---- */
        int nbytes = 1;
        if      ((*p & 0x80) == 0x00) nbytes = 1;   /* ASCII       */
        else if ((*p & 0xE0) == 0xC0) nbytes = 2;   /* 2-byte UTF-8*/
        else if ((*p & 0xF0) == 0xE0) nbytes = 3;   /* 3-byte UTF-8*/
        else if ((*p & 0xF8) == 0xF0) nbytes = 4;   /* 4-byte UTF-8*/

        Cell* c = &out[n++];
        memset(c, 0, sizeof(Cell));
        for (int i = 0; i < 4; i++) {
            if (i < nbytes && *p && *p != '\r' && *p != '\n')
                c->glyph[i] = (uint8_t)*p++;
            else
                c->glyph[i] = 0;
        }
        c->fr = fr; c->fg = fg; c->fb = fb;
        c->br = br; c->bg = bg; c->bb = bb;
    }

    return n;
}

/* Quick check: does this line look like ANSI-art data?
 * (Either a real ESC byte or a mangled '[' followed by a digit.)
 */
static bool is_ansi_row(const char* line)
{
    for (const char* p = line; *p; p++) {
        if ((unsigned char)*p == 0x1B && *(p + 1) == '[') return true;
        if (*p == '[' && isdigit((unsigned char)*(p + 1))) return true;
    }
    return false;
}

/* ============================================================
 *  Parse the .ans input file into g_sprites[]
 * ============================================================
 *  Recognised header fields (per frame):
 *      Index:       <n>      (source ID, informational)
 *      Character:   <name>   (REQUIRED -- maps to entity type)
 *      FrameNo:     <n>      (animation frame index, informational)
 *      OffsetX:     <n>      (placement offset X, in cells)
 *      OffsetY:     <n>      (placement offset Y, in cells)
 *      HotspotX:    <n>      (anchor X, in cells)
 *      HotspotY:    <n>      (anchor Y, in cells)
 *      Facing:      <dir>    (N/S/E/W; if absent, frames share
 *                             across all 4 facings)
 *
 *  A blank line separates the header block from the sprite-row
 *  data.  A frame ends when the next "Index:" or "Character:"
 *  line appears, or at EOF.
 *
 *  Index: may appear before Character: -- we defer frame
 *  creation until Character: is seen.
 */
static void parse_ans(const char* filename)
{
    FILE* f = fopen(filename, "r");
    if (!f) { perror("fopen ans"); exit(1); }

    char     line[MAX_LINE];
    Sprite*  cur_sprite = NULL;
    Sprite*  last_sprite = NULL;  /* most recent sprite for WalkTable parsing */
    Frame*   cur_frame  = NULL;
    bool     in_data    = false;

    /* Walk table parsing state */
    int      wt_dir_idx = -1;   /* index into FACE_NAME[], or -1 */

    /* Deferred frame metadata (collected between Index: and Character:) */
    int  pending_ox = 0, pending_oy = 0;
    int  pending_hx = 0, pending_hy = 0;
    int  pending_fno = 0;
    int  pending_facing = FACE_NONE;
    bool have_pending = false;

    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';

        if (strncmp(line, "Index:", 6) == 0) {
            /* Finalise current frame (already attached) and start
             * collecting metadata for the next one. */
            cur_frame = NULL;
            cur_sprite = NULL;
            in_data = false;
            pending_ox = pending_oy = pending_hx = pending_hy = 0;
            pending_fno = 0;
            pending_facing = FACE_NONE;
            have_pending = true;
        }
        else if (strncmp(line, "Character:", 10) == 0) {
            char name[MAX_NAME];
            name[0] = '\0';
            sscanf(line, "Character: %31[^\r\n]", name);
            /* Trim trailing whitespace */
            int len = (int)strlen(name);
            while (len > 0 && isspace((unsigned char)name[len - 1]))
                name[--len] = '\0';
            if (len == 0) continue;

            const EntityMap* m = resolve_entity(name);
            if (!m) {
                fprintf(stderr,
                    "Warning: character '%s' does not map to any "
                    "entity type; skipping.\n", name);
                cur_sprite = NULL;
                cur_frame  = NULL;
                have_pending = false;
                continue;
            }

            /* Find or create the sprite for this entity type */
            cur_sprite = NULL;
            for (int i = 0; i < g_sprite_count; i++) {
                if (g_sprites[i].map == m) { cur_sprite = &g_sprites[i]; break; }
            }
            if (!cur_sprite) {
                if (g_sprite_count >= MAX_SPRITES) {
                    fprintf(stderr, "Error: too many sprites (max %d)\n",
                        MAX_SPRITES);
                    exit(1);
                }
                cur_sprite = &g_sprites[g_sprite_count++];
                cur_sprite->map = m;
                cur_sprite->frame_count = 0;
                memset(cur_sprite->walk_count, 0, sizeof(cur_sprite->walk_count));
            }

            /* Allocate a new frame on this sprite */
            if (cur_sprite->frame_count >= MAX_FRAMES) {
                fprintf(stderr,
                    "Warning: too many frames for %s (max %d); "
                    "ignoring extra frames.\n", m->name, MAX_FRAMES);
                cur_frame = NULL;
                continue;
            }
            cur_frame = &cur_sprite->frames[cur_sprite->frame_count++];
            last_sprite = cur_sprite;
            memset(cur_frame, 0, sizeof(Frame));
            cur_frame->facing = FACE_NONE;  /* default: no facing tag */
            if (have_pending) {
                cur_frame->ox = pending_ox;
                cur_frame->oy = pending_oy;
                cur_frame->hx = pending_hx;
                cur_frame->hy = pending_hy;
                cur_frame->frame_no = pending_fno;
                cur_frame->facing = pending_facing;
            }
            in_data = false;
            have_pending = false;
        }
        /* --- Walk table parsing (appears after all frame blocks) ---
         * MUST come before the cur_frame check: once "Character:" sets
         * cur_frame it is never cleared, so WalkTable: lines would be
         * swallowed by the cur_frame branch.  Seeing a WalkTable: line
         * also signals end-of-frames for the current sprite. */
        else if (strncmp(line, "WalkTable:", 10) == 0 && last_sprite) {
            cur_frame = NULL;   /* frame data is finished */
            /* Parse direction name after "WalkTable: " */
            const char* dir = line + 10;
            while (*dir == ' ') dir++;
            wt_dir_idx = -1;
            for (int d = 0; d < NUM_FACE_SLOTS; d++) {
                if (strcasecmp(dir, FACE_NAME[d]) == 0) {
                    wt_dir_idx = d;
                    break;
                }
            }
            if (wt_dir_idx >= 0) {
                /* Reset this direction's walk table (new data overwrites) */
                last_sprite->walk_count[wt_dir_idx] = 0;
            }
        }
        else if (wt_dir_idx >= 0 && last_sprite && line[0] != '\0') {
            /* Parse comma-separated frame indices for current walk direction */
            const char* p = line;
            while (*p) {
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '\0' || *p == ',' || *p == '#') break;
                int val = 0;
                if (sscanf(p, "%d", &val) == 1) {
                    int cnt = last_sprite->walk_count[wt_dir_idx];
                    if (cnt < MAX_FRAMES) {
                        last_sprite->walk_table[wt_dir_idx][cnt] = val;
                        last_sprite->walk_count[wt_dir_idx] = cnt + 1;
                    }
                }
                while (*p && *p != ',') p++;
                if (*p == ',') p++;
            }
            wt_dir_idx = -1;  /* only one data line per WalkTable: */
        }
        else if (cur_frame) {
            if (strncmp(line, "OffsetX:", 8) == 0)
                sscanf(line, "OffsetX: %d", &cur_frame->ox);
            else if (strncmp(line, "OffsetY:", 8) == 0)
                sscanf(line, "OffsetY: %d", &cur_frame->oy);
            else if (strncmp(line, "HotspotX:", 9) == 0)
                sscanf(line, "HotspotX: %d", &cur_frame->hx);
            else if (strncmp(line, "HotspotY:", 9) == 0)
                sscanf(line, "HotspotY: %d", &cur_frame->hy);
            else if (strncmp(line, "FrameNo:", 8) == 0)
                sscanf(line, "FrameNo: %d", &cur_frame->frame_no);
            else if (strncmp(line, "Facing:", 7) == 0) {
                const char* val = line + 7;
                cur_frame->facing = parse_facing_token(val);
            }
            else if (line[0] == '\0') {
                /* Blank line transitions from header to data */
                in_data = true;
            }
            else if (in_data && is_ansi_row(line)) {
                if (cur_frame->h >= MAX_ROWS) {
                    fprintf(stderr,
                        "Warning: too many rows in frame (max %d); "
                        "truncating.\n", MAX_ROWS);
                    continue;
                }
                Cell* row = cur_frame->rows[cur_frame->h];
                int   n   = parse_ansi_row(line, row, MAX_COLS);
                if (n > 0) {
                    if (n > cur_frame->w) cur_frame->w = n;
                    cur_frame->h++;
                }
            }
        }
        else if (have_pending) {
            /* Metadata appeared before Character: -- stash it. */
            if (strncmp(line, "OffsetX:", 8) == 0)
                sscanf(line, "OffsetX: %d", &pending_ox);
            else if (strncmp(line, "OffsetY:", 8) == 0)
                sscanf(line, "OffsetY: %d", &pending_oy);
            else if (strncmp(line, "HotspotX:", 9) == 0)
                sscanf(line, "HotspotX: %d", &pending_hx);
            else if (strncmp(line, "HotspotY:", 9) == 0)
                sscanf(line, "HotspotY: %d", &pending_hy);
            else if (strncmp(line, "FrameNo:", 8) == 0)
                sscanf(line, "FrameNo: %d", &pending_fno);
            else if (strncmp(line, "Facing:", 7) == 0) {
                const char* val = line + 7;
                pending_facing = parse_facing_token(val);
            }
        }
        else if (line[0] == '\0' || line[0] == '#') {
            wt_dir_idx = -1;  /* blank line or comment ends walk table parsing */
        }
    }
    fclose(f);
    for (int s = 0; s < g_sprite_count; s++) {
        Sprite* sp = &g_sprites[s];

        /* Detect which facings are present and whether they are
         * all-zeroed for FrameNo. */
        int face_first[MAX_FRAMES];   /* index of first frame per facing */
        int face_last[MAX_FRAMES];    /* index of last  frame per facing */
        int face_all_zero[4] = {1, 1, 1, 1};
        int face_has_any[4]  = {0, 0, 0, 0};

        memset(face_first, -1, sizeof(face_first));
        memset(face_last, -1, sizeof(face_last));

        for (int i = 0; i < sp->frame_count; i++) {
            int fc = sp->frames[i].facing;
            if (fc >= 0 && fc < 4) {
                if (!face_has_any[fc]) face_first[fc] = i;
                face_last[fc] = i;
                face_has_any[fc] = 1;
                if (sp->frames[i].frame_no != 0)
                    face_all_zero[fc] = 0;
            }
        }

        /* For each facing that is all-zeroed, renumber sequentially.
         * NOTE: face_has_any/face_all_zero have 4 entries (basic N/S/E/W),
         * so limit this loop to 4. */
        for (int f = 0; f < 4; f++) {
            if (!face_has_any[f] || !face_all_zero[f]) continue;
            for (int i = face_first[f]; i <= face_last[f]; i++) {
                if (sp->frames[i].facing & FACE_BIT[f]) {
                    sp->frames[i].frame_no =
                        i - face_first[f];
                }
            }
        }
    }
}

/* ============================================================
 *  Parse existing sprites.h to retain non-clashing blocks
 * ============================================================
 *  A sprite block is delimited by a line beginning (after
 *  optional whitespace) with a marker of the form:
 *
 *      slash-star === <name> === star-slash
 *
 *  (i.e. a C-style comment containing triple-equals signs and
 *  the sprite name).  The block extends from that marker up to
 *  (but not including) the next marker, or the #endif guard, or
 *  EOF.
 *
 *  We store the verbatim text of each block keyed by lowercased
 *  <name>.  At emit time, new sprites replace clashing entries;
 *  non-clashing entries are written back unchanged.
 */
static void parse_existing_sprites_h(const char* filename)
{
    FILE* f = fopen(filename, "r");
    if (!f) return;  /* No existing file -- fresh build */

    char  line[MAX_LINE];
    char* block = malloc(MAX_EXISTING_BLOCK);
    if (!block) { fclose(f); return; }
    size_t blen = 0;
    char   cur_name[MAX_NAME] = "";
    bool   in_block = false;

    while (fgets(line, sizeof(line), f)) {
        /* Detect block-start marker at start of line (after ws) */
        const char* p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (strncmp(p, "/* === ", 7) == 0) {
            const char* ns = p + 7;
            const char* ne = strstr(ns, " === */");
            if (ne) {
                size_t nl = (size_t)(ne - ns);
                if (nl < MAX_NAME) {
                    /* Finalise previous block if any */
                    if (in_block && g_existing_count < MAX_SPRITES) {
                        ExistingBlock* eb = &g_existing[g_existing_count++];
                        strncpy(eb->name, cur_name, MAX_NAME - 1);
                        eb->name[MAX_NAME - 1] = '\0';
                        eb->text = malloc(blen + 1);
                        memcpy(eb->text, block, blen);
                        eb->text[blen] = '\0';
                        eb->len = blen;
                    }
                    /* Start new block -- capture the marker line
                     * itself as the first line of the block text,
                     * so the block is self-delimiting when written
                     * back. */
                    memcpy(cur_name, ns, nl);
                    cur_name[nl] = '\0';
                    for (char* q = cur_name; *q; q++)
                        *q = (char)tolower((unsigned char)*q);
                    blen = 0;
                    /* Append the marker line to the block buffer */
                    {
                        size_t ll = strlen(line);
                        if (ll < MAX_EXISTING_BLOCK) {
                            memcpy(block + blen, line, ll);
                            blen += ll;
                        }
                    }
                    in_block = true;
                }
            }
        }
        else if (strncmp(p, "#endif", 6) == 0) {
            /* Finalise current block at end-of-file guard */
            if (in_block && g_existing_count < MAX_SPRITES) {
                ExistingBlock* eb = &g_existing[g_existing_count++];
                strncpy(eb->name, cur_name, MAX_NAME - 1);
                eb->name[MAX_NAME - 1] = '\0';
                eb->text = malloc(blen + 1);
                memcpy(eb->text, block, blen);
                eb->text[blen] = '\0';
                eb->len = blen;
            }
            in_block = false;
            blen = 0;
            cur_name[0] = '\0';
        }
        else if (in_block) {
            size_t ll = strlen(line);
            if (blen + ll < MAX_EXISTING_BLOCK) {
                memcpy(block + blen, line, ll);
                blen += ll;
            }
        }
    }

    /* If file had no #endif (truncated), finalise last block */
    if (in_block && g_existing_count < MAX_SPRITES) {
        ExistingBlock* eb = &g_existing[g_existing_count++];
        strncpy(eb->name, cur_name, MAX_NAME - 1);
        eb->name[MAX_NAME - 1] = '\0';
        eb->text = malloc(blen + 1);
        memcpy(eb->text, block, blen);
        eb->text[blen] = '\0';
        eb->len = blen;
    }

    free(block);
    fclose(f);
}

/* ============================================================
 *  Emit one sprite block (new data) to the output file
 * ============================================================
 *  Walk tables are read directly from WalkTable: sections in the
 *  .ans source.  No auto-derivation, auto-split, or fallback logic.
 *
 *  If WH or EH walk tables are empty in the .ans, they are
 *  pre-filled with W or E respectively (see post-parse step in
 *  main).  This means the game renders the same sprite for even
 *  and odd rows when the entity faces W or E.
 */
static void emit_sprite_block(FILE* fout, const Sprite* sp)
{
    const EntityMap* m  = sp->map;
    const char*  name   = m->cident;       /* e.g. "grunt"           */
    const char*  disp   = m->display;      /* e.g. "Grunt"           */
    int          step   = m->step_period;
    int          N      = sp->frame_count;

    fprintf(fout, "/* === %s === */\n", name);

    /* --- 1. Per-frame row arrays + SpriteFrame structs --- */
    for (int fi = 0; fi < N; fi++) {
        const Frame* fr = &sp->frames[fi];
        int w = fr->w;
        int h = fr->h;
        if (w <= 0 || h <= 0) {
            fprintf(stderr, "Warning: %s frame %d has empty dims "
                "(w=%d, h=%d); skipping.\n", name, fi, w, h);
            continue;
        }

        /* Per-row hex arrays */
        for (int r = 0; r < h; r++) {
            fprintf(fout,
                "static const uint8_t %s_f%d_r%d[%d] = {",
                name, fi, r, w * 10);
            for (int c = 0; c < w; c++) {
                const Cell* cell = &fr->rows[r][c];
                if (c % 2 == 0) fprintf(fout, "\n    ");
                fprintf(fout,
                    "0x%02x,0x%02x,0x%02x,0x%02x, "
                    "0x%02x,0x%02x,0x%02x, "
                    "0x%02x,0x%02x,0x%02x",
                    cell->glyph[0], cell->glyph[1],
                    cell->glyph[2], cell->glyph[3],
                    cell->fr, cell->fg, cell->fb,
                    cell->br, cell->bg, cell->bb);
                if (c < w - 1) fprintf(fout, ",");
            }
            fprintf(fout, "\n};\n");
        }

        /* Row pointer array */
        fprintf(fout,
            "static const uint8_t* const %s_f%d_rows[] = {",
            name, fi);
        for (int r = 0; r < h; r++) {
            if (r % 4 == 0) fprintf(fout, "\n    ");
            fprintf(fout, "%s_f%d_r%d%s",
                name, fi, r, (r < h - 1) ? "," : "");
        }
        fprintf(fout, "\n};\n");
    }

    /* --- 2. Flat frames[] array (all frames, in source order) --- */
    fprintf(fout, "static SpriteFrame %s_frames[] = {\n", name);
    for (int fi = 0; fi < N; fi++) {
        const Frame* fr = &sp->frames[fi];
        int w = fr->w;
        int h = fr->h;
        int ox_total = fr->ox + fr->hx;
        int oy_total = fr->oy + fr->hy;
        if (w <= 0 || h <= 0) {
            fprintf(fout, "    { NULL, 0, 0, 0, 0 }%s\n",
                (fi < N - 1) ? "," : "");
        } else {
            fprintf(fout, "    { %s_f%d_rows, %d, %d, %d, %d }%s\n",
                name, fi, w, h, ox_total, oy_total,
                (fi < N - 1) ? "," : "");
        }
    }
    fprintf(fout, "};\n\n");

    /* --- 3. Emit per-facing walk-cycle tables --- */
    fprintf(fout, "/* --- %s walk-cycle tables ---\n", disp);
    fprintf(fout, " * Each entry is an index into %s_frames[].\n", name);
    fprintf(fout, " * Read from WalkTable: directives in the .ans source.\n");
    fprintf(fout, " */\n");

    for (int f = 0; f < NUM_FACE_SLOTS; f++) {
        int count = sp->walk_count[f];
        const int* indices = sp->walk_table[f];
        fprintf(fout, "static const int %s_walk_%s[] = {",
            name, FACE_SUFFIX[f]);
        for (int i = 0; i < count; i++) {
            if (i % 8 == 0) fprintf(fout, "\n    ");
            fprintf(fout, "%d%s", indices[i], (i < count - 1) ? "," : "");
        }
        fprintf(fout, "\n};\n");
    }

    /* --- 4. Emit SpriteSet with FacingInfo --- */
    fprintf(fout, "\nstatic SpriteSet sprite_%s_set = {\n", name);
    fprintf(fout, "    \"%s\", %d, %s_frames,\n", disp, N, name);
    fprintf(fout, "    { /* N, S, W, E, WH, EH */\n");
    for (int f = 0; f < NUM_FACE_SLOTS; f++) {
        int count = sp->walk_count[f];
        fprintf(fout,
            "        { %s_walk_%s, %d, %d, 0, 0, %d, %d }%s /* %s */\n",
            name, FACE_SUFFIX[f], count, step,
            step,
            step,
            (f < NUM_FACE_SLOTS - 1) ? "," : "",
            FACE_NAME[f]);
    }
    fprintf(fout, "    }\n");
    fprintf(fout, "};\n\n");
}

/* ============================================================
 *  Top-level emit: write the full sprites.h
 * ============================================================
 *  Iterates entity types in canonical order (Player first,
 *  Cruise last).  For each, prefers new data from g_sprites[];
 *  falls back to the existing block cache if no new data; skips
 *  the entity entirely if neither is present.
 */
static void emit_sprites_h(const char* outfile)
{
    FILE* fout = fopen(outfile, "w");
    if (!fout) { perror("fopen output"); exit(1); }

    fprintf(fout,
        "/* ============================================================\n"
        " *  sprites.h  --  Generated sprite data for Blapotron\n"
        " * ============================================================\n"
        " *\n"
        " * Auto-generated by build_sprites7.c. DO NOT EDIT MANUALLY.\n"
        " *\n"
        " * Format conforms to the sprite loader in btg.c / bth.c:\n"
        " *   - SpriteFrame / FacingInfo / SpriteSet structs\n"
        " *     (typedef'd in btg.c -- this header does NOT redefine)\n"
        " *   - Per-row uint8_t arrays: 10 bytes per cell\n"
        " *       glyph[4]  +  fr fg fb  +  br bg bb\n"
        " *   - Auto-toggle: real sprites have frames[0].w > 1\n"
        " *\n"
        " * Merge behaviour: re-running build_sprites7.c with new\n"
        " * .ans input preserves existing sprite blocks; entity-name\n"
        " * clashes are replaced with the new data.\n"
        " *\n"
        " * INTEGRATION:\n"
        " *   1. Place this file alongside btg.c\n"
        " *   2. Add  #include \"sprites.h\"  near the top of btg.c\n"
        " *   3. Remove (or #ifdef out) the stub definitions in btg.c:\n"
        " *        - stub_cell_blank, stub_frame_rows, stub_frames[]\n"
        " *        - sprite_*_set  definitions\n"
        " *   4. The auto-toggle in render_all() will detect\n"
        " *      frames[0].w > 1 and switch from colored-rect\n"
        " *      fallbacks to per-cell sprite rendering.\n"
        " * ============================================================\n"
        " */\n");
    fprintf(fout, "#ifndef SPRITES_H\n#define SPRITES_H\n\n");

    for (int i = 0; i < NUM_ENTITY_MAP; i++) {
        const EntityMap* m = &ENTITY_MAP[i];

        /* Look for new sprite data */
        Sprite* sp = NULL;
        for (int j = 0; j < g_sprite_count; j++) {
            if (g_sprites[j].map == m) { sp = &g_sprites[j]; break; }
        }

        /* Look for existing block */
        ExistingBlock* eb = NULL;
        for (int j = 0; j < g_existing_count; j++) {
            if (strcmp(g_existing[j].name, m->name) == 0) {
                eb = &g_existing[j];
                break;
            }
        }

        if (sp) {
            if (eb) {
                fprintf(stderr,
                    "Note: overwriting existing '%s' block with "
                    "new data from .ans.\n", m->name);
            }
            emit_sprite_block(fout, sp);
        }
        else if (eb) {
            /* Preserve existing block verbatim */
            fwrite(eb->text, 1, eb->len, fout);
            fprintf(fout, "\n");
        }
        /* else: no sprite for this entity -- skip silently */
    }

    fprintf(fout, "#endif /* SPRITES_H */\n");
    fclose(fout);
}

/* ============================================================
 *  main
 * ============================================================ */
int main(int argc, char** argv)
{
    const char* out_file  = NULL;
    const char* exist_file = "sprites.h";

    if (argc < 2) {
        fprintf(stderr,
            "Usage: %s <output.h> <input1.ans> [input2.ans ...]\n"
            "\n"
            "  output.h       Generated sprites.h path.\n"
            "  input*.ans     One or more ANSI text-art sprite sources.\n"
            "\n"
            "  If 'sprites.h' exists in CWD, it is auto-loaded as\n"
            "  the merge source (preserving blocks not overwritten by\n"
            "  new .ans data).  If absent, a fresh sprites.h is built.\n"
            "\n"
            "Character names in .ans files are matched case-"
            "insensitively and plural-awarely to the 12+ entity\n"
            "types (player, grunt, quark, hulk, brain, spheroid,\n"
            "enforcer, human/mommy/daddy/mikey, laser, terror,\n"
            "electrode, cruise, tank).\n",
            argv[0]);
        return 1;
    }

    out_file = argv[1];

    /* Step 1: load existing sprites.h if present (for merge).
     * We probe the output file first (if it already exists), then
     * fall back to "sprites.h" in CWD.  Both are silent on miss. */
    const char* merge_candidates[2] = { out_file, "sprites.h" };
    for (int mc = 0; mc < 2; mc++) {
        FILE* probe = fopen(merge_candidates[mc], "r");
        if (probe) {
            fclose(probe);
            exist_file = merge_candidates[mc];
            parse_existing_sprites_h(exist_file);
            if (g_existing_count > 0) {
                fprintf(stderr,
                    "Loaded %d existing sprite block(s) from '%s'.\n",
                    g_existing_count, exist_file);
            }
            break;
        }
    }

    /* Step 2: parse each .ans input (argv[2..]) */
    int total_input = 0;
    for (int i = 2; i < argc; i++) {
        parse_ans(argv[i]);
        total_input++;
    }
    if (g_sprite_count == 0) {
        fprintf(stderr, "Error: no sprites parsed from %d input file(s).\n",
            total_input);
        return 1;
    }

    /* Step 2.5: diagnose walk tables */
    for (int s = 0; s < g_sprite_count; s++) {
        Sprite* sp = &g_sprites[s];
        fprintf(stderr, "  %s walk:\n", sp->map->display);
        for (int d = 0; d < NUM_FACE_SLOTS; d++) {
            fprintf(stderr, "    %s: count=%d", FACE_NAME[d],
                        sp->walk_count[d]);
        }
    }
    /* Step 3: If WH/EH walk tables are missing, fill from W/E.
     * Empty WH/EH means the .ans did not define them, so
     * the entity does not distinguish even/odd rows when facing
     * W or E.  We copy W→WH and E→EH so the game always has
     * a valid walk table for all 6 directions. */
    for (int s = 0; s < g_sprite_count; s++) {
        Sprite* sp = &g_sprites[s];
        if (sp->walk_count[FACE_WH_IDX] == 0 && sp->walk_count[FACE_W_IDX] > 0) {
            memcpy(sp->walk_table[FACE_WH_IDX],
                   sp->walk_table[FACE_W_IDX],
                   sp->walk_count[FACE_W_IDX] * sizeof(int));
            sp->walk_count[FACE_WH_IDX] = sp->walk_count[FACE_W_IDX];
        }
        if (sp->walk_count[FACE_EH_IDX] == 0 && sp->walk_count[FACE_E_IDX] > 0) {
            memcpy(sp->walk_table[FACE_EH_IDX],
                   sp->walk_table[FACE_E_IDX],
                   sp->walk_count[FACE_E_IDX] * sizeof(int));
            sp->walk_count[FACE_EH_IDX] = sp->walk_count[FACE_E_IDX];
        }
    }

    /* Step 4: emit the merged sprites.h */
    emit_sprites_h(out_file);
    fprintf(stderr, "Wrote '%s'.\n", out_file);

    return 0;
}
