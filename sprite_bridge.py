#!/usr/bin/env python3
"""
sprite_bridge.py - 1980s procedural ANSI art compositor
with instance-addressed backing store and adaptive framerate control.

ANSI escapes are opaque string data, no parsing.
Column width implies newlines.
Client must not re-draw same instance ID (overwrites backing store).

================================================================
 DRAWING MODES: CFLUSH vs ERASE/DRAW
================================================================

This bridge supports two fundamentally different drawing strategies.
The choice has major implications for frame skipping and bandwidth.

MODE A: CFLUSH (Full Redraw)
----------------------------
Pattern:  CFLUSH -> DRAW x N -> FLUSH
          (clear buffer, stamp all sprites, emit)

Properties:
  - Every frame is a complete redraw from scratch.
  - The bridge's backing store and instance table are irrelevant
    across frames — CFLUSH resets everything.
  - Frame skipping is TRIVIALLY SAFE: if we drop a FLUSH, the
    terminal still shows the previous frame. The next frame starts
    with CFLUSH, which wipes the buffer and starts fresh.
  - Bandwidth is CONSTANT per frame regardless of how many
    entities moved.
  - The game does NOT need to track sprite instances.

Best for:
  - Local terminals
  - Simple demos
  - Games with many moving entities where delta tracking costs
    more than it saves
  - ANY situation where frame skipping may be needed

MODE B: ERASE/DRAW (Delta Rendering)
-------------------------------------
Pattern:  ERASE(old_pos) -> DRAW(new_pos) -> FLUSH
          (restore old background, stamp new sprite, emit)

Properties:
  - Each frame only transmits what changed.
  - The bridge's backing store is the SOURCE OF TRUTH for what
    is on the terminal.
  - Frame skipping is UNSAFE: if we skip frame N+1's FLUSH, the
    terminal still shows frame N, but the bridge's backing store
    has advanced to frame N+1. When frame N+2 issues an ERASE,
    it restores a background the terminal never actually displayed.
    Result: ghost sprites, visual corruption.
  - Bandwidth is PROPORTIONAL to motion. A static screen costs
    almost nothing; a screen full of movement costs as much as
    CFLUSH.
  - The game MUST track (sprite_type, instance_id) for every
    entity to issue correct ERASE commands.

Best for:
  - BBS doors, SSH tunnels, slow links — anywhere bandwidth is
    the bottleneck and the game can afford the bookkeeping.

FRAME SKIPPING POLICY:
  - In CFLUSH mode: frame skipping is always safe.
  - In ERASE/DRAW mode: frame skipping will corrupt the backing
    store. Use only if the game periodically sends CFLUSH to
    resync, or accept visual artifacts.

================================================================
 FRAMERATE CONTROL
================================================================

The bridge measures how long each FLUSH takes to write to stdout.
If the backend (terminal, BBS PTY, SSH pipe) is slow to consume
bytes, the flush duration increases. The bridge uses an exponential
moving average (EMA) to detect when we're falling behind and
dynamically increases the frame skip ratio.

Environment variables:
  HARDFRAME   Force a fixed skip ratio (overrides dynamic logic).
              0 = dynamic only (default)
              1 = skip 1 in 2 frames  (~15 fps at 30 fps target)
              2 = skip 2 in 3 frames  (~10 fps)
              N = skip N in N+1 frames (30/(N+1) fps)

  TARGET_FPS  Target framerate for dynamic threshold calculation.
              Default: 30

Diagnostics:
  When a frame is skipped, "SKIP_FRAME" is written to stderr.
  The game can read this to adapt its own behavior if desired.

================================================================
 COMMAND PROTOCOL
================================================================

SPRITE,<id>,<cols>,<rows>,<data>
    Define a sprite. Data may contain newlines for row breaks
    and ANSI escape sequences (opaque). Multi-frame sprites use
    underline markers (\x1b[4m ... \x1b[24m) to delimit frames.

DRAW,<sprite_id>,<instance_id>,<x>,<y>,<frame>
    Stamp sprite at (x,y). Saves background for later ERASE.

ERASE,<sprite_id>,<instance_id>
    Restore the background saved when this instance was drawn.

BOX,<x1>,<y1>,<x2>,<y2>,<type>[,<fill_char>]
    Draw a box decoration.

CLEAR
    Clear the offscreen buffer. No terminal output.

FLUSH
    Emit the current buffer to the terminal.

CFLUSH
    Clear the buffer AND emit. Equivalent to CLEAR + FLUSH.
"""
import os
import sys
import time

# --------------------------
# CONSTANTS
# --------------------------
BOX_GLYPHS = {
    0: None,
    1: ("\u2500", "\u2502", "\u250c", "\u2510", "\u2514", "\u2518"),
    2: ("\u2550", "\u2551", "\u2554", "\u2557", "\u255a", "\u255d"),
    3: ("\u2501", "\u2503", "\u250f", "\u2513", "\u2517", "\u251b"),
    4: ("\u2500", "\u2502", "\u256d", "\u256e", "\u2570", "\u256f"),
}
UL_ON = "\x1b[4m"
UL_OFF = "\x1b[24m"

# Frame skipping limits
MAX_SKIP_RATIO = 3   # Worst case: render 1 in 4 frames
EMA_ALPHA = 0.25     # Smoothing factor for timing EMAs

# --------------------------
# GLOBAL STATE
# --------------------------
width = 80
height = 24
backend = "terminal"
sprite_registry = {}
offscreen_buffer = []
instance_table = {}

# Frame timing control
frame_counter = 0
hardframe_mode = 0          # 0 = dynamic, N = skip N in N+1
skip_ratio = 0              # Current dynamic skip ratio (0..MAX_SKIP_RATIO)
flush_ema = 0.0             # EMA of flush duration (seconds)
frame_interval_ema = 0.0    # EMA of time between flushes (seconds)
last_flush_time = 0.0       # Monotonic time of last flush

# --------------------------
# INITIALIZATION
# --------------------------
def init_timing_control():
    global hardframe_mode, skip_ratio, flush_ema, frame_interval_ema
    global last_flush_time, frame_counter

    hardframe = os.environ.get("HARDFRAME", "0")
    try:
        hardframe_mode = max(0, int(hardframe))
    except ValueError:
        hardframe_mode = 0

    skip_ratio = 0
    flush_ema = 0.0
    frame_interval_ema = 0.0
    last_flush_time = 0.0
    frame_counter = 0

def init_system(w, h, be):
    global width, height, backend, offscreen_buffer
    width = w
    height = h
    backend = be
    clear_buffer()
    init_timing_control()

def clear_buffer():
    global offscreen_buffer
    offscreen_buffer = [[None for _ in range(width)] for _ in range(height)]

# --------------------------
# ANSI STRING PARSING
# --------------------------
def split_ansi_string(s):
    """Split string into (char, ansi_prefix) tuples.
    ANSI sequences are consumed and carried forward as current prefix."""
    result = []
    current_prefix = ""
    i = 0
    while i < len(s):
        if s[i] == '\x1b' and i + 1 < len(s) and s[i+1] == '[':
            end = s.find('m', i)
            if end == -1:
                current_prefix += s[i]
                i += 1
            else:
                current_prefix = s[i:end+1]
                i = end + 1
        elif s[i] == '\n':
            i += 1
        else:
            result.append((s[i], current_prefix))
            i += 1
    return result

# --------------------------
# FRAME SKIP DECISION
# --------------------------
def should_skip_frame():
    """Decide whether to drop this frame's terminal output.

    Returns True if this frame should be skipped (commands still
    processed internally, but no ANSI emitted to terminal).

    Decision logic:
      - If HARDFRAME is set, use fixed modulo pattern.
      - Otherwise, use dynamic skip_ratio based on measured
        flush duration vs frame interval.
    """
    global frame_counter
    frame_counter += 1

    if hardframe_mode > 0:
        return (frame_counter % (hardframe_mode + 1)) != 0

    if skip_ratio > 0:
        return (frame_counter % (skip_ratio + 1)) != 0

    return False

def update_timing_stats(flush_duration):
    """Update EMA stats after a successful flush."""
    global flush_ema, frame_interval_ema, last_flush_time, skip_ratio

    now = time.monotonic()
    if last_flush_time > 0:
        interval = now - last_flush_time

        if flush_ema == 0.0:
            flush_ema = flush_duration
        else:
            flush_ema = EMA_ALPHA * flush_duration + (1 - EMA_ALPHA) * flush_ema

        if frame_interval_ema == 0.0:
            frame_interval_ema = interval
        else:
            frame_interval_ema = EMA_ALPHA * interval + (1 - EMA_ALPHA) * frame_interval_ema

        if frame_interval_ema > 0:
            if flush_ema > 0.5 * frame_interval_ema:
                skip_ratio = min(skip_ratio + 1, MAX_SKIP_RATIO)
            elif flush_ema < 0.2 * frame_interval_ema and skip_ratio > 0:
                skip_ratio -= 1

    last_flush_time = now

# --------------------------
# SPRITE PARSING
# --------------------------
def parse_sprite_definition(full_cmd):
    """Parse a SPRITE command with robust data handling."""
    if not full_cmd.startswith("SPRITE,"):
        raise ValueError("Invalid SPRITE command")

    header_end = full_cmd.find(UL_ON) if UL_ON in full_cmd else None
    if header_end is None:
        parts = full_cmd.split(",")
        if len(parts) < 4:
            raise ValueError("SPRITE header expects at least 4 fields")
        sprite_id = int(parts[1])
        cols = int(parts[2])
        rows = int(parts[3])
        data_start = 4
        frame_data = ",".join(parts[data_start:])
    else:
        header = full_cmd[:header_end].rstrip(",")
        parts = header.split(",")
        if len(parts) != 4:
            raise ValueError("SPRITE header expects 4 fields, got %d" % len(parts))
        sprite_id = int(parts[1])
        cols = int(parts[2])
        rows = int(parts[3])
        frame_data = full_cmd[header_end:]

    if header_end is not None:
        all_content = []
        while UL_ON in frame_data:
            start = frame_data.find(UL_ON) + len(UL_ON)
            if UL_OFF not in frame_data[start:]:
                all_content.append(frame_data[start:])
                break
            end = frame_data.find(UL_OFF, start)
            all_content.append(frame_data[start:end])
            frame_data = frame_data[end + len(UL_OFF):]
        if frame_data and frame_data.strip():
            all_content.append(frame_data.strip())
        if not all_content:
            start = full_cmd.find(UL_ON) + len(UL_ON)
            all_content.append(full_cmd[start:])
    else:
        all_content = [frame_data]

    sprite = {
        "id": sprite_id,
        "width": cols,
        "height": rows,
        "frames": []
    }

    frames = []
    for content in all_content:
        cells = split_ansi_string(content)
        frame_grid = []
        row_idx = 0
        col_idx = 0
        for char, ansi_prefix in cells:
            if row_idx >= rows:
                break
            if len(frame_grid) <= row_idx:
                frame_grid.append([])
            if len(frame_grid[row_idx]) <= col_idx:
                frame_grid[row_idx].append((char, ansi_prefix))
            else:
                frame_grid[row_idx][col_idx] = (char, ansi_prefix)
            col_idx += 1
            if col_idx >= cols:
                col_idx = 0
                row_idx += 1
        while len(frame_grid) < rows:
            frame_grid.append([(" ", "") for _ in range(cols)])
        for row in frame_grid:
            while len(row) < cols:
                row.append((" ", ""))
        frames.append(frame_grid)

    sprite["frames"] = frames
    sprite_registry[sprite_id] = sprite
    return sprite

# --------------------------
# BACKING STORE
# --------------------------
def save_region(x, y, w, h):
    saved = []
    for row in range(y, y + h):
        row_data = []
        for col in range(x, x + w):
            if 0 <= row < height and 0 <= col < width:
                row_data.append(offscreen_buffer[row][col])
            else:
                row_data.append(None)
        saved.append(row_data)
    return saved

def restore_region(x, y, w, h, saved):
    for row in range(h):
        for col in range(w):
            buf_y = y + row
            buf_x = x + col
            if 0 <= buf_y < height and 0 <= buf_x < width:
                offscreen_buffer[buf_y][buf_x] = saved[row][col]

# --------------------------
# COMPOSITING
# --------------------------
def composite_cell(x, y, cell):
    if not (0 <= y < height and 0 <= x < width):
        return
    char, ansi_prefix = cell
    if char == ' ' and ansi_prefix == '':
        return
    offscreen_buffer[y][x] = (char, ansi_prefix)

def composite_sprite(sprite_id, x, y, frame=0):
    if sprite_id not in sprite_registry:
        sys.stderr.write("Warning: unknown sprite ID %d\n" % sprite_id)
        return
    sprite = sprite_registry[sprite_id]
    frame_idx = frame % len(sprite["frames"])
    sprite_frame = sprite["frames"][frame_idx]
    for row_idx, row in enumerate(sprite_frame):
        for col_idx, cell in enumerate(row):
            composite_cell(x + col_idx, y + row_idx, cell)

def composite_sprite_with_backing(sprite_id, instance_id, x, y, frame=0):
    if sprite_id not in sprite_registry:
        sys.stderr.write("Warning: unknown sprite ID %d\n" % sprite_id)
        return False
    key = (sprite_id, instance_id)
    sprite = sprite_registry[sprite_id]
    sw = sprite["width"]
    sh = sprite["height"]
    saved = save_region(x, y, sw, sh)
    composite_sprite(sprite_id, x, y, frame)
    instance_table[key] = {
        "sprite_id": sprite_id,
        "instance_id": instance_id,
        "x": x,
        "y": y,
        "width": sw,
        "height": sh,
        "saved_background": saved
    }
    return True

def erase_sprite(sprite_id, instance_id):
    key = (sprite_id, instance_id)
    if key not in instance_table:
        sys.stderr.write("Warning: unknown instance (%d,%d)\n" % (sprite_id, instance_id))
        return
    inst = instance_table[key]
    restore_region(inst["x"], inst["y"], inst["width"], inst["height"],
                   inst["saved_background"])
    del instance_table[key]

# --------------------------
# RENDERING
# --------------------------
def flush_buffer(clear=False):
    """Emit the current buffer to the terminal, or skip if dropping frame."""
    if should_skip_frame():
        sys.stderr.write("SKIP_FRAME\n")
        sys.stderr.flush()
        return

    render_start = time.monotonic()

    if clear:
        clear_buffer()

    if backend == "terminal":
        sys.stdout.write(_buffer_to_terminal_ansi())
        sys.stdout.flush()
    elif backend == "late":
        sys.stdout.write("LATE_PAYLOAD:%s\n" % _buffer_to_late_payload())
        sys.stdout.flush()

    flush_duration = time.monotonic() - render_start
    update_timing_stats(flush_duration)

def _buffer_to_terminal_ansi():
    """Render buffer to terminal ANSI escape sequences."""
    output = []
    output.append("\x1b[H")
    current_fg = None
    current_bg = None
    for y, row in enumerate(offscreen_buffer):
        for x, cell in enumerate(row):
            if cell is None:
                if current_fg is not None or current_bg is not None:
                    output.append("\x1b[0m")
                    current_fg = None
                    current_bg = None
                output.append(" ")
            else:
                char, ansi_prefix = cell
                if ansi_prefix:
                    output.append(ansi_prefix)
                output.append(char)
        if y < height - 1:
            output.append("\n")
    output.append("\x1b[0m")
    return "".join(output)

def _buffer_to_late_payload():
    payload = []
    for y, row in enumerate(offscreen_buffer):
        for x, cell in enumerate(row):
            if cell is not None:
                char, ansi_prefix = cell
                payload.append({"x": x, "y": y, "char": char, "ansi": ansi_prefix})
    return str(payload)

# --------------------------
# COMMAND PARSING
# --------------------------
# In parse_box_command:
def parse_box_command(line):
    parts = line.split(",", 10)
    if len(parts) < 6 or parts[0] != "BOX":
        raise ValueError("BOX expects: BOX,x1,y1,x2,y2,type[,ansi_color]")
    x1, y1, x2, y2 = int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4])
    box_type = int(parts[5])
    # Extract optional ANSI color, default to empty string
    ansi_color = parts[6].strip() if len(parts) > 6 else ""
    
    if x1 > x2: x1, x2 = x2, x1
    if y1 > y2: y1, y2 = y2, y1
    return {"x1": x1, "y1": y1, "x2": x2, "y2": y2, "type": box_type, "ansi": ansi_color}

# In composite_box, update the composite_cell calls to use the ansi color:
def composite_box(box):
    x1 = max(0, min(box["x1"], width - 1))
    y1 = max(0, min(box["y1"], height - 1))
    x2 = max(0, min(box["x2"], width - 1))
    y2 = max(0, min(box["y2"], height - 1))
    if x1 > x2 or y1 > y2:
        return
        
    ansi = box.get("ansi", "")
    
    if box["type"] == 0:
        for y in range(y1, y2 + 1):
            for x in range(x1, x2 + 1):
                offscreen_buffer[y][x] = None
        return
        
    glyphs = BOX_GLYPHS[box["type"]]
    if not glyphs:
        return
    h, v, tl, tr, bl, br = glyphs
    
    for x in range(x1, x2 + 1):
        if y1 < height and x < width:
            glyph = tl if x == x1 else tr if x == x2 else h
            composite_cell(x, y1, (glyph, ansi))
        if y2 != y1 and y2 < height and x < width:
            glyph = bl if x == x1 else br if x == x2 else h
            composite_cell(x, y2, (glyph, ansi))
            
    for y in range(y1 + 1, y2):
        if y >= height:
            continue
        if x1 < width:
            composite_cell(x1, y, (v, ansi))
        if x2 != x1 and x2 < width:
            composite_cell(x2, y, (v, ansi))
            
    if box["type"] == 5:
        fill_ch = box.get("fill_char", "\u2588") or "\u2588"
        for y in range(y1 + 1, y2):
            for x in range(x1 + 1, x2):
                if y < height and x < width:
                    composite_cell(x, y, (fill_ch, ansi))
    elif box["type"] == 6:
        for y in range(y1, y2 + 1):
            for x in range(x1, x2 + 1):
                if y < height and x < width:
                    glyph = "\u2593" if (x ^ y) & 1 else "\u2591"
                    composite_cell(x, y, (glyph, ansi))


def process_command(cmd):
    cmd = cmd.strip()
    if not cmd or cmd.startswith("#"):
        return

    if cmd.startswith("SPRITE"):
        parse_sprite_definition(cmd)
    elif cmd.startswith("DRAW"):
        parts = cmd.split(",")
        if len(parts) != 6 or parts[0] != "DRAW":
            raise ValueError("DRAW expects: DRAW,spriteID,instanceID,x,y,frame")
        sprite_id = int(parts[1])
        instance_id = int(parts[2])
        x = int(parts[3])
        y = int(parts[4])
        frame = int(parts[5])
        ok = composite_sprite_with_backing(sprite_id, instance_id, x, y, frame)
        if ok:
            sys.stderr.write("INSTANCE,%d,%d\n" % (sprite_id, instance_id))
            sys.stderr.flush()
    elif cmd.startswith("ERASE"):
        parts = cmd.split(",")
        if len(parts) != 3 or parts[0] != "ERASE":
            raise ValueError("ERASE expects: ERASE,spriteID,instanceID")
        erase_sprite(int(parts[1]), int(parts[2]))
    elif cmd.startswith("BOX"):
        box = parse_box_command(cmd)
        composite_box(box)
    elif cmd == "CLEAR":
        clear_buffer()
    elif cmd == "FLUSH":
        flush_buffer(clear=False)
    elif cmd == "CFLUSH":
        flush_buffer(clear=True)
    else:
        sys.stderr.write("Warning: unknown command %s\n" % cmd)

# --------------------------
# TESTING
# --------------------------
def run_tests():
    """Run basic unit tests for sprite parsing."""
    print("Running sprite bridge tests...")

    test1 = "SPRITE,1,6,3,asodin249182ABCDEF"
    print("\nTest 1: %s" % test1)
    sprite1 = parse_sprite_definition(test1)
    print("Result: %dx%d sprite with %d frame(s)" % (
        sprite1['width'], sprite1['height'], len(sprite1['frames'])))

    test2 = "SPRITE,2,4,2,abc\ndef"
    print("\nTest 2: %s" % test2)
    sprite2 = parse_sprite_definition(test2)
    print("Result: %dx%d sprite with %d frame(s)" % (
        sprite2['width'], sprite2['height'], len(sprite2['frames'])))

    test3 = "SPRITE,3,2,2,\x1b[31mR\x1b[0mA\x1b[32mG\x1b[0mB\n\x1b[33mY\x1b[0mE\x1b[34mB\x1b[0mL\x1b[35mO"
    print("\nTest 3: ANSI sprite")
    sprite3 = parse_sprite_definition(test3)
    print("Result: %dx%d sprite with %d frame(s)" % (
        sprite3['width'], sprite3['height'], len(sprite3['frames'])))

    test4 = "SPRITE,4,3,2,%sABC%s%sDEF%s" % (UL_ON, UL_OFF, UL_ON, UL_OFF)
    print("\nTest 4: Multi-frame sprite with UL markers")
    sprite4 = parse_sprite_definition(test4)
    print("Result: %dx%d sprite with %d frame(s)" % (
        sprite4['width'], sprite4['height'], len(sprite4['frames'])))

    test5 = "SPRITE,5,4,2,AB\nCD\x1b[31mE\x1b[0mF"
    print("\nTest 5: Mixed format sprite")
    sprite5 = parse_sprite_definition(test5)
    print("Result: %dx%d sprite with %d frame(s)" % (
        sprite5['width'], sprite5['height'], len(sprite5['frames'])))

    print("\nTests completed.")

def run_timing_tests():
    """Test frame timing and skip decision logic."""
    print("\nRunning timing tests...")

    print("\nTest 1: Default dynamic mode (HARDFRAME=0)")
    os.environ["HARDFRAME"] = "0"
    init_timing_control()
    print("  hardframe_mode: %d" % hardframe_mode)
    print("  Initial skip_ratio: %d" % skip_ratio)
    for i in range(5):
        skip = should_skip_frame()
        print("  Frame %d: skip=%s" % (i + 1, skip))

    print("\nTest 2: HARDFRAME=1 (skip 1 in 2)")
    os.environ["HARDFRAME"] = "1"
    init_timing_control()
    print("  hardframe_mode: %d" % hardframe_mode)
    for i in range(6):
        skip = should_skip_frame()
        print("  Frame %d: skip=%s" % (i + 1, skip))

    print("\nTest 3: HARDFRAME=2 (skip 2 in 3)")
    os.environ["HARDFRAME"] = "2"
    init_timing_control()
    print("  hardframe_mode: %d" % hardframe_mode)
    for i in range(6):
        skip = should_skip_frame()
        print("  Frame %d: skip=%s" % (i + 1, skip))

    print("\nTest 4: Dynamic skip adjustment")
    os.environ["HARDFRAME"] = "0"
    init_timing_control()
    global flush_ema, frame_interval_ema, last_flush_time
    last_flush_time = time.monotonic() - 0.033
    frame_interval_ema = 0.033
    update_timing_stats(0.025)
    print("  After slow flush: skip_ratio=%d" % skip_ratio)
    update_timing_stats(0.025)
    print("  After 2nd slow flush: skip_ratio=%d" % skip_ratio)
    update_timing_stats(0.001)
    print("  After fast flush: skip_ratio=%d" % skip_ratio)
    update_timing_stats(0.001)
    print("  After 2nd fast flush: skip_ratio=%d" % skip_ratio)

    print("\nTiming tests completed.")

# --------------------------
# MAIN
# --------------------------
def main():
    global width, height, backend

    if len(sys.argv) > 1 and sys.argv[1] == "-t":
        run_tests()
        return

    if len(sys.argv) > 1 and sys.argv[1] == "--timing":
        run_timing_tests()
        return

    # === FIX: Prefer os.get_terminal_size() over env vars ===
    # Try stdout first (fd=1), then stdin (fd=0), then stderr (fd=2)
    # Fall back to COLUMNS/LINES env vars, then 80x24
    w = h = None
    for fd in [1, 0, 2]:
        try:
            size = os.get_terminal_size(fd)
            w, h = size.columns, size.lines
            break
        except OSError:
            continue

    if w is None or h is None:
        w = int(os.environ.get("COLUMNS", 80))
        h = int(os.environ.get("LINES", 24))

    be = "terminal"
    if len(sys.argv) > 1 and sys.argv[1] == "--late":
        be = "late"

    init_system(w, h, be)

    while True:
        line = sys.stdin.readline()
        if not line:
            break
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        process_command(line)

if __name__ == "__main__":
    main()
