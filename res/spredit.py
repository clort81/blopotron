#!/usr/bin/env python3
"""spredit.py — Sprite editor for .ans sprite files.

A terminal TUI tool for:
  - Loading .ans sprite files (ANSI art sprites with metadata headers)
  - Reviewing sprites rendered with true-color ANSI
  - Assigning facing flags (N, S, W, E, WH, EH) per frame
  - Marking frames as half-shifted (top-half blank first row)
  - Creating and editing walk tables (frame index sequences per direction)
  - Previewing walk table animations in a preview area
  - Writing updated metadata back to .ans format

Usage:
  python3 spredit.py <file.ans>
"""

import sys
import os
import time
import select
import struct
import fcntl
import termios

DIRECTIONS = ["N", "S", "W", "E", "WH", "EH"]
FACING_LABELS = {
    "N": "North", "S": "South", "W": "West",
    "E": "East", "WH": "West-Half", "EH": "East-Half"
}


# ── ANSI Output Primitives (ECMA-48 24-bit only) ─────────────────────────────

def sgr(*codes):
    """Emit an SGR sequence.  Accepts ints or '38;2;r;g;b' strings."""
    return f"\x1b[{';'.join(str(c) for c in codes)}m"


def sgr_fg(r, g, b):
    return f"\x1b[38;2;{r};{g};{b}m"


def sgr_bg(r, g, b):
    return f"\x1b[48;2;{r};{g};{b}m"


def cur(y, x):
    """Cursor position (1-based)."""
    return f"\x1b[{y};{x}H"


def clr():
    """Clear screen and home cursor."""
    return "\x1b[2J\x1b[H"


def clr_line(y, x, w):
    """Clear w characters starting at (y,x) (1-based)."""
    return f"\x1b[{y};{x}H{' ' * w}"


def hide_cursor():
    return "\x1b[?25l"


def show_cursor():
    return "\x1b[?25h"


# ── ANSI Parsing ──────────────────────────────────────────────────────────────

def parse_ansi_line(raw):
    """Parse a raw ANSI line into a list of (fg_rgb, bg_rgb, char) tuples."""
    cells = []
    fg = (192, 192, 192)
    bg = (0, 0, 0)
    i = 0
    n = len(raw)
    while i < n:
        if raw[i] == "\x1b" and i + 1 < n and raw[i + 1] == "[":
            j = raw.find("m", i)
            if j == -1:
                break
            seq = raw[i + 2 : j]
            parts = seq.split(";")
            k = 0
            while k < len(parts):
                p = parts[k]
                if p == "38" and k + 4 < len(parts) and parts[k + 1] == "2":
                    fg = (int(parts[k+2]), int(parts[k+3]), int(parts[k+4]))
                    k += 5
                    continue
                elif p == "48" and k + 4 < len(parts) and parts[k + 1] == "2":
                    bg = (int(parts[k+2]), int(parts[k+3]), int(parts[k+4]))
                    k += 5
                    continue
                elif p == "0":
                    fg = (192, 192, 192)
                    bg = (0, 0, 0)
                k += 1
            i = j + 1
        else:
            ch = raw[i]
            i += 1
            if ch in ("\n", "\r"):
                continue
            cells.append((fg, bg, ch))
    return cells


# ── .ans File Parsing ────────────────────────────────────────────────────────

def parse_facing_str(s):
    """Parse a facing string like 'NSWE' or 'WHEH' into a set of tokens."""
    result = set()
    s = s.upper()
    i = 0
    while i < len(s):
        if s[i:i+2] in ("WH", "EH"):
            result.add(s[i:i+2])
            i += 2
        elif s[i] in "NSEW":
            result.add(s[i])
            i += 1
        else:
            i += 1
    return result


def parse_ans_file(path):
    """Parse an .ans sprite file.

    Returns:
        frames: list of frame dicts
        walk_tables: dict of direction -> list of frame indices
    """
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        lines = f.read().split("\n")

    frames = []
    walk_tables = {d: [] for d in DIRECTIONS}
    i = 0

    def peek_next_block(lines, pos):
        """Look ahead to see if the next non-blank line starts a new block."""
        j = pos
        while j < len(lines) and lines[j].strip() == "":
            j += 1
        if j < len(lines):
            return lines[j].strip()
        return ""

    while i < len(lines):
        line = lines[i].strip()

        # WalkTable section
        if line.startswith("WalkTable:"):
            wt_dir = line[len("WalkTable:"):].strip().upper()
            i += 1
            while i < len(lines) and lines[i].strip():
                for part in lines[i].strip().rstrip(",").split(","):
                    part = part.strip()
                    if part:
                        try:
                            walk_tables.setdefault(wt_dir, []).append(int(part))
                        except ValueError:
                            pass
                i += 1
            continue

        # Frame block
        if line.startswith("Index:"):
            frame = {
                "index": 0, "character": "", "facing_str": "", "frame_no": 0,
                "offset_x": 0, "offset_y": 0, "hotspot_x": 0, "hotspot_y": 0,
                "half_shift": False, "cells": [], "raw_lines": [],
                "facing_set": set(),
            }
            frame["index"] = int(line[len("Index:"):].strip())
            i += 1

            # Header fields until blank line
            while i < len(lines) and lines[i].strip() != "":
                hl = lines[i].strip()
                if hl.startswith("Index:"):
                    break
                if hl.startswith("Character:"):
                    frame["character"] = hl[len("Character:"):].strip()
                elif hl.startswith("Facing:"):
                    frame["facing_str"] = hl[len("Facing:"):].strip()
                elif hl.startswith("FrameNo:"):
                    frame["frame_no"] = int(hl[len("FrameNo:"):].strip())
                elif hl.startswith("OffsetX:"):
                    frame["offset_x"] = int(hl[len("OffsetX:"):].strip())
                elif hl.startswith("OffsetY:"):
                    frame["offset_y"] = int(hl[len("OffsetY:"):].strip())
                elif hl.startswith("HotspotX:"):
                    frame["hotspot_x"] = int(hl[len("HotspotX:"):].strip())
                elif hl.startswith("HotspotY:"):
                    frame["hotspot_y"] = int(hl[len("HotspotY:"):].strip())
                elif hl.startswith("HalfShift:"):
                    val = hl[len("HalfShift:"):].strip().lower()
                    frame["half_shift"] = val in ("true", "1", "yes")
                i += 1

            # Skip blank line separator
            if i < len(lines) and lines[i].strip() == "":
                i += 1

            frame["facing_set"] = parse_facing_str(frame["facing_str"])

            # Read sprite data lines until next Index: or end
            data_lines = []
            while i < len(lines):
                stripped = lines[i].strip()
                if stripped.startswith("Index:"):
                    break
                if stripped.startswith("WalkTable:"):
                    break
                if stripped.startswith("#"):
                    break
                if stripped == "" and len(data_lines) > 0:
                    if i + 1 < len(lines) and lines[i + 1].strip() == "":
                        break
                    nxt = peek_next_block(lines, i + 1)
                    if nxt.startswith("Index:") or nxt.startswith("WalkTable:") or nxt == "":
                        break
                data_lines.append(lines[i])
                i += 1

            frame["raw_lines"] = data_lines
            for dl in data_lines:
                frame["cells"].append(parse_ansi_line(dl))

            # Auto-detect half-shift
            if not frame["half_shift"] and len(frame["cells"]) > 0:
                first_row = frame["cells"][0]
                has_color = any(
                    bg != (0, 0, 0) and ch.strip() != ""
                    for (fg, bg, ch) in first_row
                )
                if not has_color:
                    frame["half_shift"] = True

            frames.append(frame)
        else:
            i += 1

    return frames, walk_tables


# ── .ans File Writing ────────────────────────────────────────────────────────

def write_ans_file(path, frames, walk_tables):
    """Write frames and walk tables back to .ans format."""
    lines = []
    for frame in frames:
        facing_str = ""
        for d in ["N", "S", "W", "E", "WH", "EH"]:
            if d in frame["facing_set"]:
                facing_str += d
        if not facing_str:
            facing_str = "NSEW"

        lines.append(f"Index: {frame['index']}")
        lines.append(f"Character: {frame['character']}")
        lines.append(f"Facing: {facing_str}")
        lines.append(f"FrameNo: {frame['frame_no']}")
        lines.append(f"OffsetX: {frame['offset_x']}")
        lines.append(f"OffsetY: {frame['offset_y']}")
        lines.append(f"HotspotX: {frame['hotspot_x']}")
        lines.append(f"HotspotY: {frame['hotspot_y']}")
        if frame["half_shift"]:
            lines.append("HalfShift: true")
        lines.append("")
        for dl in frame["raw_lines"]:
            lines.append(dl)
        lines.append("")

    if any(walk_tables.values()):
        lines.append("")
        lines.append("# Walk Tables (edited by spredit.py)")
        for direction in DIRECTIONS:
            wt = walk_tables.get(direction, [])
            if wt:
                idx_str = ", ".join(str(x) for x in wt)
                lines.append(f"WalkTable: {direction}")
                lines.append(f"  {idx_str}")
                lines.append("")

    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


# ── Terminal Helpers ─────────────────────────────────────────────────────────

def get_terminal_size():
    try:
        h, w = struct.unpack('hh', fcntl.ioctl(0, termios.TIOCGWINSZ, b'\x00' * 4))
        return h, w
    except Exception:
        return 24, 80


def render_cells(cells, max_rows=0, max_cols=0):
    """Render cell rows to an ANSI string (spriteclip-style run-length SGR).
    Tracks previous fg/bg per row to minimize escape sequences."""
    lines = []
    rows = cells[:max_rows] if max_rows else cells
    for row in rows:
        line, pf, pb = '', None, None
        for cx, (fg, bg, ch) in enumerate(row):
            if max_cols and cx >= max_cols:
                break
            display = ch
            if fg != pf:
                line += sgr_fg(*fg)
                pf = fg
            if bg != pb:
                line += sgr_bg(*bg)
                pb = bg
            line += display
        line += sgr(0)
        lines.append(line)
    return lines


def _read1(fd):
    """Read exactly 1 byte from fd, bypassing Python's stdin buffer.

    Critical: mixing select.select(fd) with sys.stdin.read() causes
    buffer desync — select sees the raw fd but sys.stdin may have
    already consumed bytes into its own internal buffer (or vice versa).
    os.read() operates on the same fd that select() polls, so they
    stay in sync.  This is why spriteclip0.98.py never has this issue:
    it never calls select at all, only blocking sys.stdin.read(1).
    """
    b = os.read(fd, 1)
    return b


def read_key(fd, timeout=None):
    """Read a keypress.  Returns a string.

    Regular chars: the char itself (e.g. 'q').
    Special keys:  '\x1b[A' (up), '\x1b[B' (down), etc.
    Timeout:      None (block) or float seconds; returns '' on timeout.

    All reads go through os.read(fd, 1) to stay in sync with select().
    """
    if timeout is not None:
        ready, _, _ = select.select([fd], [], [], timeout)
        if not ready:
            return ''
    b = _read1(fd)
    if b != b'\x1b':
        return b.decode('utf-8', errors='replace')
    # ESC received — wait to see if more bytes follow (escape sequence)
    if not select.select([fd], [], [], 0.2)[0]:
        return '\x1b'
    # Bytes follow: assemble the full CSI sequence with blocking reads
    nc = _read1(fd)
    if nc != b'[':
        # Not a CSI sequence — return standalone ESC, discard nc
        return '\x1b'
    # Read final byte(s) of CSI sequence
    seq = _read1(fd)
    if not seq:
        return '\x1b['
    while len(seq) < 16:
        if 0x40 <= seq[-1] <= 0x7E:  # final byte range
            break
        nxt = _read1(fd)
        if not nxt:
            break
        seq += nxt
    return '\x1b[' + seq.decode('utf-8', errors='replace')


# ── SGR attribute constants (ECMA-48) ────────────────────────────────────────
A_REV   = 7
A_BOLD  = 1
A_DIM   = 2
A_ULINE = 4


# ── Main Editor Class ─────────────────────────────────────────────────────────

class SpriteEditor:
    def __init__(self, filepath):
        self.filepath = filepath
        self.frames = []
        self.walk_tables = {d: [] for d in DIRECTIONS}
        self.load_file()
        self.sel = 0            # selected frame index
        self.mode = "browse"    # browse | facing | walktable | preview
        self.wt_dir = "N"       # current walk table direction being edited
        self.wt_build = []      # frame indices being assembled
        self.wt_phase = "dir"  # "dir" = picking direction, "add" = adding sprites
        self.wt_buf = ""        # (unused, kept for compat)
        self.pv_dir = "N"       # preview direction
        self.pv_step = 0        # current step in preview
        self.pv_pos = [0.0, 0.0]  # x, y position in preview
        self.pv_playing = True
        self.msg = ""
        self.msg_until = 0
        self.dirty = True
        self.term_h = 0
        self.term_w = 0

    # ── File I/O ──

    def load_file(self):
        self.frames, wt = parse_ans_file(self.filepath)
        for d in DIRECTIONS:
            if d in wt and wt[d]:
                self.walk_tables[d] = list(wt[d])

    def save_file(self):
        write_ans_file(self.filepath, self.frames, self.walk_tables)
        self.flash("Saved: " + self.filepath)

    def flash(self, msg, secs=2.0):
        self.msg = msg
        self.msg_until = time.time() + secs

    # ── Main Loop ──

    def run(self):
        fd = sys.stdin.fileno()
        old = termios.tcgetattr(fd)
        self.term_h, self.term_w = get_terminal_size()
        try:
            ns = termios.tcgetattr(fd)
            ns[3] &= ~(termios.ECHO | termios.ICANON)
            ns[6][termios.VMIN] = 1
            ns[6][termios.VTIME] = 0
            termios.tcsetattr(fd, termios.TCSANOW, ns)
            sys.stdout.write(hide_cursor())
            sys.stdout.flush()
            self._loop(fd)
        finally:
            termios.tcsetattr(fd, termios.TCSADRAIN, old)
            sys.stdout.write(sgr(0) + show_cursor() + cur(self.term_h, 1) + "\n")
            sys.stdout.flush()

    def _loop(self, fd):
        while True:
            if self.dirty:
                self._draw()
                self.dirty = False

            # Preview auto-advance with 150ms tick
            if self.mode == "preview" and self.pv_playing:
                self._preview_tick()
                self.dirty = True
                ch = read_key(fd, 0.15)
            else:
                ch = read_key(fd)

            if ch == '':
                continue

            if self.mode == "browse":
                self._key_browse(ch)
            elif self.mode == "facing":
                self._key_facing(ch)
            elif self.mode == "walktable":
                self._key_walktable(ch)
            elif self.mode == "preview":
                self._key_preview(ch)

            # Expire message
            if self.msg and time.time() > self.msg_until:
                self.msg = ""
                self.dirty = True

            # Check terminal resize
            h, w = get_terminal_size()
            if h != self.term_h or w != self.term_w:
                self.term_h, self.term_w = h, w
                self.dirty = True

    # ── Drawing ─────────────────────────────────────────────────────────────

    def _put(self, y, x, text, attr=0):
        """Write styled text at 1-based (y, x) via ANSI."""
        s = cur(y, x)
        if attr:
            s += sgr(attr)
        s += text
        if attr:
            s += sgr(0)
        sys.stdout.write(s)

    def _render_cells_at(self, cells, start_y, start_x, max_rows=0, max_cols=0):
        """Render cell rows at 1-based (start_y, start_x)."""
        lines = render_cells(cells, max_rows, max_cols)
        for ry, line in enumerate(lines):
            sys.stdout.write(cur(start_y + ry, start_x) + line)

    def _draw(self):
        out = []  # accumulate all output, write once
        h, w = self.term_h, self.term_w

        # Clear screen
        out.append(clr())

        # Title bar (row 1, reverse video)
        fname = os.path.basename(self.filepath)
        title = f" spredit  {fname}  {len(self.frames)} frames "
        out.append(cur(1, 1) + sgr(A_REV) + title[:w] + sgr(0))

        # Sprite strip (starting row 2, wraps to multiple rows)
        strip_y = 2
        strip_h = self._draw_strip(out, strip_y, h, w)

        # Separator
        sep_y = strip_y + strip_h
        out.append(cur(sep_y, 1) + sgr(A_DIM) + "\u2500" * w + sgr(0))

        # Content area
        content_y = sep_y + 1
        content_h = h - content_y - 1

        if self.mode == "browse":
            self._draw_browse(out, content_y, content_h, w)
        elif self.mode == "facing":
            self._draw_facing(out, content_y, content_h, w)
        elif self.mode == "walktable":
            self._draw_walktable(out, content_y, content_h, w)
        elif self.mode == "preview":
            self._draw_preview(out, content_y, content_h, w)

        # Status bar (bottom row)
        sy = h
        if self.msg:
            txt = self.msg[:w]
        elif self.mode == "browse":
            txt = "q:Quit  S:Save  f:Facing  c:WalkTable  ?:ToggleHalfShift  arrows:Select"
        elif self.mode == "facing":
            txt = "1-6 or N/S/W/E/A/D: Toggle facing   ESC: Back"
        elif self.mode == "walktable":
            if self.wt_phase == "dir":
                txt = "1-6:Select direction  Enter:Confirm  ESC:Cancel"
            else:
                txt = "arrows:Select sprite  Enter:Add  Del:Remove last  1-6:Switch dir  c:Clear  p:Preview  ESC:Save & Back"
        elif self.mode == "preview":
            txt = "SPACE:Pause  arrows:ChangeDir  ESC:Back"
        else:
            txt = ""
        out.append(cur(sy, 1) + sgr(A_REV) + txt[:w].ljust(w) + sgr(0))

        sys.stdout.write("".join(out))
        sys.stdout.flush()

    def _draw_strip(self, out, y, term_h, w):
        """Draw the thumbnail strip, wrapping to multiple rows.

        Returns the total height (rows) consumed by the strip.
        """
        thumb_w = 4
        per_thumb = thumb_w + 2          # 4 cols + 2 gap = 6 per sprite
        per_row = max(1, w // per_thumb)  # how many fit per row
        n_frames = len(self.frames)
        n_bands = max(1, (n_frames + per_row - 1) // per_row)

        # Adaptive thumbnail height: fewer rows when many bands needed
        # Must leave room for separator + at least 8 rows of content + status bar
        max_strip_h = term_h - 10         # absolute ceiling
        band_h_no_gap = 1 + 2             # label row + 2 thumb rows
        band_h_gap = band_h_no_gap + 1    # + inter-band gap
        # Estimate height: (n_bands - 1) * band_h_gap + band_h_no_gap
        est_h = (n_bands - 1) * band_h_gap + band_h_no_gap if n_bands > 1 else 5

        if est_h <= max_strip_h and n_bands == 1:
            thumb_h = 4   # plenty of room, show full 4-row thumbs
        elif est_h <= max_strip_h:
            thumb_h = 2   # multi-band fits, use 2-row thumbs
        else:
            # Even 2-row thumbs won't fit all bands — show what we can
            thumb_h = 2
            band_h_no_gap = 1 + thumb_h
            band_h_gap = band_h_no_gap + 1
            n_bands = max(1, (max_strip_h + 1) // band_h_gap)

        band_h = 1 + thumb_h  # height of one band (label + thumbs)
        gap = 1               # gap between bands
        fi = 0                # frame index
        cur_y = y

        for band in range(n_bands):
            if fi >= n_frames:
                break
            # Render one band of thumbnails at cur_y
            x = 1
            row_count = min(per_row, n_frames - fi)
            for col in range(row_count):
                frame = self.frames[fi]
                marker = ">" if fi == self.sel else " "
                hs = "H" if frame["half_shift"] else " "
                label = f"{marker}{fi+1}{hs}"
                attr = A_REV if fi == self.sel else 0
                if attr:
                    out.append(cur(cur_y, x) + sgr(attr) + label + sgr(0))
                else:
                    out.append(cur(cur_y, x) + label)
                lines = render_cells(frame["cells"], thumb_h, thumb_w)
                for ry, line in enumerate(lines):
                    out.append(cur(cur_y + 1 + ry, x) + line)
                x += per_thumb
                fi += 1
            cur_y += band_h
            # Add gap between bands (not after the last)
            if band < n_bands - 1 and fi < n_frames:
                cur_y += gap

        return cur_y - y  # total rows consumed

    def _draw_browse(self, out, y, h, w):
        if not self.frames:
            out.append(cur(y + 2, 2) + "No frames loaded.")
            return

        frame = self.frames[self.sel]
        hs_tag = " [HALF]" if frame["half_shift"] else ""
        fw = max((len(r) for r in frame["cells"]), default=0)
        fh = len(frame["cells"])
        info = (f"Frame {self.sel+1}/{len(self.frames)}  {frame['character']}  "
                f"Facing:{frame['facing_str'] or '-'}{hs_tag}  "
                f"Hotspot:({frame['hotspot_x']},{frame['hotspot_y']})  "
                f"Size:{fw}x{fh}")
        out.append(cur(y, 2) + info[:w-2])

        # Render selected sprite
        sprite_y = y + 2
        sprite_x = 2
        area_h = h - 4

        # Walk tables summary on the right
        wt_x = max(sprite_x + 14, w // 2)
        if wt_x < w - 24:
            out.append(cur(y, wt_x) + sgr(A_BOLD) + "Walk Tables:" + sgr(0))
            for di, d in enumerate(DIRECTIONS):
                wt = self.walk_tables.get(d, [])
                wt_s = ",".join(str(x+1) for x in wt) if wt else "(empty)"
                line = f"{d:>2}: [{wt_s}]"
                out.append(cur(y + 1 + di, wt_x) + line[:w - wt_x - 1])

        # Clear sprite area
        clear_w = min(w - 4, wt_x - sprite_x - 2) if wt_x < w - 24 else w - 4
        for ry in range(area_h):
            out.append(clr_line(sprite_y + ry, sprite_x, clear_w))

        # Render sprite cells
        lines = render_cells(frame["cells"], area_h)
        for ry, line in enumerate(lines):
            out.append(cur(sprite_y + ry, sprite_x) + line)

    def _draw_facing(self, out, y, h, w):
        if not self.frames:
            return
        frame = self.frames[self.sel]
        out.append(cur(y, 2) + sgr(A_BOLD) +
                   f"Facing Editor \u2014 Frame {self.sel+1}: {frame['character']}" +
                   sgr(0))
        current = frame["facing_set"]
        for i, d in enumerate(DIRECTIONS):
            active = d in current
            marker = "[X]" if active else "[ ]"
            label = f"  {marker} {d:>2}  {FACING_LABELS[d]}"
            attr = A_REV if active else 0
            if attr:
                out.append(cur(y + 2 + i, 4) + sgr(attr) + label + sgr(0))
            else:
                out.append(cur(y + 2 + i, 4) + label)

        # Frame overview
        out.append(cur(y + 10, 2) + sgr(A_ULINE) + "All frames:" + sgr(0))
        cols_per = max(1, (w - 4) // 22)
        for i, f in enumerate(self.frames):
            row_i = i // cols_per
            col_i = i % cols_per
            fs = f["facing_str"] or "-"
            hs = "H" if f["half_shift"] else " "
            sel = ">" if i == self.sel else " "
            line = f"{sel}{i+1:>3}:{fs:<8}{hs}"
            ty = y + 12 + row_i
            tx = 2 + col_i * 22
            if ty < y + h - 1 and tx < w - 10:
                out.append(cur(ty, tx) + line)

    def _draw_walktable(self, out, y, h, w):
        if self.wt_phase == "dir":
            self._draw_wt_dir(out, y, h, w)
        else:
            self._draw_wt_add(out, y, h, w)

    def _draw_wt_dir(self, out, y, h, w):
        """Draw direction-selection sub-phase."""
        out.append(cur(y, 2) + sgr(A_BOLD) +
                   "Select Walk Table Direction" + sgr(0))
        out.append(cur(y + 1, 2) + "Press 1-6 to highlight, Enter to confirm, ESC to cancel")

        for i, d in enumerate(DIRECTIONS):
            sel = ">" if d == self.wt_dir else " "
            cnt = len(self.walk_tables.get(d, []))
            label = f"{sel}[{i+1}] {d:>2} ({FACING_LABELS[d]}) — {cnt} frames"
            attr = A_REV if d == self.wt_dir else 0
            if attr:
                out.append(cur(y + 3 + i, 4) + sgr(attr) + label + sgr(0))
            else:
                out.append(cur(y + 3 + i, 4) + label)

    def _draw_wt_add(self, out, y, h, w):
        """Draw sprite-adding sub-phase."""
        dir_label = FACING_LABELS.get(self.wt_dir, self.wt_dir)
        out.append(cur(y, 2) + sgr(A_BOLD) +
                   f"Walk {self.wt_dir} ({dir_label}) — Adding Sprites" + sgr(0))

        # Build sequence display
        build_disp = ", ".join(str(x+1) for x in self.wt_build) if self.wt_build else "(empty)"
        out.append(cur(y + 2, 2) + f"Sequence: [{build_disp}]"[:w-4])

        # Highlight which sprites are already in the build
        in_build_set = set(self.wt_build)

        # Show all directions summary (compact, 2 columns)
        ref_y = y + 4
        out.append(cur(ref_y, 2) + sgr(A_DIM) + "All walk tables:" + sgr(0))
        cols_per = max(1, (w - 4) // 38)
        for i, d in enumerate(DIRECTIONS):
            ri = i // cols_per
            ci = i % cols_per
            cnt = len(self.walk_tables.get(d, []))
            cur_dir = " ▶" if d == self.wt_dir else "  "
            line = f"{cur_dir} {d:>2}: {cnt}f"
            tx = 2 + ci * 38
            ty = ref_y + 1 + ri
            if ty < y + h - 2:
                out.append(cur(ty, tx) + line)

        # Selected sprite preview
        spr_y = ref_y + 4
        if spr_y < y + h - 2 and self.frames:
            frame = self.frames[self.sel]
            in_b = self.sel in in_build_set
            tag = " [IN SEQUENCE]" if in_b else ""
            info = f"Selected: frame {self.sel+1}/{len(self.frames)}{tag}"
            out.append(cur(spr_y, 2) + sgr(A_BOLD) + info + sgr(0))

            # Render the selected sprite larger
            sprite_y = spr_y + 2
            sprite_x = 2
            area_h = y + h - 2 - sprite_y
            if area_h > 0:
                lines = render_cells(frame["cells"], area_h)
                for ry, line in enumerate(lines):
                    out.append(cur(sprite_y + ry, sprite_x) + line)

    def _draw_preview(self, out, y, h, w):
        dir_label = FACING_LABELS.get(self.pv_dir, self.pv_dir)
        wt = self.walk_tables.get(self.pv_dir, [])
        status = "PLAY" if self.pv_playing else "PAUSE"
        wt_s = ",".join(str(x+1) for x in wt)
        out.append(cur(y, 2) + sgr(A_BOLD) +
                   f"Preview {self.pv_dir} ({dir_label}) [{status}]  [{wt_s}]" +
                   sgr(0))

        # Preview box
        py = y + 2
        pw = min(w - 4, 50)
        ph = h - 6
        px = 2

        # Draw box with box-drawing characters
        out.append(cur(py, px) + "\u250c" + "\u2500" * (pw - 2) + "\u2510")
        for ry in range(ph - 2):
            out.append(cur(py + 1 + ry, px) + "\u2502" +
                       sgr_bg(0, 0, 0) + " " * (pw - 2) + sgr(0) + "\u2502")
        out.append(cur(py + ph - 1, px) + "\u2514" + "\u2500" * (pw - 2) + "\u2518")

        # Draw current frame inside box
        if wt:
            fidx = wt[self.pv_step % len(wt)]
            if 0 <= fidx < len(self.frames):
                frame = self.frames[fidx]
                dx = px + 1 + int(self.pv_pos[0])
                dy = py + 1 + int(self.pv_pos[1])
                # Clip rows to preview box interior
                visible = []
                for ry, row in enumerate(frame["cells"]):
                    screen_y = dy + ry
                    if screen_y >= py + ph - 1:
                        break
                    if screen_y < py + 1:
                        continue
                    visible.append(row)
                if visible:
                    lines = render_cells(visible)
                    for ry, line in enumerate(lines):
                        out.append(cur(dy + ry, dx) + line)

        # Step info
        out.append(cur(y + h - 3, 2) +
                   f"Step:{self.pv_step}  Pos:({self.pv_pos[0]:.1f},{self.pv_pos[1]:.1f})")

    def _preview_tick(self):
        wt = self.walk_tables.get(self.pv_dir, [])
        if not wt:
            self.pv_playing = False
            return
        self.pv_step += 1
        d = self.pv_dir
        if d in ("E", "EH"):
            self.pv_pos[0] += 1.0
        elif d in ("W", "WH"):
            self.pv_pos[0] -= 1.0
        elif d == "S":
            self.pv_pos[1] += 0.5
        elif d == "N":
            self.pv_pos[1] -= 0.5
        # Loop position
        if self.pv_step % len(wt) == 0:
            self.pv_pos = [0.0, 0.0]

    # ── Key Handlers ──

    def _is_key(self, ch, *targets):
        """Check if ch matches any target (char or ESC sequence)."""
        return ch in targets

    def _key_browse(self, ch):
        if ch == 'q':
            sys.exit(0)
        elif ch == 'S':
            self.save_file()
        elif ch == 'f':
            self.mode = "facing"
        elif ch == 'c':
            self.mode = "walktable"
            self.wt_phase = "dir"
            self.wt_build = list(self.walk_tables.get(self.wt_dir, []))
        elif ch == '?':
            if self.frames:
                f = self.frames[self.sel]
                f["half_shift"] = not f["half_shift"]
                tag = "ON" if f["half_shift"] else "OFF"
                self.flash(f"Half-shift {tag} (frame {self.sel+1})")
        elif ch == '\x1b[C' or ch == 'l':
            if self.sel < len(self.frames) - 1:
                self.sel += 1
        elif ch == '\x1b[D' or ch == 'h':
            if self.sel > 0:
                self.sel -= 1
        elif ch == '\x1b[B' or ch == 'j':
            self.sel = min(self.sel + 6, len(self.frames) - 1)
        elif ch == '\x1b[A' or ch == 'k':
            self.sel = max(self.sel - 6, 0)
        elif ch == '\x1b[H' or ch == 'g':
            self.sel = 0
        elif ch == '\x1b[F' or ch == 'G':
            self.sel = max(0, len(self.frames) - 1)
        self.dirty = True

    def _key_facing(self, ch):
        if ch == '\x1b':
            self.mode = "browse"
            self.dirty = True
            return
        frame = self.frames[self.sel]
        toggle_map = {
            '1': 'N', 'n': 'N', 'N': 'N',
            '2': 'S', 's': 'S',
            '3': 'W', 'w': 'W',
            '4': 'E', 'e': 'E',
            '5': 'WH', 'a': 'WH',
            '6': 'EH', 'd': 'EH',
        }
        if ch in toggle_map:
            d = toggle_map[ch]
            if d in frame["facing_set"]:
                frame["facing_set"].discard(d)
            else:
                frame["facing_set"].add(d)
            fs = ""
            for dd in ["N", "S", "W", "E", "WH", "EH"]:
                if dd in frame["facing_set"]:
                    fs += dd
            frame["facing_str"] = fs
            self.flash(f"Facing: {fs}")
            self.dirty = True

    def _key_walktable(self, ch):
        if self.wt_phase == "dir":
            self._key_wt_dir(ch)
        else:
            self._key_wt_add(ch)

    def _key_wt_dir(self, ch):
        """Sub-phase: selecting a walk-table direction."""
        if ch == '\x1b':
            self.mode = "browse"
            self.dirty = True
            return
        dir_map = {'1': 'N', '2': 'S', '3': 'W', '4': 'E', '5': 'WH', '6': 'EH'}
        if ch in dir_map:
            self.wt_dir = dir_map[ch]
            self.wt_build = list(self.walk_tables.get(self.wt_dir, []))
            self.dirty = True
            return
        if ch in ('\n', '\r'):
            # Confirm direction → switch to add phase
            self.wt_build = list(self.walk_tables.get(self.wt_dir, []))
            self.wt_phase = "add"
            self.flash(f"Editing walk {self.wt_dir} — use arrows + Enter to add sprites")
            self.dirty = True

    def _key_wt_add(self, ch):
        """Sub-phase: adding/removing sprites from the walk table."""
        if ch == '\x1b':
            # Save and return to browse
            self.walk_tables[self.wt_dir] = list(self.wt_build)
            self.flash(f"Walk {self.wt_dir} saved: {[x+1 for x in self.wt_build]}")
            self.wt_phase = "dir"
            self.mode = "browse"
            self.dirty = True
            return
        # Arrow keys navigate sprite selection
        if ch == '\x1b[C' or ch == 'l':
            if self.sel < len(self.frames) - 1:
                self.sel += 1
            self.dirty = True
            return
        if ch == '\x1b[D' or ch == 'h':
            if self.sel > 0:
                self.sel -= 1
            self.dirty = True
            return
        if ch == '\x1b[B' or ch == 'j':
            self.sel = min(self.sel + 6, len(self.frames) - 1)
            self.dirty = True
            return
        if ch == '\x1b[A' or ch == 'k':
            self.sel = max(self.sel - 6, 0)
            self.dirty = True
            return
        # Enter: add selected sprite
        if ch in ('\n', '\r'):
            self.wt_build.append(self.sel)
            self.flash(f"Added frame {self.sel+1} to walk {self.wt_dir}")
            self.dirty = True
            return
        # Delete / Backspace: remove most recent
        if ch in ('\x7f', '\x08'):
            if self.wt_build:
                removed = self.wt_build.pop()
                self.flash(f"Removed frame {removed+1}")
            self.dirty = True
            return
        # 1-6: quick-switch direction (save current first)
        dir_map = {'1': 'N', '2': 'S', '3': 'W', '4': 'E', '5': 'WH', '6': 'EH'}
        if ch in dir_map:
            self.walk_tables[self.wt_dir] = list(self.wt_build)
            self.wt_dir = dir_map[ch]
            self.wt_build = list(self.walk_tables.get(self.wt_dir, []))
            self.flash(f"Switched to {self.wt_dir}")
            self.dirty = True
            return
        # c: clear build
        if ch in ('c', 'C'):
            self.wt_build = []
            self.flash(f"Cleared walk {self.wt_dir}")
            self.dirty = True
            return
        # p: preview
        if ch in ('p', 'P'):
            if self.wt_build:
                self.walk_tables[self.wt_dir] = list(self.wt_build)
                self.pv_dir = self.wt_dir
                self.pv_step = 0
                self.pv_pos = [0.0, 0.0]
                self.pv_playing = True
                self.mode = "preview"
            else:
                self.flash("Add frames to table first")
            self.dirty = True
            return


    def _key_preview(self, ch):
        if ch == '\x1b':
            self.pv_playing = False
            self.wt_phase = "add"
            self.mode = "walktable"
            self.dirty = True
        elif ch == ' ':
            self.pv_playing = not self.pv_playing
            self.dirty = True
        elif ch == '\x1b[C' or ch == 'l':
            self._next_pv_dir(1)
        elif ch == '\x1b[D' or ch == 'h':
            self._next_pv_dir(-1)

    def _next_pv_dir(self, step):
        idx = DIRECTIONS.index(self.pv_dir) if self.pv_dir in DIRECTIONS else 0
        idx = (idx + step) % len(DIRECTIONS)
        self.pv_dir = DIRECTIONS[idx]
        self.pv_step = 0
        self.pv_pos = [0.0, 0.0]
        self.dirty = True


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 spredit.py <file.ans>")
        print("")
        print("Sprite editor for .ans sprite files.")
        print("  - Browse sprites with arrow keys")
        print("  - f: Edit facing flags per frame")
        print("  - c: Create/edit walk tables")
        print("  - ?: Toggle half-shift flag")
        print("  - S: Save changes to .ans file")
        print("  - q: Quit")
        sys.exit(1)

    filepath = sys.argv[1]
    if not os.path.exists(filepath):
        print(f"Error: {filepath} not found")
        sys.exit(1)

    editor = SpriteEditor(filepath)
    editor.run()


if __name__ == "__main__":
    main()
