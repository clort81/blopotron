#!/usr/bin/env python3
"""
ansi_sprite_editor.py - Terminal-native ANSI sprite isolation & curation tool.
Design Philosophy:
- Procedural, Unix-style simplicity (KISS principle).
- Deterministic behavior and explicit state management.
- Minimal dependencies; relies only on standard library.
- Extreme OOP avoidance: 'Cell' is used strictly as a data record, not an object with behavior.
"""
import sys
import os
import time
import termios
import shutil
from collections import defaultdict

# === CONFIGURATION ===
# Tolerance for RGB color matching to handle minor ANSI rendering variations
RGB_TOLERANCE = 4

# Explicit ordering of metadata fields for deterministic export output.
# 'Facing' is placed immediately after 'Character' per specification.
METADATA_KEYS = [
    'Index',
    'Character',
    'Facing',
    'FrameNo',
    'OffsetX',
    'OffsetY',
    'HotspotX',
    'HotspotY'
]

# Half-shift toggle map for E/W <-> EH/WH
HALF_SHIFT_TOGGLE = {'E': 'EH', 'EH': 'E', 'W': 'WH', 'WH': 'W'}


class Cell:
    """
    Lightweight data record for grid cells.
    Used strictly as a structured container to avoid passing 3-tuples
    (char, fg, bg) through every function signature.
    This is not an object-oriented design pattern, but a procedural
    convenience for state grouping. No behavioral methods are attached.
    """
    def __init__(self, char=' ', fg=(255, 255, 255), bg=(0, 0, 0)):
        self.char = char
        self.fg = fg
        self.bg = bg

    def copy(self):
        return Cell(self.char, self.fg, self.bg)


def is_color_similar(c1, c2, tolerance=RGB_TOLERANCE):
    """
    Determines if two RGB tuples are visually similar within a defined tolerance.
    Prevents minor ANSI encoding variations from breaking background detection.
    """
    return (abs(c1[0] - c2[0]) <= tolerance and
            abs(c1[1] - c2[1]) <= tolerance and
            abs(c1[2] - c2[2]) <= tolerance)


def parse_to_grid(text):
    """
    Minimal state-machine parser for 24-bit RGB ANSI text.
    Converts a raw ANSI string into a 2D grid of Cell records.
    """
    grid = []
    current_line = []
    current_fg = (255, 255, 255)
    current_bg = (0, 0, 0)

    i = 0
    n = len(text)

    while i < n:
        if text[i] == '\x1b' and i + 1 < n and text[i+1] == '[':
            i += 2
            start = i
            while i < n and not (text[i].isalpha() or text[i] == '@'):
                i += 1
            if i >= n:
                break
            cmd = text[i]
            params_str = text[start:i]
            i += 1

            if cmd == 'm':
                parts = [p.strip() for p in params_str.split(';') if p.strip()]
                j = 0
                while j < len(parts):
                    try:
                        code = parts[j]
                        if code == '0':
                            current_fg = (255, 255, 255)
                            current_bg = (0, 0, 0)
                            j += 1
                        elif j + 4 <= len(parts) and code == '38' and parts[j+1] == '2':
                            r, g, b = int(parts[j+2]), int(parts[j+3]), int(parts[j+4])
                            current_fg = (max(0, min(255, r)), max(0, min(255, g)), max(0, min(255, b)))
                            j += 5
                        elif j + 4 <= len(parts) and code == '48' and parts[j+1] == '2':
                            r, g, b = int(parts[j+2]), int(parts[j+3]), int(parts[j+4])
                            current_bg = (max(0, min(255, r)), max(0, min(255, g)), max(0, min(255, b)))
                            j += 5
                        else:
                            j += 1
                    except (ValueError, IndexError):
                        j += 1
                        continue
            elif i < n and text[i] == '\n':
                grid.append(current_line)
                current_line = []
                i += 1
            elif i < n:
                current_line.append(Cell(text[i], current_fg, current_bg))
                i += 1
        elif text[i] == '\n':
            grid.append(current_line)
            current_line = []
            i += 1
        else:
            current_line.append(Cell(text[i], current_fg, current_bg))
            i += 1

    if current_line:
        grid.append(current_line)

    return grid


def find_background_color(grid, margin=2):
    """
    Samples the outer margin of the grid to determine the dominant background color.
    Uses median calculation per RGB channel to resist outliers (e.g., stray artifacts).
    """
    colors = []
    h = len(grid)
    if h == 0:
        return (0, 0, 0)

    for y, row in enumerate(grid):
        for x in range(len(row)):
            if x < margin or x >= len(row) - margin or y < margin or y >= h - margin:
                colors.append(row[x].bg)

    if not colors:
        return (0, 0, 0)

    colors.sort(key=lambda c: c[0])
    r = colors[len(colors)//2][0]
    colors.sort(key=lambda c: c[1])
    g = colors[len(colors)//2][1]
    colors.sort(key=lambda c: c[2])
    b = colors[len(colors)//2][2]

    return (r, g, b)


def normalize_empty_cells(grid, bg_color):
    """
    Replaces cells where both FG and BG match the global background color
    with actual space characters. Fixes exporter artifacts (like '▔') that
    visually render as empty but break flood-fill separation logic.
    """
    for row in grid:
        for cell in row:
            if is_color_similar(cell.fg, bg_color) and is_color_similar(cell.bg, bg_color):
                cell.char = ' '


def is_sprite_cell(cell, bg_color):
    """
    Heuristic to determine if a cell belongs to a sprite.
    A cell is part of a sprite if it is a non-space character, OR if its
    background color differs from the global background (indicating a block element).
    """
    space_chars = {' ', '\u00A0', '\u2000', '\u2001', '\u2002', '\u2003',
                   '\u2004', '\u2005', '\u2006', '\u2007', '\u2008', '\u2009',
                   '\u200A', '\u202F', '\u205F', '\u2800', '\u3000'}
    if cell.char not in space_chars:
        return True
    return not is_color_similar(cell.bg, bg_color)


def clean_grid_for_detection(grid, bg_color):
    """
    Creates a sanitized copy of the grid for detection purposes.
    Zeroes out foreground colors that match the background to prevent
    invisible text from being falsely detected as sprite data.
    """
    cleaned = []
    for row in grid:
        cleaned.append([c.copy() if not is_color_similar(c.fg, bg_color) else Cell(' ', c.fg, c.bg) for c in row])
    return cleaned


def find_sprites_banded(original_grid, bg_color, min_row_char_thresh=2):
    """
    Core sprite detection algorithm with two-phase approach:

    Phase 1: Identify sprite rows (horizontal bands)
    - Scan each Y row and count sprite cells
    - If a row has >= min_row_char_thresh sprite cells, it's part of a sprite row
    - Group consecutive qualifying rows into bands
    - For each band, record sprite_row_y1, sprite_row_y2, sprite_row_height

    Phase 2: Within each band, scan left-to-right
    - For each X column in the band's Y range, check for sprite cells
    - Find first unvisited sprite cell and flood-fill to capture full shape
    - Guarantees monotonically increasing sprite IDs in strict left-to-right order
    """
    if not original_grid:
        sys.stderr.write("\x1b[31mERROR: Empty grid\x1b[0m\n")
        return []

    grid_h = len(original_grid)
    grid_w = max(len(row) for row in original_grid) if grid_h else 0
    cleaned = clean_grid_for_detection(original_grid, bg_color)

    # === PHASE 1: Identify sprite rows (horizontal bands) ===
    sprite_row_flags = []
    for y in range(grid_h):
        count = sum(1 for c in cleaned[y] if c.char != ' ')
        sprite_row_flags.append(count >= min_row_char_thresh)

    bands = []
    in_band = False
    start_y = 0
    for y in range(grid_h):
        if sprite_row_flags[y]:
            if not in_band:
                in_band = True
                start_y = y
        elif in_band:
            bands.append({
                'y1': start_y,
                'y2': y - 1,
                'height': y - start_y
            })
            in_band = False
    if in_band:
        bands.append({
            'y1': start_y,
            'y2': grid_h - 1,
            'height': grid_h - start_y
        })

    if not bands:
        sys.stderr.write("\x1b[31mERROR: No sprite rows detected\x1b[0m\n")
        return []

    # === PHASE 2: Within each band, scan left-to-right ===
    sprites = []
    sprite_id = 1
    dirs = [(-1, -1), (0, -1), (1, -1), (-1, 0), (1, 0), (-1, 1), (0, 1), (1, 1)]
    visited = [[False] * grid_w for _ in range(grid_h)]

    for band in bands:
        b_start = band['y1']
        b_end = band['y2']

        for x in range(grid_w):
            start_y_cell = None
            for y in range(b_start, b_end + 1):
                if x < len(original_grid[y]) and not visited[y][x]:
                    if is_sprite_cell(original_grid[y][x], bg_color):
                        start_y_cell = y
                        break

            if start_y_cell is None:
                continue

            q = [(x, start_y_cell)]
            comp = []
            bnds = [x, start_y_cell, x, start_y_cell]
            visited[start_y_cell][x] = True

            while q:
                cx, cy = q.pop(0)
                comp.append((cx, cy))
                bnds[0] = min(bnds[0], cx)
                bnds[1] = min(bnds[1], cy)
                bnds[2] = max(bnds[2], cx)
                bnds[3] = max(bnds[3], cy)

                for dx, dy in dirs:
                    nx, ny = cx + dx, cy + dy
                    if (0 <= ny < grid_h and 0 <= nx < len(original_grid[ny]) and
                            not visited[ny][nx] and
                            is_sprite_cell(original_grid[ny][nx], bg_color)):
                        visited[ny][nx] = True
                        q.append((nx, ny))

            if len(comp) >= 3:
                x1, y1, x2, y2 = bnds
                w, h = x2 - x1 + 1, y2 - y1 + 1
                sprites.append({
                    'id': sprite_id, 'x': x1, 'y': y1, 'w': w, 'h': h,
                    'cells': comp, 'enabled': True,
                    'metadata': {
                        'Index': str(sprite_id),
                        'Character': '',
                        'Facing': '',
                        'FrameNo': '0',
                        'OffsetX': '0',
                        'OffsetY': '0',
                        'HotspotX': str(w // 2),
                        'HotspotY': str(h // 2)
                    }
                })
                sprite_id += 1

    return sprites


def clean_cell(cell, global_bg):
    """
    Sanitizes a single cell for export or clean preview.
    Forces true background cells to black/empty, and converts background-colored
    block characters into standard block characters with inverted colors for visibility.
    """
    space_chars = {' ', '\u00A0', '\u2000', '\u2001', '\u2002', '\u2003',
                   '\u2004', '\u2005', '\u2006', '\u2007', '\u2008', '\u2009',
                   '\u200A', '\u202F', '\u205F', '\u2800', '\u3000'}
    c = cell.copy()
    if c.char in space_chars:
        if is_color_similar(c.bg, global_bg):
            c.fg = c.bg = (0, 0, 0)
            return c
        c.char, c.fg, c.bg = '█', c.bg, (0, 0, 0)
        return c

    if is_color_similar(c.bg, global_bg):
        c.bg = (0, 0, 0)
    if is_color_similar(c.fg, global_bg) and c.bg != (0, 0, 0):
        c.fg = (0, 0, 0)
    return c


def mix_color(c1, c2, ratio):
    """
    Linearly interpolates between two RGB colors.
    Used to apply a translucent highlight (e.g., red) to selected sprites.
    """
    return tuple(int(c1[i] * (1 - ratio) + c2[i] * ratio) for i in range(3))


def generate_output(grid, sprites, selected_id=None, clean_mode=False, global_bg=None):
    """
    Renders the entire grid to a string with ANSI escape codes.
    Handles selection highlighting, sprite ID + Facing labeling, and the bottom status bar.
    """
    RED = (255, 0, 0)
    work = [[c.copy() for c in r] for r in grid]

    if clean_mode and global_bg is not None:
        for y, row in enumerate(work):
            for x, c in enumerate(row):
                work[y][x] = clean_cell(c, global_bg)

    if selected_id is not None:
        for s in sprites:
            if s['id'] == selected_id:
                for x, y in s['cells']:
                    if 0 <= y < len(work) and 0 <= x < len(work[y]):
                        c = work[y][x]
                        c.fg = mix_color(c.fg, RED, 0.5)
                        c.bg = mix_color(c.bg, RED, 0.5)
                break

    for s in sprites:
        x = s['x']
        y = max(0, s['y'] - 1)
        facing = s['metadata'].get('Facing', '')
        lbl = f"{s['id']}:{facing}" if facing else str(s['id'])

        if y < len(work) and x < len(work[y]):
            fg = (255, 255, 0) if s['enabled'] else (160, 160, 160)
            for i, ch in enumerate(lbl):
                if x + i < len(work[y]):
                    work[y][x + i].char = ch
                    work[y][x + i].fg = fg
                    work[y][x + i].bg = (0, 0, 0)

            if selected_id == s['id']:
                for i in range(len(lbl)):
                    if x + i < len(work[y]):
                        work[y][x + i].bg = RED

    lines = []
    for row in work:
        line, pf, pb = '', None, None
        for c in row:
            if c.fg != pf:
                line += f'\x1b[38;2;{c.fg[0]};{c.fg[1]};{c.fg[2]}m'
                pf = c.fg
            if c.bg != pb:
                line += f'\x1b[48;2;{c.bg[0]};{c.bg[1]};{c.bg[2]}m'
                pb = c.bg
            line += c.char
        lines.append(line + '\x1b[0m')

    # Status bar now includes the t=half-shift hint
    lines.append(f"\x1b[30;107m ←→=nav  SPACE=toggle  O=clean  i=info  f=facing  t=half-shift  R=row-name  E=export  Q=quit \x1b[0m")
    sel = next((s for s in sprites if s['id'] == selected_id), None)
    st2 = f"\x1b[36m MODE: {'CLEAN' if clean_mode else 'ORIG'} | Sprite {selected_id} [{'ENABLED' if sel and sel['enabled'] else 'DISABLED'}] \x1b[0m" if selected_id is not None else "\x1b[33m No sprite selected \x1b[0m"
    lines.append(st2)

    return '\n'.join(lines)


def draw_info_modal(sprite, cols, rows):
    """
    Renders an overlay modal displaying editable metadata for the selected sprite.
    """
    mw, mh = 34, 9
    sr = sprite['y'] + sprite['h'] + 1
    if sr + mh > rows:
        sr = max(1, rows - mh)
    sc = max(1, sprite['x'] + sprite['w'] // 2 - mw // 2)
    if sc + mw > cols:
        sc = max(1, cols - mw)

    fields = [
        ('1', 'Character:', sprite['metadata'].get('Character', '')),
        ('2', 'Index:',     sprite['metadata'].get('Index', '')),
        ('3', 'FrameNo:',   sprite['metadata'].get('FrameNo', '')),
        ('4', 'Facing:',    sprite['metadata'].get('Facing', '')),
        ('5', 'OffsetX:',   sprite['metadata'].get('OffsetX', '')),
        ('6', 'OffsetY:',   sprite['metadata'].get('OffsetY', '')),
        ('7', 'HotspotX:',  sprite['metadata'].get('HotspotX', ''))
    ]

    sys.stdout.write(f"\x1b[{sr};{sc}H\x1b[1;37;44m{'─ Info ─'}{'─' * (mw - 8)}\x1b[0m\n")
    for i, (num, lbl, val) in enumerate(fields):
        bg = "\x1b[44m" if i % 2 == 0 else "\x1b[100m"
        sys.stdout.write(f"\x1b[{sr+i+1};{sc}H{bg}\x1b[37m {num}. {lbl:<14} {val:<14}\x1b[0m\n")
    sys.stdout.write(f"\x1b[{sr+mh-1};{sc}H\x1b[1;37;44m{'─' * mw}\x1b[0m\n")


def get_bottom_input(prompt, default=''):
    """
    Generic bottom-of-screen input prompt.
    """
    cols, rows = shutil.get_terminal_size()
    sys.stdout.write(f"\x1b[{rows};1H\x1b[K")
    sys.stdout.write(f"\x1b[1;33m{prompt} [{default}]: \x1b[0m")
    sys.stdout.flush()

    line = ''
    while True:
        ch = sys.stdin.read(1)
        if ch in ('\n', '\r'):
            sys.stdout.write('\n')
            break
        elif ch in ('\x7f', '\x08'):
            if line:
                line = line[:-1]
                sys.stdout.write('\x08 \x08')
                sys.stdout.flush()
        else:
            line += ch
            sys.stdout.write(ch)
            sys.stdout.flush()

    return line.strip() if line.strip() else default


def show_help_screen():
    """
    Displays a static help overlay before entering the main event loop.
    """
    sys.stdout.write("\x1b[H\x1b[2J")
    for line in [
        "\x1b[1;36m┌── ANSI SPRITE CLIP v0.98 ───────────────────────────────────────────────────┐",
        "│ \x1b[1;37mTerminal Sprite Isolation & Curation Tool\x1b[0m                                   \x1b[1,36m│",
        "\x1b[1,36m└─────────────────────────────────────────────────────────────────────────────┘",
        "", "\x1b[1;33mControls:\x1b[0m",
        "  \x1b[1;37m← →\x1b[0m      Navigate sprites",
        "  \x1b[1;37mSPACE\x1b[0m    Toggle export enable",
        "  \x1b[1;37mO\x1b[0m        Toggle clean preview",
        "  \x1b[1;37mi\x1b[0m        Info modal (1-7 to edit)",
        "  \x1b[1;37mf\x1b[0m        Set Facing (Arrow or Enter for text)",
        "  \x1b[1;37mt\x1b[0m        Toggle half-shift (E<->EH, W<->WH)",
        "  \x1b[1;37mR\x1b[0m        Name all sprites in row",
        "  \x1b[1;37mE\x1b[0m        Export",
        "  \x1b[1;37mQ\x1b[0m        Quit",
        "", "\x1b[1;32mPress ENTER...\x1b[0m"
    ]:
        sys.stdout.write(line + "\n")
    sys.stdout.flush()
    sys.stdin.read(1)


def export_sprites(grid, sprites, inp, bg, clean=False):
    """
    Writes isolated sprites to individual .ans files.
    Groups sprites by 'Character' name if provided, otherwise exports individually.

    Export Rules:
    - Index is strictly re-sequenced to 0-base for every file.
    - FrameNo starts at 0 and increments by 1.
    - FrameNo resets to 0 immediately if the 'Facing' metadata changes.
    """
    base = os.path.splitext(os.path.basename(inp))[0]
    ts = int(time.time() % 10000)
    d = f"{base}_{ts}"
    os.makedirs(d, exist_ok=True)

    by_char = defaultdict(list)
    solo = []
    for s in sprites:
        if not s['enabled']:
            continue
        cn = s['metadata'].get('Character', '').strip()
        (by_char[cn] if cn else solo).append(s)

    def render(s):
        ls = []
        for y in range(s['y'], s['y'] + s['h']):
            ln, pf, pb = '', None, None
            for x in range(s['x'], s['x'] + s['w']):
                if y < len(grid) and x < len(grid[y]):
                    c = clean_cell(grid[y][x], bg) if clean else grid[y][x].copy()
                else:
                    c = Cell()
                if c.fg != pf:
                    ln += f'\x1b[38;2;{c.fg[0]};{c.fg[1]};{c.fg[2]}m'
                    pf = c.fg
                if c.bg != pb:
                    ln += f'\x1b[48;2;{c.bg[0]};{c.bg[1]};{c.bg[2]}m'
                    pb = c.bg
                ln += c.char
            ls.append(ln + '\x1b[0m')
        return ls

    def get_meta_lines(meta):
        lines = []
        for k in METADATA_KEYS:
            v = meta.get(k, '')
            if k == 'Facing' or v:
                lines.append(f"{k}: {v}")
        return lines

    # Process grouped character sprites
    for cn, grp in by_char.items():
        grp.sort(key=lambda s: int(s['metadata'].get('Index', '0')))

        with open(os.path.join(d, f"{base}_{cn}.ans"), 'w') as f:
            frame_idx = 0
            frame_no = 0
            last_facing = None

            for s in grp:
                export_meta = dict(s['metadata'])
                export_meta['Index'] = str(frame_idx)

                current_facing = export_meta.get('Facing', '')
                if current_facing != last_facing:
                    frame_no = 0
                    last_facing = current_facing
                else:
                    frame_no += 1

                export_meta['FrameNo'] = str(frame_no)

                f.write('\n'.join(get_meta_lines(export_meta) + [''] + render(s)) + '\n')
                frame_idx += 1

    # Process ungrouped solo sprites
    for s in solo:
        export_meta = dict(s['metadata'])
        export_meta['Index'] = '0'
        export_meta['FrameNo'] = '0'
        with open(os.path.join(d, f"{base}_{s['id']:03d}.ans"), 'w') as f:
            f.write('\n'.join(get_meta_lines(export_meta) + [''] + render(s)) + '\n')

    print(f"\x1b[32mExported {len(solo)+len(by_char)} to './{d}' ({'CLEAN' if clean else 'RAW'})\x1b[0m\n")


def main():
    """
    Entry point. Handles argument parsing, terminal raw-mode configuration,
    and the main event loop for interactive sprite curation.
    """
    if len(sys.argv) < 2:
        print(f"\x1b[31mUsage: {sys.argv[0]} [-clean] <file.ans>\x1b[0m")
        sys.exit(1)

    force_clean = False
    inp = None
    for a in sys.argv[1:]:
        if a == '-clean':
            force_clean = True
        elif not inp:
            inp = a

    if not inp:
        print("\x1b[31mNo input file.\x1b[0m")
        sys.exit(1)

    try:
        with open(inp, 'r', encoding='utf-8') as f:
            txt = f.read()
    except Exception as e:
        print(f"\x1b[31mRead error: {e}\x1b[0m")
        sys.exit(1)

    if not txt.strip():
        print("\x1b[33mEmpty.\x1b[0m")
        sys.exit(0)

    grid = parse_to_grid(txt)
    if not grid:
        print("\x1b[31mParse fail.\x1b[0m")
        sys.exit(1)

    bg = find_background_color(grid)
    normalize_empty_cells(grid, bg)
    sprites = find_sprites_banded(grid, bg)

    if not sprites:
        print("\x1b[33mNo sprites.\x1b[0m")
        sys.exit(0)

    show_help_screen()

    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    try:
        ns = termios.tcgetattr(fd)
        ns[3] &= ~(termios.ECHO | termios.ICANON)
        ns[6][termios.VMIN], ns[6][termios.VTIME] = 1, 0
        termios.tcsetattr(fd, termios.TCSANOW, ns)
        sys.stdout.write('\x1b[?25l')
        sys.stdout.flush()

        si, cv, inf, run = 0, False, False, True
        while run:
            sys.stdout.write('\x1b[H\x1b[J' + generate_output(grid, sprites, sprites[si]['id'], cv, bg))
            if inf:
                draw_info_modal(sprites[si], *shutil.get_terminal_size())
            sys.stdout.flush()

            ch = sys.stdin.read(1)
            if ch == '\x1b':
                nc = sys.stdin.read(1)
                if nc == '[':
                    fc = sys.stdin.read(1)
                    if fc == 'C':
                        ch = '\x1b[C'
                    elif fc == 'D':
                        ch = '\x1b[D'

            if ch in ('q', 'Q'):
                run = False
            elif ch == ' ':
                sprites[si]['enabled'] = not sprites[si]['enabled']
            elif ch == '\x1b[D':
                si = (si - 1) % len(sprites)
            elif ch == '\x1b[C':
                si = (si + 1) % len(sprites)
            elif ch in ('o', 'O'):
                cv = not cv
            elif ch == 'i':
                inf = not inf
            elif ch in ('f', 'F'):
                nxt = sys.stdin.read(1)
                if nxt == '\x1b':
                    k1 = sys.stdin.read(1)
                    k2 = sys.stdin.read(1)
                    if k1 == '[':
                        arrow_map = {'A': 'N', 'B': 'S', 'C': 'E', 'D': 'W'}
                        if k2 in arrow_map:
                            sprites[si]['metadata']['Facing'] = arrow_map[k2]
                elif nxt in ('\n', '\r'):
                    current = sprites[si]['metadata'].get('Facing', '')
                    new_val = get_bottom_input("Facing (e.g., N, S, E, W, NSWE, EH, WH)", current)
                    sprites[si]['metadata']['Facing'] = new_val
            elif ch in ('t', 'T'):
                # Toggle East/West <-> Half-shifted variant
                cur = sprites[si]['metadata'].get('Facing', '')
                if cur in HALF_SHIFT_TOGGLE:
                    sprites[si]['metadata']['Facing'] = HALF_SHIFT_TOGGLE[cur]
            elif inf and ch == '3':
                nxt = sys.stdin.read(1)
                if nxt in '0123456789':
                    sprites[si]['metadata']['FrameNo'] = nxt
            elif inf and ch == '4':
                nxt = sys.stdin.read(1)
                if nxt == '\x1b':
                    k1 = sys.stdin.read(1)
                    k2 = sys.stdin.read(1)
                    if k1 == '[':
                        if k2 == 'A':
                            sprites[si]['metadata']['Facing'] = 'N'
                        elif k2 == 'B':
                            sprites[si]['metadata']['Facing'] = 'S'
                        elif k2 == 'C':
                            sprites[si]['metadata']['Facing'] = 'E'
                        elif k2 == 'D':
                            sprites[si]['metadata']['Facing'] = 'W'
            elif inf and ch in '12567':
                fm = {'1': 'Character', '2': 'Index', '5': 'OffsetX', '6': 'OffsetY', '7': 'HotspotX'}
                k = fm[ch]
                sprites[si]['metadata'][k] = get_bottom_input(f"Edit {k}: ")
            elif ch in ('r', 'R'):
                ry = sprites[si]['y']
                nm = get_bottom_input(f"Name for row Y~{ry}")
                if nm:
                    for s in sprites:
                        if abs(s['y'] - ry) <= 2:
                            s['metadata']['Character'] = nm
            elif ch in ('e', 'E'):
                export_sprites(grid, sprites, inp, bg, force_clean or cv)
                time.sleep(1)

    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)
        sys.stdout.write('\x1b[?25h\x1b[0m\n')
        sys.stdout.flush()

    print("\nBackground", bg, "....")


if __name__ == "__main__":
    main()
