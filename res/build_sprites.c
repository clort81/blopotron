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
 *  MULTI-DIRECTION FACING TAGS
 *  ---------------------------
 *  The "Facing:" field may list ONE OR MORE directions in any
 *  order, case-insensitive, with or without separators:
 *
 *      Facing: N        -> N only
 *      Facing: NS        -> both N and S  (frame index appears in
 *                                       walk_n[] AND walk_s[])
 *      Facing: SN        -> same as NS
 *      Facing: NSEW      -> all four
 *      Facing: N S       -> N+S (spaces ok)
 *
 *  Internally each Frame carries a 4-bit mask (FACE_N_BIT etc.).
 *  The auto-grouper ORs tagged frames into every named facing's
 *  walk-cycle table, in source order.  Facings with no tagged
 *  frames fall back to ALL frames in source order (legacy
 *  default); frames with no Facing: tag at all are skipped by
 *  the auto-grouper and are reachable only via that fallback.
 *
 *  MERGE BEHAVIOUR
 *  ---------------
 *  If <existing_sprites.h> exists, the tool parses it for
 *  previously-generated sprite blocks (delimited by  slash-star
 *  === <name> === star-slash  markers) and retains them.  New
 *  sprites from <input.ans> are added; if a name clashes with
 *  an existing block, the NEW data overwrites the old block.
 *  Non-clashing existing blocks are preserved verbatim -- this
 *  is how you hand-edit one sprite (e.g. Grunt's step_period=0
 *  positional tweak) and then add another sprite (e.g. Hulk)
 *  without losing your edits.
 *
 *  USAGE
 *  -----
 *  Two forms, auto-detected by whether argv[1] ends in ".ans":
 *
 *      # Legacy single-input form (argv[1] is .ans):
 *      build_sprites7 <input.ans> <output.h> [existing_sprites.h]
 *
 *      # Multi-input form (argv[1] is the output path):
 *      build_sprites7 <output.h> <in1.ans> [in2.ans ...]
 *
 *  In multi-input form, the existing-sprites.h path defaults to
 *  the OUTPUT path itself, so re-running
 *      build_sprites7 sprites.h ENT_HULK.ans
 *  merges with the sprites.h already on disk (preserving any
 *  hand-edited blocks that don't appear in the new .ans inputs).
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
 * Order matches the EntityType enum in btg.c:
 *   Player, Grunt, Quark, Hulk, Brain, Spheroid, Enforcer,
 *   Human, Laser, Terror, Electrode, Cruise
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
    { "player",    "player",    "Player",    6 },
    { "grunt",     "grunt",     "Grunt",     8 },
    { "quark",     "quark",     "Quark",     4 },
    { "hulk",      "hulk",      "Hulk",      4 },
    { "brain",     "brain",     "Brain",     4 },
    { "spheroid",  "spheroid",  "Spheroid",  2 },
    { "enforcer",  "enforcer",  "Enforcer",  2 },
    { "human",     "human",     "Human",     4 },
    { "laser",     "laser",     "Laser",     1 },
    { "terror",    "terror",    "Terror",    1 },
    { "electrode", "electrode", "Electrode", 1 },
    { "cruise",    "cruise",    "Cruise",    1 },
};
#define NUM_ENTITY_MAP  (int)(sizeof(ENTITY_MAP) / sizeof(ENTITY_MAP[0]))

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
    int  facing;        /* 4-bit mask: FACE_N_BIT|FACE_S_BIT|...  */
                       /* or FACE_NONE if untagged               */
} Frame;

typedef struct {
    const EntityMap* map;
    Frame  frames[MAX_FRAMES];
    int    frame_count;
} Sprite;

static Sprite g_sprites[MAX_SPRITES];
static int    g_sprite_count = 0;

/* Facing codes -- each Frame carries a 4-bit facing MASK.  The bit
 * order matches the Direction enum order in btg.c (N=0,S=1,E=2,W=3)
 * so the auto-grouper can index group[0..3] by face index.
 *
 * A frame tagged "Facing: NS" gets (FACE_N_BIT | FACE_S_BIT) and
 * contributes its index to BOTH walk_n[] and walk_s[].  FACE_NONE
 * (0) means "no Facing: tag" -- the auto-grouper skips such frames;
 * they're reachable only via the "no tagged frames for this facing"
 * fallback that lists all frames in source order.
 */
#define FACE_N_BIT (1 << 0)
#define FACE_S_BIT (1 << 1)
#define FACE_E_BIT (1 << 2)
#define FACE_W_BIT (1 << 3)
#define FACE_NONE   0
#define FACE_ALL    (FACE_N_BIT | FACE_S_BIT | FACE_E_BIT | FACE_W_BIT)

/* Face index (0..3) -> bitmask.  Mirrors FACE_SUFFIX/FACE_NAME order. */
static const int FACE_BIT[4] = { FACE_N_BIT, FACE_S_BIT, FACE_E_BIT, FACE_W_BIT };

static const char* FACE_SUFFIX[4] = { "n", "s", "e", "w" };
static const char* FACE_NAME[4]   = { "N", "S", "E", "W" };

/* Parse a Facing: field value and return a 4-bit mask.
 *
 * Walks the WHOLE string (not just the first character) so multi-
 * direction tags like "NS", "SN", "NSEW", "N S", or "N/S" all work.
 * Any character that isn't N/S/E/W (case-insensitive) is silently
 * skipped -- that way separators (spaces, commas, slashes, pipes)
 * are tolerated without special handling.
 *
 * Returns FACE_NONE (0) if the string contains no N/S/E/W letters
 * at all, which the auto-grouper treats as "untagged".
 */
static int parse_facing_list(const char* s) {
    int mask = FACE_NONE;
    while (*s) {
        char c = (char)toupper((unsigned char)*s);
        switch (c) {
            case 'N': mask |= FACE_N_BIT; break;
            case 'S': mask |= FACE_S_BIT; break;
            case 'E': mask |= FACE_E_BIT; break;
            case 'W': mask |= FACE_W_BIT; break;
            default: break;  /* skip spaces, commas, slashes, etc. */
        }
        s++;
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
    Frame*   cur_frame  = NULL;
    bool     in_data    = false;

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
                cur_frame->facing = parse_facing_list(val);
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
                pending_facing = parse_facing_list(val);
            }
        }
    }
    fclose(f);
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
 *  Layout per sprite (using "grunt" as an example).  Each block
 *  produces:
 *
 *    [marker comment:  === grunt === ]
 *    static const uint8_t grunt_f0_r0[40] = { ... };
 *    static const uint8_t grunt_f0_r1[40] = { ... };
 *    static const uint8_t* const grunt_f0_rows[] = {
 *        grunt_f0_r0, grunt_f0_r1
 *    };
 *    static const SpriteFrame grunt_f0 = {
 *        grunt_f0_rows, 4, 2, 2, 1     // rows, w, h, ox+hx, oy+hy
 *    };
 *    ... (per frame) ...
 *    static const SpriteFrame grunt_frames[] = {
 *        grunt_f0, grunt_f1, grunt_f2, grunt_f3
 *    };
 *
 *    // --- Hand-editable walk-cycle tables ---
 *    // Edit these int arrays freely to reorder, reuse, or
 *    // build ping-pong cycles.  Each entry is an index into
 *    // grunt_frames[].  To make a cycle longer or shorter, just
 *    // change the array contents -- no need to touch row data.
 *    static const int grunt_walk_n[] = { 0, 1 };
 *    static const int grunt_walk_s[] = { 2, 3 };
 *    static const int grunt_walk_e[] = { 0, 1 };   // = N fallback
 *    static const int grunt_walk_w[] = { 2, 3 };   // = S fallback
 *
 *    static const SpriteSet sprite_grunt_set = {
 *        "Grunt", 4, grunt_frames,
 *        { { grunt_walk_n, 2, 8, 0, 0, 1, 1 },
 *          { grunt_walk_s, 2, 8, 0, 0, 1, 1 },
 *          { grunt_walk_e, 2, 8, 0, 0, 1, 1 },
 *          { grunt_walk_w, 2, 8, 0, 0, 1, 1 } }
 *    };
 *
 *  The 7-tuple inside each FacingInfo is:
 *      { frame_indices_ptr, count, step_period,
 *        offset_x, offset_y, scale_x, scale_y }
 *
 *  scale_x / scale_y are per-axis stride multipliers consumed by
 *  btk.c's entity_select_frame() with INVERTED semantics:
 *      frame = ((cells * scale) / step_period) % count
 *      -- scale_x applies to E/W facings, scale_y to N/S
 *      -- bigger scale = FASTER animation (more frame advances
 *         per unit of travel)
 *      -- scale=1 reproduces legacy `cells / step_period` exactly
 *      -- scale > step_period yields sub-cell strides (e.g.
 *         step_period=4, scale=8 -> one frame per half-cell)
 *  Defaults emitted here are 1,1 (legacy behaviour, no scaling).
 *  Hand-tune in sprites.h, or live via the `btk -e` walk-table
 *  editor which writes the new values back into the FacingInfo
 *  initializer.  Per-facing granularity means N/S can use a
 *  different scale from E/W on the same entity.
 *
 *  AUTO-GROUPING RULE
 *  ------------------
 *  Each frame's "Facing:" tag may list ONE OR MORE directions
 *  (e.g. "N", "NS", "NSEW").  The builder ORs them into a 4-bit
 *  mask and adds the frame's index to EVERY named facing's
 *  walk_<x>[] array, in source order.
 *
 *    - A frame tagged "Facing: NS" contributes its index to
 *      BOTH walk_n[] and walk_s[].
 *    - Facings with NO tagged frames fall back to ALL frames
 *      in source order (legacy default).
 *    - Frames with NO Facing: tag at all are skipped by the
 *      auto-grouper -- they're reachable only via the fallback.
 *    - If NO frames have any facing tags, all 4 facings share
 *      ALL frames (this is what you get from ENT_GRUNT.ans, which
 *      has no Facing: tags at all -- the Grunt's hand-edited N/S
 *      split is then applied by hand in sprites.h afterwards).
 *
 *  Either way, you can hand-edit the walk_<x>[] arrays afterwards
 *  to assign frames however you like -- the builder's defaults
 *  are just a starting point.
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

        /* SpriteFrame: rows, w, h, ox (combined with hx), oy (with hy). */
        int ox_total = fr->ox + fr->hx;
        int oy_total = fr->oy + fr->hy;
        fprintf(fout,
            "static const SpriteFrame %s_f%d = { "
            "%s_f%d_rows, %d, %d, %d, %d };\n",
            name, fi, name, fi, w, h, ox_total, oy_total);
    }

    /* --- 2. Flat frames[] array (all frames, in source order) --- */
    fprintf(fout, "static const SpriteFrame %s_frames[] = {\n", name);
    for (int fi = 0; fi < N; fi++) {
        fprintf(fout, "    %s_f%d%s\n",
            name, fi, (fi < N - 1) ? "," : "");
    }
    fprintf(fout, "};\n\n");

    /* --- 3. Group frames by facing (multi-direction mask) --- */
    int group[4][MAX_FRAMES];
    int group_count[4] = { 0, 0, 0, 0 };
    int any_tagged = 0;
    for (int fi = 0; fi < N; fi++) {
        int mask = sp->frames[fi].facing;
        if (mask == FACE_NONE) continue;  /* untagged: fallback-only */
        any_tagged = 1;
        for (int f = 0; f < 4; f++) {
            if (mask & FACE_BIT[f]) {
                if (group_count[f] < MAX_FRAMES)
                    group[f][group_count[f]++] = fi;
            }
        }
    }

    /* --- 4. Emit per-facing walk-cycle tables --- */
    fprintf(fout, "/* --- %s walk-cycle tables (HAND-EDITABLE) ---\n", disp);
    fprintf(fout, " * Each entry is an index into %s_frames[].\n", name);
    fprintf(fout, " * Edit freely to reorder, reuse, or build ping-pong cycles.\n");
    if (any_tagged) {
        fprintf(fout, " * Auto-grouped from multi-direction 'Facing:' tags in source\n");
        fprintf(fout, " * .ans -- a frame tagged \"NS\" contributes its index to BOTH\n");
        fprintf(fout, " * walk_n[] and walk_s[].  Facings with no tagged frames fall\n");
        fprintf(fout, " * back to all %d frames in source order.\n", N);
    } else {
        fprintf(fout, " * No 'Facing:' tags in source .ans -- all 4 facings\n");
        fprintf(fout, " * default to ALL %d frames.  Reassign by editing the\n", N);
        fprintf(fout, " * numeric literals below.\n");
    }
    fprintf(fout, " */\n");

    /* Default index sequence: 0..N-1 (used when no facing tags, or
     * when a facing has no tagged frames). */
    int all_indices[MAX_FRAMES];
    for (int i = 0; i < N && i < MAX_FRAMES; i++) all_indices[i] = i;

    for (int f = 0; f < 4; f++) {
        const int* indices;
        int        count;
        bool       from_tags;
        if (any_tagged && group_count[f] > 0) {
            indices   = group[f];
            count     = group_count[f];
            from_tags = true;
        } else {
            /* Either no tags at all, or this facing has no tagged
             * frames -- fall back to "all frames in source order". */
            indices   = all_indices;
            count     = N;
            from_tags = false;
        }
        fprintf(fout, "/* %s: %d frame%s%s */\n",
            FACE_NAME[f], count,
            (count == 1) ? "" : "s",
            from_tags ? " (from Facing: tags)" : " (fallback: all frames)");
        fprintf(fout, "static const int %s_walk_%s[] = {",
            name, FACE_SUFFIX[f]);
        for (int i = 0; i < count; i++) {
            if (i % 8 == 0) fprintf(fout, "\n    ");
            fprintf(fout, "%d%s", indices[i], (i < count - 1) ? "," : "");
        }
        fprintf(fout, "\n};\n");
    }

    /* --- 5. Emit SpriteSet with new FacingInfo format --- */
    fprintf(fout, "\nstatic const SpriteSet sprite_%s_set = {\n", name);
    fprintf(fout, "    \"%s\", %d, %s_frames,\n", disp, N, name);
    fprintf(fout, "    { /* N, S, E, W -- {indices, count, step, ox, oy, scale_x, scale_y} */\n");
    for (int f = 0; f < 4; f++) {
        int count;
        if (any_tagged && group_count[f] > 0) count = group_count[f];
        else                                   count = N;
        /* scale_x / scale_y default to 1,1 -- hand-edit in sprites.h
         * or tune live via `btk -e`; the editor writes back into
         * these last two fields of the FacingInfo initializer. */
        fprintf(fout,
            "        { %s_walk_%s, %d, %d, 0, 0, 1, 1 }%s /* %s */\n",
            name, FACE_SUFFIX[f], count, step,
            (f < 3) ? "," : "",
            FACE_NAME[f]);
    }
    fprintf(fout, "    }\n");
    fprintf(fout, "};\n\n");

    /* --- 6. Emit guard macro so bti.c can skip its stub --- *
     * Produces:  #define HAVE_SPRITE_GRUNT 1
     * bti.c wraps each stub with  #ifndef HAVE_SPRITE_<NAME>
     * so that including sprites.h suppresses only the stubs
     * for entities that have real sprite data here. */
    fprintf(fout, "#define HAVE_SPRITE_");
    for (const char* p = name; *p; p++) {
        fputc(toupper((unsigned char)*p), fout);
    }
    fprintf(fout, " 1\n\n");
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
    /* Two CLI forms, auto-detected by whether argv[1] ends in .ans:
     *
     *   Legacy (single input):
     *     build_sprites7 <input.ans> <output.h> [existing_sprites.h]
     *
     *   Multi-input:
     *     build_sprites7 <output.h> <in1.ans> [in2.ans ...]
     *
     * In multi-input form, the existing-sprites.h path defaults to
     * the OUTPUT path itself, so re-running
     *     build_sprites7 sprites.h ENT_HULK.ans
     * merges with the sprites.h already on disk (preserving any
     * hand-edited blocks that don't appear in the new .ans inputs).
     */
    const char*  out_file   = NULL;
    const char*  exist_file = NULL;
    const char** ans_files  = NULL;
    int          n_ans      = 0;
    bool         legacy     = false;

    if (argc < 3) {
        fprintf(stderr,
            "Usage:\n"
            "  Legacy : %s <input.ans> <output.h> [existing_sprites.h]\n"
            "  Multi  : %s <output.h>  <in1.ans> [in2.ans ...]\n"
            "\n"
            "  Auto-detected by whether argv[1] ends in '.ans'.\n"
            "\n"
            "  In multi-input form, existing_sprites.h defaults to the\n"
            "  OUTPUT path itself, so re-running\n"
            "      %s sprites.h ENT_HULK.ans\n"
            "  merges with the sprites.h already on disk (preserving\n"
            "  hand-edited blocks that aren't in the new .ans inputs).\n"
            "\n"
            "  Facing: tags may list multiple directions, e.g. 'NS'\n"
            "  contributes the frame to BOTH walk_n[] and walk_s[].\n"
            "\n"
            "  Character names are matched case-insensitively and\n"
            "  plural-awarely to the 12 entity types (player, grunt,\n"
            "  quark, hulk, brain, spheroid, enforcer, human, laser,\n"
            "  terror, electrode, cruise).\n",
            argv[0], argv[0], argv[0]);
        return 1;
    }

    /* Auto-detect: if argv[1] ends in '.ans', it's the legacy form. */
    {
        size_t L = strlen(argv[1]);
        if (L >= 4 && strcmp(argv[1] + L - 4, ".ans") == 0) legacy = true;
    }

    if (legacy) {
        ans_files  = (const char**)malloc(sizeof(char*));
        ans_files[0] = argv[1];
        n_ans      = 1;
        out_file   = argv[2];
        exist_file = (argc >= 4) ? argv[3] : "sprites.h";
    } else {
        out_file   = argv[1];
        exist_file = out_file;  /* merge from output file itself */
        n_ans      = argc - 2;
        ans_files  = (const char**)malloc(sizeof(char*) * (size_t)n_ans);
        for (int i = 0; i < n_ans; i++) ans_files[i] = argv[2 + i];
    }

    /* ---- SAFETY CHECK: refuse .ans as output destination ----
     * The output file must be a C header (.h, .hpp, .hh, or .hxx).
     * Rejecting other extensions here prevents the footgun of
     * running
     *     build_sprites7 ENT_GRUNT.ans ENT_HULK.ans sprites.h
     * (which the auto-detector interprets as "out=ENT_HULK.ans,
     * inputs=[sprites.h]") and clobbering an .ans source file
     * with generated C code.  We also reject .c, .o, and other
     * non-header extensions for the same reason.
     */
    {
        /* Find the last '.' in the basename (after last '/'). */
        const char* base = strrchr(out_file, '/');
        base = base ? base + 1 : out_file;
        const char* dot  = strrchr(base, '.');
        bool ok = false;
        if (dot) {
            /* Case-insensitive compare against allowed extensions. */
            static const char* const ALLOWED[] = { ".h", ".hpp", ".hh", ".hxx", NULL };
            for (int i = 0; ALLOWED[i]; i++) {
                size_t n = strlen(ALLOWED[i]);
                if (strncasecmp(dot, ALLOWED[i], n) == 0 && dot[n] == '\0') {
                    ok = true;
                    break;
                }
            }
        }
        if (!ok) {
            fprintf(stderr,
                "Error: output path '%s' does not look like a C header.\n"
                "  The destination must end in .h (or .hpp/.hh/.hxx).\n"
                "  This safety check prevents accidental overwrite of\n"
                "  .ans source files or other non-header artifacts.\n"
                "\n"
                "  Correct usage:\n"
                "    %s <output.h>  <in1.ans> [in2.ans ...]\n"
                "  Legacy single-input:\n"
                "    %s <input.ans> <output.h> [existing_sprites.h]\n",
                out_file, argv[0], argv[0]);
            free(ans_files);
            return 1;
        }
    }

    /* ---- SAFETY CHECK: prompt before overwriting existing .h ----
     * If the output file already exists (and isn't the same path we
     * just loaded as the existing-sprites.h merge source -- in that
     * case the merge IS the overwrite, and we've already announced
     * "Loaded N existing block(s)" above so the user knows), ask
     * for explicit confirmation.  Pipe 'y' through stdin to bypass
     * in scripts:   echo y | build_sprites7 sprites.h ENT_HULK.ans
     * or set the env var  BTSPRITES_FORCE=1  to skip the prompt.
     */
    {
        FILE* probe = fopen(out_file, "r");
        if (probe) {
            fclose(probe);
            const char* force = getenv("BTSPRITES_FORCE");
            if (force && force[0] == '1' && force[1] == '\0') {
                /* -F bypass: silent */
            } else {
                fprintf(stderr,
                    "Warning: output file '%s' already exists.\n"
                    "  Existing blocks will be merged; clashing blocks\n"
                    "  will be overwritten with new data from .ans.\n"
                    "  Continue? [y/N] ",
                    out_file);
                fflush(stderr);
                char buf[16];
                if (!fgets(buf, sizeof(buf), stdin)) {
                    fprintf(stderr, "  (no input) -- aborting.\n");
                    free(ans_files);
                    return 1;
                }
                if (buf[0] != 'y' && buf[0] != 'Y') {
                    fprintf(stderr, "  Aborted.  No files changed.\n");
                    free(ans_files);
                    return 1;
                }
            }
        }
    }

    /* Step 1: load existing sprites.h if present (for merge) */
    parse_existing_sprites_h(exist_file);
    if (g_existing_count > 0) {
        fprintf(stderr, "Loaded %d existing sprite block(s) from '%s'.\n",
            g_existing_count, exist_file);
    }

    /* Step 2: parse each input .ans, accumulating into g_sprites[] */
    for (int i = 0; i < n_ans; i++) {
        parse_ans(ans_files[i]);
    }
    if (g_sprite_count == 0) {
        fprintf(stderr, "Error: no sprites parsed from input(s).\n");
        return 1;
    }

    fprintf(stderr, "Parsed %d sprite(s) from %d .ans file(s):\n",
        g_sprite_count, n_ans);
    for (int i = 0; i < g_sprite_count; i++) {
        fprintf(stderr, "  - %-10s : %d frame(s), dims ",
            g_sprites[i].map->display, g_sprites[i].frame_count);
        for (int j = 0; j < g_sprites[i].frame_count; j++) {
            fprintf(stderr, "%s%dx%d",
                j ? "/" : "",
                g_sprites[i].frames[j].w,
                g_sprites[i].frames[j].h);
        }
        /* Tally tagged facings for a quick sanity readout. */
        int tagged = 0;
        for (int j = 0; j < g_sprites[i].frame_count; j++)
            if (g_sprites[i].frames[j].facing != FACE_NONE) tagged++;
        fprintf(stderr, "  (%d tagged, %d untagged)\n",
            tagged, g_sprites[i].frame_count - tagged);
    }

    /* Step 3: emit the merged sprites.h */
    emit_sprites_h(out_file);
    fprintf(stderr, "Wrote '%s'.\n", out_file);

    free(ans_files);
    return 0;
}
