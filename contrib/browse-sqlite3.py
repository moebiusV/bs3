#!/usr/bin/env python3
import sqlite3
import curses
import sys
import os
import subprocess
import tempfile
import re
import json
import locale
import time
import unicodedata
from typing import List, Tuple, Union

try:
    from _displaywidth import display_width as _c_display_width
    from _displaywidth import truncate_to_width as _c_truncate_to_width
    from _displaywidth import skip_display_cols as _c_skip_display_cols
    _USE_C_WIDTH = True
except ImportError:
    _USE_C_WIDTH = False


class SQLiteBrowser:
    def __init__(self, db_path: str):
        self.db_path = db_path
        self.conn = sqlite3.connect(db_path)
        self.cursor = self.conn.cursor()
        self.current_view = 'tables'  # 'tables', 'rows', or 'edit'
        self.selected_table_idx = 0
        self.selected_row_idx = 0
        self.selected_field_idx = 0
        self.table_scroll_offset = 0
        self.row_scroll_offset = 0
        self.row_horizontal_offset = 0
        self.field_scroll_offset = 0
        self.current_table_data = []
        self.current_table_name = ''
        self.current_columns = []
        self.primary_keys = {}

        # Edit mode state
        self.edit_values = []
        self.edit_original_values = []

        # Safety mode and command input
        self.safe_mode = True
        self.command_mode = False
        self.command_input = ""

        # Prompt mode for confirmations
        self.prompt_mode = False
        self.prompt_text = ""
        self.prompt_action = None

        # Help mode
        self.help_mode = False
        self.help_scroll_offset = 0

        # Sort overlay state
        self.sort_mode = False
        self.sort_context = ''  # 'tables', 'columns', 'rows'
        self.sort_items = []
        self.sort_selected_idx = 0
        self.sort_grabbed_idx = -1  # -1 = not grabbed
        self.sort_scroll_offset = 0
        self.sort_directions = {}   # col_name -> 'ASC' or 'DESC' (rows view)
        self.sort_active_cols = []  # ordered list of active sort columns (rows view)

        # Cached config
        self.table_display_order = None

        # Message for feedback
        self.message = None

        # Find/filter state
        self.find_mode = False        # True when bottom-bar input is active
        self.find_input = ""          # Current text being typed
        self.find_filter = ""         # Active filter string (applied)
        self.find_where = ""          # SQL WHERE clause built from find_filter
        self.find_params = []         # Parameters for the WHERE clause
        self.unfilt_row_count = 0     # Total row count before filtering
        self.find_history = []        # Past search strings (oldest first)
        self.find_history_idx = -1    # -1 = new entry; 0..n = browsing history
        self.find_saved_input = ""    # Stash current input when browsing history

        # Find dialog state (per-column filter overlay)
        self.find_dialog = False           # True when dialog is open
        self.find_dialog_inputs = []       # list of strings, one per column
        self.find_dialog_focus = 0         # index of focused text field
        self.find_dialog_scroll = 0        # scroll offset for many columns
        self.find_dialog_and_flags = []    # per-field bool: True = AND (required)

        # Quit flag for ^C
        self.quit_flag = False

        # Cached formatted row strings (one per data row, without
        # leading space or horizontal offset applied).  Built once in
        # load_table_data; _format_data_row reads from here.
        self._row_strings = []

        # Load config then tables (config needed for table ordering)
        self.load_all_config()
        self.tables = self.get_tables()

    # -- Color pair constants (set in run() after curses.start_color) --
    # These are indices into curses color pairs.
    C_NORMAL = 1       # Normal text: yellow on blue
    C_SELECTED = 2     # Selected/highlight: white on green
    C_BRIGHT = 3       # Headers/modified: white on blue
    C_TITLE = 4        # Title bar: white on cyan
    C_STATUS = 5       # Status bar: black on cyan
    C_ERROR = 6        # Error/warning: white on red
    C_HELP = 7         # Help window body: black on cyan
    C_HELP_KEY = 8     # Help window keys: white on cyan
    C_BORDER = 9       # Borders/separators: cyan on blue

    def get_tables(self) -> List[Tuple[str, List[str]]]:
        """Returns list of (table_name, [column_names])"""
        self.cursor.execute("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' AND name != '_browse_config' ORDER BY name")
        tables = []
        for (table_name,) in self.cursor.fetchall():
            self.cursor.execute(f"PRAGMA table_info({table_name})")
            table_info = self.cursor.fetchall()
            columns = [row[1] for row in table_info]
            primary_keys = [row[1] for row in table_info if row[5] > 0]
            self.primary_keys[table_name] = primary_keys
            tables.append((table_name, columns))
        return self.apply_table_order(tables)

    # ----------------------------------------------------------------
    # Read-only detection
    # ----------------------------------------------------------------

    def _readonly_message(self) -> str:
        """Build a status message for read-only database failures."""
        import stat
        try:
            st = os.stat(self.db_path)
            mode = stat.filemode(st.st_mode)
            try:
                import pwd, grp
                owner = pwd.getpwuid(st.st_uid).pw_name
                group = grp.getgrgid(st.st_gid).gr_name
                perms = f"{mode} {owner}:{group}"
            except (ImportError, KeyError):
                perms = mode
            return f"Database is read-only ({perms})"
        except OSError:
            return "Database is read-only"

    # Config persistence (_browse_config table)
    # ----------------------------------------------------------------

    def ensure_config_table(self):
        """Create _browse_config table if it doesn't exist"""
        try:
            self.cursor.execute("CREATE TABLE IF NOT EXISTS _browse_config (key TEXT PRIMARY KEY, value TEXT NOT NULL)")
            self.conn.commit()
        except sqlite3.OperationalError:
            pass  # read-only database

    def load_config(self, key: str):
        """Read a JSON config value, return None if missing"""
        try:
            self.cursor.execute("SELECT value FROM _browse_config WHERE key = ?", (key,))
            row = self.cursor.fetchone()
            if row:
                return json.loads(row[0])
        except sqlite3.Error:
            pass
        return None

    def save_config(self, key: str, value):
        """Upsert a JSON config value, creating the table if needed"""
        try:
            self.ensure_config_table()
            json_val = json.dumps(value)
            self.cursor.execute("INSERT OR REPLACE INTO _browse_config (key, value) VALUES (?, ?)", (key, json_val))
            self.conn.commit()
        except sqlite3.OperationalError:
            pass  # read-only database

    def load_all_config(self):
        """Load cached config values at startup"""
        self.table_display_order = self.load_config('table_order')

    def apply_table_order(self, tables):
        """Reorder table list per stored order. Ordered tables first, then new ones."""
        if not self.table_display_order:
            return tables
        order = self.table_display_order
        ordered = []
        remaining = list(tables)
        for name in order:
            for t in remaining:
                if t[0] == name:
                    ordered.append(t)
                    remaining.remove(t)
                    break
        return ordered + remaining

    def load_table_data(self, table_name: str):
        """Load all rows from a table with rowid for updates/deletes"""
        self.current_table_name = table_name
        # Get base column list from tables
        base_cols = []
        for name, cols in self.tables:
            if name == table_name:
                base_cols = list(cols)
                break

        # Apply stored column order
        col_order = self.load_config(f'column_order:{table_name}')
        if col_order:
            ordered = []
            remaining = list(base_cols)
            for c in col_order:
                if c in remaining:
                    ordered.append(c)
                    remaining.remove(c)
            self.current_columns = ordered + remaining
        else:
            self.current_columns = base_cols

        # Build ORDER BY from stored row sort config
        order_clause = ""
        row_sort = self.load_config(f'row_sort:{table_name}')
        if row_sort:
            parts = []
            for col, direction in row_sort:
                if col in self.current_columns:
                    parts.append(f"{col} {direction}")
            if parts:
                order_clause = " ORDER BY " + ", ".join(parts)

        # Get unfiltered row count
        self.cursor.execute(f"SELECT COUNT(*) FROM {table_name}")
        self.unfilt_row_count = self.cursor.fetchone()[0]

        # Use explicit column list in SELECT, with optional WHERE filter
        col_list = ", ".join(self.current_columns)
        where_clause = ""
        query_params = []
        if self.find_where:
            where_clause = f" WHERE {self.find_where}"
            query_params = list(self.find_params)
        self.cursor.execute(f"SELECT rowid, {col_list} FROM {table_name}{where_clause}{order_clause}", query_params)
        self.current_table_data = self.cursor.fetchall()
        self._cache_row_strings()

    def _cache_row_strings(self):
        """Pre-format all row strings (whitespace-cleaned, pipe-joined).

        Stores strings WITHOUT leading space or horizontal offset so that
        _format_data_row can apply the current offset cheaply.
        """
        strings = []
        for row in self.current_table_data:
            cleaned = []
            for val in row[1:]:
                if val is not None:
                    cleaned.append(re.sub(r'\s+', ' ', str(val).strip()))
                else:
                    cleaned.append("NULL")
            strings.append(" | ".join(cleaned))
        self._row_strings = strings

    def delete_current_row(self) -> bool:
        """Delete the currently selected row"""
        if not self.current_table_data or self.selected_row_idx >= len(self.current_table_data):
            return False
        try:
            row = self.current_table_data[self.selected_row_idx]
            rowid = row[0]
            self.cursor.execute(f"DELETE FROM {self.current_table_name} WHERE rowid = ?", (rowid,))
            self.conn.commit()
            self.load_table_data(self.current_table_name)
            if self.selected_row_idx >= len(self.current_table_data):
                self.selected_row_idx = max(0, len(self.current_table_data) - 1)
            return True
        except sqlite3.OperationalError:
            self.message = self._readonly_message()
            return False
        except sqlite3.Error:
            return False

    def drop_current_table(self) -> bool:
        """Drop the currently selected table"""
        if self.current_view != 'tables' or self.selected_table_idx >= len(self.tables):
            return False
        table_name = self.tables[self.selected_table_idx][0]
        try:
            self.cursor.execute(f"DROP TABLE {table_name}")
            self.conn.commit()
            self.tables = self.get_tables()
            self.selected_table_idx = max(0, min(self.selected_table_idx, len(self.tables) - 1))
            return True
        except sqlite3.OperationalError:
            self.message = self._readonly_message()
            return False
        except sqlite3.Error:
            return False

    def enter_edit_mode(self):
        """Enter edit mode for the currently selected row"""
        if not self.current_table_data or self.selected_row_idx >= len(self.current_table_data):
            return
        row = self.current_table_data[self.selected_row_idx]
        self.edit_values = list(row[1:])
        self.edit_original_values = list(row[1:])
        self.selected_field_idx = 0
        self.field_scroll_offset = 0
        self.current_view = 'edit'

    def save_edit_changes(self) -> bool:
        """Save changes from edit mode back to database"""
        try:
            row = self.current_table_data[self.selected_row_idx]
            rowid = row[0]
            set_clause = ", ".join([f"{col} = ?" for col in self.current_columns])
            query = f"UPDATE {self.current_table_name} SET {set_clause} WHERE rowid = ?"
            self.cursor.execute(query, self.edit_values + [rowid])
            self.conn.commit()
            self.load_table_data(self.current_table_name)
            return True
        except sqlite3.OperationalError:
            self.message = self._readonly_message()
            return False
        except sqlite3.Error:
            return False

    def _char_width(self, char: str) -> int:
        """Return display width of a single character.

        Zero-width categories (combining marks, format chars like ZWJ)
        return 0.  East Asian wide/fullwidth return 2.  Variation selectors
        (U+FE00-FE0F) are kept at width 1.

        Symbols (category So) at U+2600+ are treated as width 2: modern
        terminals render them as emoji (wide) even though Unicode classifies
        many as neutral/ambiguous width (e.g. U+2764 HEAVY BLACK HEART).
        Overcounting is always safer than undercounting (truncation vs overflow).
        """
        # Fast path: printable ASCII is always width 1.  This avoids
        # expensive unicodedata lookups for the vast majority of
        # database content.
        cp = ord(char)
        if 0x20 <= cp <= 0x7E:
            return 1
        cat = unicodedata.category(char)
        if cat in ('Mn', 'Me', 'Cf'):
            if '\ufe00' <= char <= '\ufe0f':
                return 1  # variation selectors: keep as width 1
            return 0
        ea = unicodedata.east_asian_width(char)
        if ea in ('W', 'F'):
            return 2
        # Emoji-style symbols: terminals render wide despite neutral ea_width
        if cat == 'So' and cp >= 0x2600:
            return 2
        return 1

    def get_display_width(self, text: str) -> int:
        """Calculate display width considering East Asian and zero-width characters"""
        if text.isascii():
            return len(text)
        if _USE_C_WIDTH:
            return _c_display_width(text)
        width = 0
        for char in text:
            width += self._char_width(char)
        return width

    def skip_display_cols(self, text: str, cols: int) -> str:
        """Skip *cols* display columns from the start of *text* and return the rest."""
        if text.isascii():
            return text[cols:] if cols < len(text) else ''
        if _USE_C_WIDTH:
            return _c_skip_display_cols(text, cols)
        w = 0
        for i, char in enumerate(text):
            cw = self._char_width(char)
            if cw == 0:
                continue  # zero-width chars don't count toward columns
            if w + cw > cols:
                return text[i:]
            w += cw
            if w >= cols:
                # Also skip trailing zero-width chars that modify this character
                j = i + 1
                while j < len(text) and self._char_width(text[j]) == 0:
                    j += 1
                return text[j:]
        return ''

    def truncate_to_width(self, text: str, max_w: int) -> str:
        """Truncate text to fit within display width"""
        if text.isascii():
            return text[:max_w] if len(text) > max_w else text
        if _USE_C_WIDTH:
            return _c_truncate_to_width(text, max_w)
        w = 0
        parts = []
        for char in text:
            cw = self._char_width(char)
            if cw == 0:
                parts.append(char)  # always include zero-width chars
                continue
            if w + cw > max_w:
                break
            parts.append(char)
            w += cw
        return ''.join(parts)

    def safe_addstr(self, stdscr, y: int, x: int, text: str, attr=0):
        """Safely add string to screen, handling boundaries and wide characters.
        Writes up to the full width including the last column. The curses error
        from writing to the bottom-right cell is caught (the char is still drawn)."""
        height, width = stdscr.getmaxyx()
        if y >= height or y < 0 or x >= width or x < 0:
            return
        max_w = width - x
        display_text = self.truncate_to_width(text, max_w)
        try:
            stdscr.addstr(y, x, display_text, attr)
        except curses.error:
            pass

    def safe_addstr_full_width(self, stdscr, y: int, x: int, text: str, attr=0):
        """Add string padded to fill the full line width.

        Truncates text to fit and pads with spaces to the end of the
        line in a single addstr call to minimize curses overhead."""
        height, width = stdscr.getmaxyx()
        if y >= height or y < 0 or x >= width or x < 0:
            return
        max_w = width - x
        truncated = self.truncate_to_width(text, max_w)
        dw = self.get_display_width(truncated)
        pad = max_w - dw
        if pad > 0:
            truncated = truncated + ' ' * pad
        # Trim to max_w chars as a safety net (for pure-ASCII paths
        # where display_width == len, this is exact)
        try:
            stdscr.addstr(y, x, truncated, attr)
        except curses.error:
            pass

    # ----------------------------------------------------------------
    # Help system
    # ----------------------------------------------------------------

    def get_help_lines(self) -> List[Tuple[str, str]]:
        """Return context-sensitive help as list of (key, description) tuples.
        An empty key string means it's a section header."""
        common = [
            ("", "-- General (all screens) --"),
            ("q / Esc", "Quit program without saving"),
            ("^C", "Quit program without saving"),
            ("F1", "Toggle this help screen"),
            (":", "Enter command mode"),
        ]

        if self.current_view == 'tables':
            specific = [
                ("", "-- Table List --"),
                ("k / Up", "Move selection up"),
                ("j / Down", "Move selection down"),
                ("K / PgUp", "Page up"),
                ("J / PgDn", "Page down"),
                ("Tab", "Next table"),
                ("l / Right / Enter", "Open selected table"),
                ("d / Del", "Drop selected table"),
                ("", "  SAFE: prompts | UNSAFE: immediate"),
                ("o", "Set table display order"),
                ("/", "Search/filter tables"),
            ]
        elif self.current_view == 'rows':
            specific = [
                ("", "-- Row Browser --"),
                ("k / Up", "Move selection up"),
                ("j / Down", "Move selection down"),
                ("K / PgUp", "Page up"),
                ("J / PgDn", "Page down"),
                ("Tab", "Next row"),
                ("l / Right / Enter", "Edit selected row"),
                ("h / Left", "Go back to table list"),
                ("d / Del", "Delete selected row"),
                ("", "  SAFE: prompts | UNSAFE: immediate"),
                ("Shift+Left", "Scroll display left"),
                ("Shift+Right", "Scroll display right"),
                ("o", "Sort rows by column(s)"),
                ("/", "Search rows (any field)"),
                ("f", "Find by column (dialog)"),
            ]
        else:  # edit
            specific = [
                ("", "-- Field Editor --"),
                ("k / Up", "Move to previous field"),
                ("j / Down", "Move to next field"),
                ("K / PgUp", "Page up through fields"),
                ("J / PgDn", "Page down through fields"),
                ("Tab", "Next field"),
                ("Enter / v", "View field in pager ($PAGER)"),
                ("l / Right", "View in pager (if truncated)"),
                ("e", "Edit field inline"),
                ("h / Left", "Back to rows (prompts if unsaved)"),
                ("E", "Edit in external editor ($EDITOR)"),
                ("d / Del", "Set field to NULL"),
                ("", "  SAFE: prompts | UNSAFE: immediate"),
                ("s", "Save all changes to database"),
                ("o", "Set column display order"),
                ("/", "Search/highlight fields"),
                ("", ""),
                ("", "-- Inline Editing (after e) --"),
                ("Left / ^B", "Move cursor left"),
                ("Right / ^F", "Move cursor right"),
                ("^A", "Jump to beginning"),
                ("^E", "Jump to end"),
                ("Backspace", "Delete before cursor"),
                ("Del / ^D", "Delete at cursor"),
                ("^W", "Delete word backward"),
                ("^K", "Kill to end of line"),
                ("^U", "Kill to beginning of line"),
                ("^T", "Transpose characters"),
                ("^L", "Redraw screen"),
                ("Tab", "Save + next field"),
                ("Enter", "Save + stop editing"),
                ("Esc / ^G", "Abort (discard changes)"),
                ("^C", "Quit program immediately"),
            ]

        mode_info = [
            ("", ""),
            ("", "-- Safety Mode --"),
            ("", f"  Current mode: {'SAFE' if self.safe_mode else 'UNSAFE'}"),
            ("", "  :safe    - Enable safe mode (default)"),
            ("", "  :unsafe  - Disable safe mode"),
            ("", "  Destructive actions (d key) require"),
            ("", "  confirmation in SAFE mode."),
        ]

        return specific + common + mode_info

    def draw_help_overlay(self, stdscr, height: int, width: int):
        """Draw a Turbo Pascal style help window overlay"""
        help_lines = self.get_help_lines()

        # Calculate window dimensions
        box_w = min(62, width - 4)
        box_h = min(len(help_lines) + 4, height - 2)
        start_y = max(0, (height - box_h) // 2)
        start_x = max(0, (width - box_w) // 2)
        inner_w = box_w - 4   # border + 1 padding each side
        key_col_w = 20

        help_attr = curses.color_pair(self.C_HELP)
        key_attr = curses.color_pair(self.C_HELP_KEY) | curses.A_BOLD
        border_attr = curses.color_pair(self.C_TITLE) | curses.A_BOLD

        # Box drawing characters: prefer Unicode, fall back to ASCII
        try:
            "\u2500".encode(locale.getpreferredencoding())
            TL, TR, BL, BR, HZ, VT = "\u250c", "\u2510", "\u2514", "\u2518", "\u2500", "\u2502"
        except (UnicodeEncodeError, LookupError):
            TL, TR, BL, BR, HZ, VT = "+", "+", "+", "+", "-", "|"

        # Top border with centered title
        view_name = {"tables": "Table List", "rows": "Row Browser", "edit": "Field Editor"}.get(self.current_view, "")
        title = f" Help: {view_name} "
        pad_total = box_w - 2 - len(title)
        pad_left = pad_total // 2
        pad_right = pad_total - pad_left
        top_border = TL + HZ * pad_left + title + HZ * pad_right + TR
        self.safe_addstr(stdscr, start_y, start_x, top_border, border_attr)

        # Helper to draw a content row with matching border color
        def draw_row(y, content):
            """Draw │ content │ with borders in border_attr, content in help_attr"""
            self.safe_addstr(stdscr, y, start_x, VT, border_attr)
            self.safe_addstr(stdscr, y, start_x + 1, " " + content + " ", help_attr)
            self.safe_addstr(stdscr, y, start_x + box_w - 1, VT, border_attr)

        # Blank line after title
        blank_content = " " * inner_w
        draw_row(start_y + 1, blank_content)

        # Content area
        content_h = box_h - 4
        content_start_y = start_y + 2

        visible_lines = help_lines[self.help_scroll_offset:self.help_scroll_offset + content_h]
        for i in range(content_h):
            cy = content_start_y + i
            if i < len(visible_lines):
                key_text, desc = visible_lines[i]
                if not key_text:
                    line_content = desc[:inner_w].ljust(inner_w)
                else:
                    key_part = key_text[:key_col_w].ljust(key_col_w)
                    desc_part = desc[:inner_w - key_col_w]
                    line_content = (key_part + desc_part).ljust(inner_w)
                draw_row(cy, line_content)
                if key_text:
                    self.safe_addstr(stdscr, cy, start_x + 2, key_text[:key_col_w].ljust(key_col_w), key_attr)
            else:
                draw_row(cy, blank_content)

        # Blank line before bottom border
        draw_row(start_y + box_h - 2, blank_content)

        # Bottom border with hint
        can_scroll = len(help_lines) > content_h
        if can_scroll:
            pos = self.help_scroll_offset + 1
            total = max(1, len(help_lines) - content_h + 1)
            hint = f" j/k:Scroll  Other:Close ({pos}/{total}) "
        else:
            hint = " Any key to close "
        pad_total = box_w - 2 - len(hint)
        pad_left = pad_total // 2
        pad_right = pad_total - pad_left
        bot_border = BL + HZ * pad_left + hint + HZ * pad_right + BR
        self.safe_addstr(stdscr, start_y + box_h - 1, start_x, bot_border, border_attr)

    # ----------------------------------------------------------------
    # Status and UI chrome
    # ----------------------------------------------------------------

    def get_status_text(self) -> str:
        """Generate status text based on current view and mode"""
        mode_str = "SAFE" if self.safe_mode else "UNSAFE"
        nav = "hjkl/\u2190\u2191\u2193\u2192:Nav"   # ←↑↓→
        pg  = "JK/PgUp PgDn"
        if self.current_view == 'tables':
            return f" {mode_str} | {nav} | {pg} | Tab:\u2192 | l:Open | d:Drop | o:Order | /:Search | q:Quit | F1:Help "
        elif self.current_view == 'rows':
            return f" {mode_str} | {nav} | {pg} | Tab:\u2192 | l:Edit | d:Del | h:Back | o:Sort | /:Search | f:Find | q:Quit | F1:Help "
        else:  # edit
            modified = self.has_unsaved_changes()
            base = f" {mode_str} | {nav} | {pg} | Tab:\u2192 | Enter:View | e:Edit | E:Ext | s:Save | h:Back | o:Order | /:Search | q:Quit | F1:Help "
            if modified:
                return base.rstrip() + " [UNSAVED!] "
            return base

    def draw_command_bar(self, stdscr, height: int, width: int):
        """Draw the vi-style command bar"""
        prompt = f": {self.command_input}"
        self.safe_addstr_full_width(stdscr, height - 1, 0, prompt, curses.color_pair(self.C_STATUS))
        stdscr.move(height - 1, min(self.get_display_width(prompt), width - 1))

    def draw_prompt_bar(self, stdscr, height: int, width: int):
        """Draw the prompt bar for confirmations"""
        self.safe_addstr_full_width(stdscr, height - 1, 0, self.prompt_text, curses.color_pair(self.C_ERROR))

    def draw_status_bar(self, stdscr, height: int, width: int):
        """Draw the status bar at the bottom"""
        if self.message:
            self.safe_addstr_full_width(stdscr, height - 1, 0, f" {self.message} ", curses.color_pair(self.C_ERROR) | curses.A_BOLD)
        else:
            self.safe_addstr_full_width(stdscr, height - 1, 0, self.get_status_text(), curses.color_pair(self.C_STATUS))

    # ----------------------------------------------------------------
    # View drawing
    # ----------------------------------------------------------------

    def get_filtered_tables(self):
        """Return tables filtered by the current find_filter (for tables view)"""
        if self.find_filter and self.current_view == 'tables':
            pattern = self.find_filter.lower()
            return [(n, c) for n, c in self.tables if pattern in n.lower()]
        return self.tables

    def draw_tables_view(self, stdscr, height: int, width: int):
        """Draw the table list view content"""
        title = f" SQLite Browser - {self.db_path} "
        if self.find_filter:
            title = title.rstrip() + f" [Find: {self.find_filter}] "
        self.safe_addstr_full_width(stdscr, 0, 0, title, curses.color_pair(self.C_TITLE) | curses.A_BOLD)
        try:
            "\u2500".encode(locale.getpreferredencoding())
            hr_char = "\u2500"
        except (UnicodeEncodeError, LookupError):
            hr_char = "-"
        self.safe_addstr_full_width(stdscr, 1, 0, hr_char * (width - 1), curses.color_pair(self.C_BORDER))

        filtered_tables = self.get_filtered_tables()
        visible_height = height - 3
        start_idx = self.table_scroll_offset
        end_idx = min(start_idx + visible_height, len(filtered_tables))

        for i in range(start_idx, end_idx):
            table_name, columns = filtered_tables[i]
            line_num = 2 + (i - start_idx)
            columns_str = ", ".join(columns)
            display_line = f" {table_name}: {columns_str}"
            if i == self.selected_table_idx:
                self.safe_addstr_full_width(stdscr, line_num, 0, display_line, curses.color_pair(self.C_SELECTED))
            else:
                self.safe_addstr_full_width(stdscr, line_num, 0, display_line, curses.color_pair(self.C_NORMAL))
        # Clear any remaining content lines (including gap line before status bar)
        for line_num in range(2 + end_idx - start_idx, height - 1):
            self.safe_addstr_full_width(stdscr, line_num, 0, "", curses.color_pair(self.C_NORMAL))

        if not self.command_mode and not self.prompt_mode and not self.find_mode:
            self.draw_status_bar(stdscr, height, width)

    def _draw_single_table_row(self, stdscr, table_idx: int, height: int, width: int):
        """Redraw a single table list row at its screen position."""
        filtered_tables = self.get_filtered_tables()
        start_idx = self.table_scroll_offset
        line_num = 2 + (table_idx - start_idx)
        if line_num < 2 or line_num >= height - 1 or table_idx >= len(filtered_tables):
            return
        table_name, columns = filtered_tables[table_idx]
        columns_str = ", ".join(columns)
        display_line = f" {table_name}: {columns_str}"
        if table_idx == self.selected_table_idx:
            attr = curses.color_pair(self.C_SELECTED)
        else:
            attr = curses.color_pair(self.C_NORMAL)
        self.safe_addstr_full_width(stdscr, line_num, 0, display_line, attr)

    def _format_data_row(self, row_idx: int) -> str:
        """Format a single data row for display using cached strings."""
        return " " + self.skip_display_cols(self._row_strings[row_idx], self.row_horizontal_offset)

    def _draw_single_row(self, stdscr, row_idx: int, height: int, width: int):
        """Redraw a single data row at its screen position."""
        start_idx = self.row_scroll_offset
        line_num = 2 + (row_idx - start_idx)
        if line_num < 2 or line_num >= height - 1:
            return
        display = self._format_data_row(row_idx)
        if row_idx == self.selected_row_idx:
            attr = curses.color_pair(self.C_SELECTED)
        else:
            attr = curses.color_pair(self.C_NORMAL)
        self.safe_addstr_full_width(stdscr, line_num, 0, display, attr)

    def draw_rows_view(self, stdscr, height: int, width: int):
        """Draw the table rows view content (full redraw)."""
        if self.find_filter and self.find_where:
            title = f" Table: {self.current_table_name} ({len(self.current_table_data)}/{self.unfilt_row_count} rows) [Find: {self.find_filter}] "
        else:
            title = f" Table: {self.current_table_name} ({len(self.current_table_data)} rows) "
        self.safe_addstr_full_width(stdscr, 0, 0, title, curses.color_pair(self.C_TITLE) | curses.A_BOLD)

        header = " | ".join(self.current_columns)
        self.safe_addstr_full_width(stdscr, 1, 0, self.skip_display_cols(header, self.row_horizontal_offset), curses.color_pair(self.C_BRIGHT) | curses.A_UNDERLINE)

        visible_height = height - 3
        start_idx = self.row_scroll_offset
        end_idx = min(start_idx + visible_height, len(self.current_table_data))

        for i in range(start_idx, end_idx):
            self._draw_single_row(stdscr, i, height, width)
        # Clear any remaining content lines (including gap line before status bar)
        for line_num in range(2 + end_idx - start_idx, height - 1):
            self.safe_addstr_full_width(stdscr, line_num, 0, "", curses.color_pair(self.C_NORMAL))

        if not self.command_mode and not self.prompt_mode and not self.find_mode:
            self.draw_status_bar(stdscr, height, width)

    def has_unsaved_changes(self) -> bool:
        """Check if there are unsaved changes in edit mode"""
        return self.edit_values != self.edit_original_values

    def draw_edit_view(self, stdscr, height: int, width: int):
        """Draw the edit view for a single row"""
        row_num = self.selected_row_idx + 1
        modified = self.has_unsaved_changes()
        title = f" Edit Row {row_num} of {self.current_table_name} "
        if modified:
            title += "[MODIFIED] "
        if self.find_filter:
            title = title.rstrip() + f" [Find: {self.find_filter}] "
        self.safe_addstr_full_width(stdscr, 0, 0, title, curses.color_pair(self.C_TITLE) | curses.A_BOLD)
        try:
            "\u2500".encode(locale.getpreferredencoding())
            hr_char = "\u2500"
        except (UnicodeEncodeError, LookupError):
            hr_char = "-"
        self.safe_addstr_full_width(stdscr, 1, 0, hr_char * (width - 1), curses.color_pair(self.C_BORDER))

        visible_height = height - 3
        start_idx = self.field_scroll_offset
        end_idx = min(start_idx + visible_height, len(self.current_columns))

        line_num = 2
        for i in range(start_idx, end_idx):
            col_name = self.current_columns[i]
            value = self.edit_values[i]
            if value is not None:
                value_str = re.sub(r'\s+', ' ', str(value).strip())
            else:
                value_str = "NULL"
            col_prefix = f" {col_name}: "
            max_display = width - self.get_display_width(col_prefix) - 10
            if self.get_display_width(value_str) > max_display:
                avail_w = max(0, max_display - self.get_display_width("... [LONG]"))
                display_value = self.truncate_to_width(value_str, avail_w) + "... [LONG]"
            else:
                display_value = value_str

            # Check if this field matches the find filter (for highlighting)
            find_match = False
            if self.find_filter and self.current_view == 'edit':
                pattern = self.find_filter.lower()
                if pattern in col_name.lower() or pattern in value_str.lower():
                    find_match = True

            if i == self.selected_field_idx:
                self.safe_addstr_full_width(stdscr, line_num, 0, col_prefix + display_value, curses.color_pair(self.C_SELECTED))
            elif self.edit_values[i] != self.edit_original_values[i]:
                self.safe_addstr_full_width(stdscr, line_num, 0, col_prefix + display_value + " *", curses.color_pair(self.C_BRIGHT) | curses.A_BOLD)
            elif find_match:
                self.safe_addstr_full_width(stdscr, line_num, 0, col_prefix + display_value, curses.color_pair(self.C_BRIGHT) | curses.A_BOLD)
            else:
                self.safe_addstr_full_width(stdscr, line_num, 0, col_prefix + display_value, curses.color_pair(self.C_NORMAL))
            line_num += 1
        # Clear any remaining content lines (including gap line before status bar)
        for ln in range(line_num, height - 1):
            self.safe_addstr_full_width(stdscr, ln, 0, "", curses.color_pair(self.C_NORMAL))

        if not self.command_mode and not self.prompt_mode and not self.find_mode:
            self.draw_status_bar(stdscr, height, width)

    # ----------------------------------------------------------------
    # Main loop
    # ----------------------------------------------------------------

    def init_colors(self):
        """Initialize colors using exact CGA palette RGB values when possible.

        The CGA palette used by Turbo Pascal's IDE has specific RGB values
        that differ from most modern terminal themes. If the terminal supports
        redefining colors (can_change_color), we set the exact CGA hex values.
        Otherwise we fall back to the standard curses color constants.

        CGA palette (hex -> curses 0-1000 scale):
          Black   #000000 ->    0,   0,   0
          Blue    #0000AA ->    0,   0, 667
          Green   #00AA00 ->    0, 667,   0
          Cyan    #00AAAA ->    0, 667, 667
          Red     #AA0000 ->  667,   0,   0
          Yellow  #FFFF55 -> 1000,1000, 333
          White   #FFFFFF -> 1000,1000,1000
        """
        curses.start_color()

        if curses.can_change_color() and curses.COLORS >= 24:
            # Define exact CGA colors using indices 16+ to avoid
            # clobbering the terminal's own palette (curses.wrapper
            # restores on exit either way, but this is cleaner).
            CGA_BLACK  = 16
            CGA_BLUE   = 17
            CGA_GREEN  = 18
            CGA_CYAN   = 19
            CGA_RED    = 20
            CGA_YELLOW = 21
            CGA_WHITE  = 22

            curses.init_color(CGA_BLACK,     0,    0,    0)  # #000000
            curses.init_color(CGA_BLUE,      0,    0,  667)  # #0000AA
            curses.init_color(CGA_GREEN,     0,  667,    0)  # #00AA00
            curses.init_color(CGA_CYAN,      0,  667,  667)  # #00AAAA
            curses.init_color(CGA_RED,     667,    0,    0)  # #AA0000
            curses.init_color(CGA_YELLOW, 1000, 1000,  333)  # #FFFF55
            curses.init_color(CGA_WHITE,  1000, 1000, 1000)  # #FFFFFF

            curses.init_pair(self.C_NORMAL,   CGA_YELLOW, CGA_BLUE)   # Desktop text
            curses.init_pair(self.C_SELECTED, CGA_WHITE,  CGA_GREEN)  # Selection bar
            curses.init_pair(self.C_BRIGHT,   CGA_WHITE,  CGA_BLUE)   # Headers/modified
            curses.init_pair(self.C_TITLE,    CGA_WHITE,  CGA_CYAN)   # Title bar
            curses.init_pair(self.C_STATUS,   CGA_BLACK,  CGA_CYAN)   # Status bar
            curses.init_pair(self.C_ERROR,    CGA_WHITE,  CGA_RED)    # Error messages
            curses.init_pair(self.C_HELP,     CGA_BLACK,  CGA_CYAN)   # Help body
            curses.init_pair(self.C_HELP_KEY, CGA_WHITE,  CGA_CYAN)   # Help key column
            curses.init_pair(self.C_BORDER,   CGA_CYAN,   CGA_BLUE)   # Borders
        else:
            # Fallback: use standard curses color constants.
            # Colors will depend on the terminal's theme.
            curses.init_pair(self.C_NORMAL,   curses.COLOR_YELLOW,  curses.COLOR_BLUE)
            curses.init_pair(self.C_SELECTED, curses.COLOR_WHITE,   curses.COLOR_GREEN)
            curses.init_pair(self.C_BRIGHT,   curses.COLOR_WHITE,   curses.COLOR_BLUE)
            curses.init_pair(self.C_TITLE,    curses.COLOR_WHITE,   curses.COLOR_CYAN)
            curses.init_pair(self.C_STATUS,   curses.COLOR_BLACK,   curses.COLOR_CYAN)
            curses.init_pair(self.C_ERROR,    curses.COLOR_WHITE,   curses.COLOR_RED)
            curses.init_pair(self.C_HELP,     curses.COLOR_BLACK,   curses.COLOR_CYAN)
            curses.init_pair(self.C_HELP_KEY, curses.COLOR_WHITE,   curses.COLOR_CYAN)
            curses.init_pair(self.C_BORDER,   curses.COLOR_CYAN,    curses.COLOR_BLUE)

    def run(self, stdscr):
        if curses.has_colors():
            self.init_colors()
            stdscr.bkgd(' ', curses.color_pair(self.C_NORMAL))

        curses.curs_set(0)
        stdscr.erase()

        FRAME_SEC = 1.0 / 30
        up_keys = {curses.KEY_UP, ord('k')}
        down_keys = {curses.KEY_DOWN, ord('j')}
        pgup_keys = {curses.KEY_PPAGE, ord('K')}
        pgdn_keys = {curses.KEY_NPAGE, ord('J')}
        left_keys = {curses.KEY_SLEFT}
        right_keys = {curses.KEY_SRIGHT}
        nav_pairs = {
            'vert':  (up_keys, down_keys),
            'page':  (pgup_keys, pgdn_keys),
            'horiz': (left_keys, right_keys),
        }
        all_nav_keys = up_keys | down_keys | pgup_keys | pgdn_keys | left_keys | right_keys

        _full_redraw = True
        _prev_view = None
        _prev_row_idx = -1
        _prev_scroll = -1
        _prev_field_idx = -1
        _prev_field_scroll = -1
        _prev_table_idx = -1
        _prev_table_scroll = -1

        while True:
            frame_start = time.monotonic()
            height, width = stdscr.getmaxyx()

            # --- Partial vs full redraw decision ---
            did_partial = False
            if (not _full_redraw
                    and self.current_view == _prev_view
                    and not self.find_mode and not self.command_mode
                    and not self.prompt_mode and not self.help_mode
                    and not self.sort_mode and not self.find_dialog):
                if (self.current_view == 'rows'
                        and self.row_scroll_offset == _prev_scroll
                        and self.row_horizontal_offset == getattr(self, '_prev_horiz', self.row_horizontal_offset)
                        and self.selected_row_idx != _prev_row_idx):
                    self._draw_single_row(stdscr, _prev_row_idx, height, width)
                    self._draw_single_row(stdscr, self.selected_row_idx, height, width)
                    self.draw_status_bar(stdscr, height, width)
                    did_partial = True
                elif (self.current_view == 'tables'
                        and self.table_scroll_offset == _prev_table_scroll
                        and self.selected_table_idx != _prev_table_idx):
                    self._draw_single_table_row(stdscr, _prev_table_idx, height, width)
                    self._draw_single_table_row(stdscr, self.selected_table_idx, height, width)
                    self.draw_status_bar(stdscr, height, width)
                    did_partial = True

            if not did_partial:
                stdscr.erase()

                if self.current_view == 'tables':
                    self.draw_tables_view(stdscr, height, width)
                elif self.current_view == 'rows':
                    self.draw_rows_view(stdscr, height, width)
                elif self.current_view == 'edit':
                    self.draw_edit_view(stdscr, height, width)

                if self.find_mode:
                    self.draw_find_bar(stdscr, height, width)
                    curses.curs_set(1)
                elif self.command_mode:
                    self.draw_command_bar(stdscr, height, width)
                    curses.curs_set(1)
                elif self.prompt_mode:
                    self.draw_prompt_bar(stdscr, height, width)
                    curses.curs_set(0)
                else:
                    curses.curs_set(0)

                if self.help_mode:
                    self.draw_help_overlay(stdscr, height, width)

                if self.sort_mode:
                    self.draw_sort_overlay(stdscr, height, width)

                if self.find_dialog:
                    self.draw_find_dialog(stdscr, height, width)

            if not did_partial:
                stdscr.touchwin()
            stdscr.refresh()

            try:
                key = stdscr.getch()
            except KeyboardInterrupt:
                key = 3  # Treat as ^C

            # --- frame-paced input batching ---
            # Navigation keys on the same axis (e.g. up/down) are
            # collected into a net displacement.  To keep scrolling
            # smooth we pace frames at 30 fps: after the first nav
            # key arrives we sleep only the time remaining in the
            # current frame (subtracting render + getch time already
            # spent) then drain whatever accumulated.

            # Classify the first key into a navigation axis
            axis = None
            for ax, (neg, pos) in nav_pairs.items():
                if key in neg or key in pos:
                    axis = ax
                    break

            repeat_count = 1
            drained = 0
            if axis is not None:
                neg_keys, pos_keys = nav_pairs[axis]
                all_axis_keys = neg_keys | pos_keys
                direction = 1 if key in pos_keys else -1
                net = direction  # first key already counted

                # Sleep only the remaining frame budget so that
                # render_time + sleep = FRAME_SEC.  If rendering
                # already exceeded the budget, skip the sleep.
                elapsed = time.monotonic() - frame_start
                remaining = FRAME_SEC - elapsed
                if remaining > 0:
                    time.sleep(remaining)

                stdscr.nodelay(True)
                while True:
                    try:
                        next_key = stdscr.getch()
                    except KeyboardInterrupt:
                        next_key = 3
                    if next_key == -1:
                        break
                    drained += 1
                    if next_key in all_axis_keys:
                        net += 1 if next_key in pos_keys else -1
                    else:
                        curses.ungetch(next_key)
                        break
                stdscr.nodelay(False)

                # Convert net displacement to a canonical key + count
                if net > 0:
                    key = min(pos_keys)     # canonical positive key
                    repeat_count = net
                elif net < 0:
                    key = min(neg_keys)     # canonical negative key
                    repeat_count = -net
                else:
                    # Net zero — movements cancelled out; skip dispatch
                    repeat_count = 0
            else:
                # Non-navigation key: drain same-key repeats instantly
                stdscr.nodelay(True)
                while True:
                    try:
                        next_key = stdscr.getch()
                    except KeyboardInterrupt:
                        next_key = 3
                    if next_key == -1:
                        break
                    drained += 1
                    if next_key == key:
                        repeat_count += 1
                    else:
                        curses.ungetch(next_key)
                        break
                stdscr.nodelay(False)

            self.message = None  # Reset message after display

            # Help mode: scroll or dismiss
            if self.help_mode:
                help_lines = self.get_help_lines()
                help_content_h = min(height - 6, 40)  # visible content lines
                max_scroll = max(0, len(help_lines) - help_content_h)
                if key == curses.KEY_DOWN or key == ord('j'):
                    self.help_scroll_offset = min(max_scroll, self.help_scroll_offset + repeat_count)
                elif key == curses.KEY_UP or key == ord('k'):
                    self.help_scroll_offset = max(0, self.help_scroll_offset - repeat_count)
                elif key == curses.KEY_NPAGE or key == ord(' '):
                    self.help_scroll_offset = min(max_scroll, self.help_scroll_offset + help_content_h * repeat_count)
                elif key == curses.KEY_PPAGE:
                    self.help_scroll_offset = max(0, self.help_scroll_offset - help_content_h * repeat_count)
                else:
                    # Any other key dismisses help
                    self.help_mode = False
                    self.help_scroll_offset = 0
                continue

            # Sort overlay mode
            if self.sort_mode:
                self.handle_sort_input(key)
                if self.quit_flag:
                    break
                continue

            # Find dialog mode (per-column filter overlay)
            if self.find_dialog:
                if key == 27:  # Esc - cancel
                    self.find_dialog = False
                elif key == ord('\n') or key == curses.KEY_ENTER:
                    self.find_dialog = False
                    self.apply_find_dialog()
                elif key == 9:  # Tab - next field
                    self.find_dialog_focus = (self.find_dialog_focus + 1) % len(self.current_columns)
                elif key == curses.KEY_BTAB:  # Shift+Tab - previous field
                    self.find_dialog_focus = (self.find_dialog_focus - 1) % len(self.current_columns)
                elif key == curses.KEY_DOWN:
                    self.find_dialog_focus = min(self.find_dialog_focus + 1, len(self.current_columns) - 1)
                elif key == curses.KEY_UP:
                    self.find_dialog_focus = max(0, self.find_dialog_focus - 1)
                elif key == curses.KEY_BACKSPACE or key == 127:
                    idx = self.find_dialog_focus
                    if self.find_dialog_inputs[idx]:
                        self.find_dialog_inputs[idx] = self.find_dialog_inputs[idx][:-1]
                elif 32 <= key <= 126:
                    self.find_dialog_inputs[self.find_dialog_focus] += chr(key)
                elif key == 1:  # Ctrl+A - toggle AND/OR for focused field
                    idx = self.find_dialog_focus
                    self.find_dialog_and_flags[idx] = not self.find_dialog_and_flags[idx]
                elif key == 18:  # Ctrl+R - reset all fields
                    self.find_dialog_inputs = [""] * len(self.current_columns)
                    self.find_dialog_and_flags = [False] * len(self.current_columns)
                if key == 3:  # ^C
                    self.quit_flag = True
                continue

            if self.command_mode:
                if key == ord('\n') or key == curses.KEY_ENTER:
                    cmd = self.command_input.strip().lower()
                    if cmd == "unsafe":
                        self.safe_mode = False
                        self.message = "Safety mode OFF - destructive actions enabled"
                    elif cmd == "safe":
                        self.safe_mode = True
                        self.message = "Safety mode ON - destructive actions require confirmation"
                    elif cmd:
                        self.message = f"Unknown command: {cmd}"
                    self.command_mode = False
                    self.command_input = ""
                elif key == 27 or key == 7:  # Esc or Ctrl+G
                    self.command_mode = False
                    self.command_input = ""
                elif key == curses.KEY_BACKSPACE or key == 127:
                    if self.command_input:
                        self.command_input = self.command_input[:-1]
                    else:
                        self.command_mode = False
                elif key == curses.KEY_F1:
                    self.help_mode = True
                elif 32 <= key <= 126:
                    self.command_input += chr(key)
                if key == 3:  # ^C
                    self.quit_flag = True
                continue

            if self.find_mode:
                if key == ord('\n') or key == curses.KEY_ENTER:
                    self.find_mode = False
                    self.apply_find_filter()
                elif key == 27 or key == 7:  # Esc or Ctrl+G
                    self.find_mode = False
                    self.find_input = ""
                elif key == curses.KEY_UP or key == 16:  # Up or ^P
                    if self.find_history:
                        if self.find_history_idx == -1:
                            self.find_saved_input = self.find_input
                            self.find_history_idx = len(self.find_history) - 1
                        elif self.find_history_idx > 0:
                            self.find_history_idx -= 1
                        self.find_input = self.find_history[self.find_history_idx]
                elif key == curses.KEY_DOWN or key == 14:  # Down or ^N
                    if self.find_history_idx >= 0:
                        if self.find_history_idx < len(self.find_history) - 1:
                            self.find_history_idx += 1
                            self.find_input = self.find_history[self.find_history_idx]
                        else:
                            self.find_history_idx = -1
                            self.find_input = self.find_saved_input
                elif key == curses.KEY_BACKSPACE or key == 127:
                    if self.find_input:
                        self.find_input = self.find_input[:-1]
                    else:
                        self.find_mode = False
                elif key == curses.KEY_F1:
                    self.help_mode = True
                elif 32 <= key <= 126:
                    self.find_input += chr(key)
                    self.find_history_idx = -1  # typing resets history browsing
                if key == 3:  # ^C
                    self.quit_flag = True
                continue

            if self.prompt_mode:
                if key in (ord('y'), ord('\n'), curses.KEY_ENTER):
                    if self.prompt_action:
                        self.prompt_action()
                    self.prompt_mode = False
                    self.prompt_action = None
                    self.prompt_text = ""
                elif key in (ord('n'), 7, 27):  # n, ^G, Esc
                    self.prompt_mode = False
                    self.prompt_action = None
                    self.prompt_text = ""
                    self.message = "Cancelled."
                if key == 3:  # ^C
                    self.quit_flag = True
                continue

            if key == 3:  # ^C
                self.quit_flag = True
                continue

            # Global keys: q and Esc both quit the program
            if key == ord('q') or key == 27:  # q or Esc
                break
            elif key == curses.KEY_F1:
                self.help_mode = True
                self.help_scroll_offset = 0
                continue
            elif key == ord(':'):
                self.command_mode = True
                self.command_input = ""
                continue

            # Keys safe to repeat (navigation only — not destructive actions)
            nav_keys = {
                curses.KEY_UP, curses.KEY_DOWN, curses.KEY_PPAGE, curses.KEY_NPAGE,
                curses.KEY_SLEFT, curses.KEY_SRIGHT,
                ord('j'), ord('k'), ord('J'), ord('K'), 9,  # 9 = Tab
            }
            count = repeat_count if key in nav_keys else 1

            # Save state before dispatch so we can detect what changed
            _prev_view = self.current_view
            _prev_row_idx = self.selected_row_idx
            _prev_scroll = self.row_scroll_offset
            self._prev_horiz = self.row_horizontal_offset
            _prev_table_idx = self.selected_table_idx
            _prev_table_scroll = self.table_scroll_offset
            _prev_field_idx = self.selected_field_idx
            _prev_field_scroll = self.field_scroll_offset

            # View-specific input
            for _ in range(count):
                if self.current_view == 'tables':
                    self.handle_tables_input(key, height)
                elif self.current_view == 'rows':
                    self.handle_rows_input(key, height)
                elif self.current_view == 'edit':
                    self.handle_edit_input(stdscr, key, height, width)

            # Decide if next frame needs a full redraw or just partial
            _full_redraw = (
                self.current_view != _prev_view
                or self.message is not None
            )

            if self.quit_flag:
                break

    # ----------------------------------------------------------------
    # Input handlers
    # ----------------------------------------------------------------

    def handle_tables_input(self, key: int, height: int):
        """Handle input in tables view"""
        visible_height = height - 3
        filtered_tables = self.get_filtered_tables()
        table_count = len(filtered_tables)

        if key == curses.KEY_UP or key == ord('k'):
            if self.selected_table_idx > 0:
                self.selected_table_idx -= 1
                if self.selected_table_idx < self.table_scroll_offset:
                    self.table_scroll_offset = self.selected_table_idx
        elif key == curses.KEY_DOWN or key == ord('j'):
            if self.selected_table_idx < table_count - 1:
                self.selected_table_idx += 1
                if self.selected_table_idx >= self.table_scroll_offset + visible_height:
                    self.table_scroll_offset = self.selected_table_idx - visible_height + 1
        elif key == curses.KEY_PPAGE or key == ord('K'):
            self.selected_table_idx = max(0, self.selected_table_idx - visible_height)
            self.table_scroll_offset = max(0, self.table_scroll_offset - visible_height)
        elif key == curses.KEY_NPAGE or key == ord('J'):
            self.selected_table_idx = min(table_count - 1, self.selected_table_idx + visible_height)
            self.table_scroll_offset = max(0, min(table_count - visible_height, self.selected_table_idx - visible_height + 1))
        elif key == ord('\n') or key == curses.KEY_ENTER or key == ord('l') or key == curses.KEY_RIGHT:
            if filtered_tables:
                table_name = filtered_tables[self.selected_table_idx][0]
                self.find_filter = ""  # clear table filter when entering a table
                self.load_table_data(table_name)
                self.current_view = 'rows'
                self.selected_row_idx = 0
                self.row_scroll_offset = 0
                self.row_horizontal_offset = 0
        elif key == 9:  # Tab - next table
            if self.selected_table_idx < table_count - 1:
                self.selected_table_idx += 1
                if self.selected_table_idx >= self.table_scroll_offset + visible_height:
                    self.table_scroll_offset = self.selected_table_idx - visible_height + 1
        elif key == ord('d') or key == curses.KEY_DC:
            if filtered_tables:
                table_name = filtered_tables[self.selected_table_idx][0]
                if not self.safe_mode:
                    if self.drop_current_table():
                        self.message = f"Dropped table: {table_name}"
                else:
                    self.prompt_mode = True
                    self.prompt_text = f" Drop table '{table_name}'? (y/n) "
                    self.prompt_action = self.drop_current_table
        elif key == ord('o'):
            self.open_sort_dialog()
        elif key == ord('/'):
            self.enter_find_mode()

    def handle_rows_input(self, key: int, height: int):
        """Handle input in rows view"""
        visible_height = height - 3

        if key == curses.KEY_UP or key == ord('k'):
            if self.selected_row_idx > 0:
                self.selected_row_idx -= 1
                if self.selected_row_idx < self.row_scroll_offset:
                    self.row_scroll_offset = self.selected_row_idx
        elif key == curses.KEY_DOWN or key == ord('j'):
            if self.selected_row_idx < len(self.current_table_data) - 1:
                self.selected_row_idx += 1
                if self.selected_row_idx >= self.row_scroll_offset + visible_height:
                    self.row_scroll_offset = self.selected_row_idx - visible_height + 1
        elif key == curses.KEY_PPAGE or key == ord('K'):
            self.selected_row_idx = max(0, self.selected_row_idx - visible_height)
            self.row_scroll_offset = max(0, self.row_scroll_offset - visible_height)
        elif key == curses.KEY_NPAGE or key == ord('J'):
            self.selected_row_idx = min(len(self.current_table_data) - 1, self.selected_row_idx + visible_height)
            self.row_scroll_offset = max(0, min(len(self.current_table_data) - visible_height, self.selected_row_idx - visible_height + 1))
        elif key == ord('\n') or key == curses.KEY_ENTER or key == ord('l') or key == curses.KEY_RIGHT:
            self.enter_edit_mode()
        elif key == 9:  # Tab - next row
            if self.selected_row_idx < len(self.current_table_data) - 1:
                self.selected_row_idx += 1
                if self.selected_row_idx >= self.row_scroll_offset + visible_height:
                    self.row_scroll_offset = self.selected_row_idx - visible_height + 1
        elif key == ord('d') or key == curses.KEY_DC:
            if self.current_table_data:
                if not self.safe_mode:
                    self.delete_current_row()
                else:
                    self.prompt_mode = True
                    self.prompt_text = " Delete this row? (y/n) "
                    self.prompt_action = self.delete_current_row
        elif key in (curses.KEY_BACKSPACE, 127, ord('h'), curses.KEY_LEFT):
            self.find_filter = ""
            self.find_where = ""
            self.find_params = []
            self.current_view = 'tables'
        elif key == curses.KEY_SLEFT:
            self.row_horizontal_offset = max(0, self.row_horizontal_offset - 5)
        elif key == curses.KEY_SRIGHT:
            self.row_horizontal_offset += 5
        elif key == ord('o'):
            self.open_sort_dialog()
        elif key == ord('/'):
            self.enter_find_mode()
        elif key == ord('f'):
            self.enter_find_dialog()

    def handle_edit_input(self, stdscr, key: int, height: int, width: int):
        """Handle input in edit view"""
        visible_height = height - 3
        num_fields = len(self.current_columns)

        if key == curses.KEY_UP or key == ord('k'):
            if self.selected_field_idx > 0:
                self.selected_field_idx -= 1
                if self.selected_field_idx < self.field_scroll_offset:
                    self.field_scroll_offset = self.selected_field_idx
        elif key == curses.KEY_DOWN or key == ord('j'):
            if self.selected_field_idx < num_fields - 1:
                self.selected_field_idx += 1
                if self.selected_field_idx >= self.field_scroll_offset + visible_height:
                    self.field_scroll_offset = self.selected_field_idx - visible_height + 1
        elif key == curses.KEY_PPAGE or key == ord('K'):
            self.selected_field_idx = max(0, self.selected_field_idx - visible_height)
            self.field_scroll_offset = max(0, self.field_scroll_offset - visible_height)
        elif key == curses.KEY_NPAGE or key == ord('J'):
            self.selected_field_idx = min(num_fields - 1, self.selected_field_idx + visible_height)
            self.field_scroll_offset = max(0, min(num_fields - visible_height, self.selected_field_idx - visible_height + 1))
        elif key == ord('\n') or key == curses.KEY_ENTER or key == ord('v'):
            current_value = self.edit_values[self.selected_field_idx]
            self.view_field_in_pager(current_value)
        elif key == ord('l') or key == curses.KEY_RIGHT:
            # Open pager if field content is truncated on screen
            current_value = self.edit_values[self.selected_field_idx]
            value_str = str(current_value) if current_value is not None else "NULL"
            col_prefix = f" {self.current_columns[self.selected_field_idx]}: "
            max_display = width - self.get_display_width(col_prefix) - 10
            if '\n' in value_str or self.get_display_width(value_str) > max_display:
                self.view_field_in_pager(current_value)
        elif key == ord('e'):
            self.edit_current_field(stdscr, height, width)
        elif key == 9:  # Tab - next field
            if self.selected_field_idx < num_fields - 1:
                self.selected_field_idx += 1
                if self.selected_field_idx >= self.field_scroll_offset + visible_height:
                    self.field_scroll_offset = self.selected_field_idx - visible_height + 1
        elif key == ord('d') or key == curses.KEY_DC:
            if not self.safe_mode:
                self.edit_values[self.selected_field_idx] = None
            else:
                self.prompt_mode = True
                self.prompt_text = " Set field to NULL? (y/n) "
                self.prompt_action = lambda: self.edit_values.__setitem__(self.selected_field_idx, None)
        elif key == ord('E'):
            current_value = self.edit_values[self.selected_field_idx]
            new_value = self.edit_field_in_editor(current_value)
            if new_value is not None:
                self.set_edit_value(self.selected_field_idx, new_value)
        elif key == ord('s'):
            if self.save_edit_changes():
                self.message = "Changes saved."
            elif not self.message:
                self.message = "Save failed!"
        elif key in (curses.KEY_BACKSPACE, 127, ord('h'), curses.KEY_LEFT):
            if self.has_unsaved_changes():
                self.prompt_mode = True
                self.prompt_text = " Discard unsaved changes? (y/n) "
                self.prompt_action = lambda: setattr(self, 'current_view', 'rows')
            else:
                self.current_view = 'rows'
        elif key == ord('o'):
            if self.has_unsaved_changes():
                self.message = "Save or discard changes before reordering columns."
            else:
                self.open_sort_dialog()
        elif key == ord('/'):
            self.enter_find_mode()

    # ----------------------------------------------------------------
    # Field editing
    # ----------------------------------------------------------------

    def edit_current_field(self, stdscr, height: int, width: int):
        """Edit the currently selected field (inline or external)"""
        col_name = self.current_columns[self.selected_field_idx]
        current_value = self.edit_values[self.selected_field_idx]
        value_str = str(current_value) if current_value is not None else ""
        col_prefix = f" {col_name}: "
        edit_x = self.get_display_width(col_prefix)
        max_width = width - edit_x - 10

        # If contains newline or too wide, use external editor
        if '\n' in value_str or self.get_display_width(value_str) > max_width:
            new_value = self.edit_field_in_editor(current_value)
            if new_value is not None:
                self.set_edit_value(self.selected_field_idx, new_value)
        else:
            # Inline editing
            edit_y = 2 + (self.selected_field_idx - self.field_scroll_offset)
            curses.curs_set(1)
            while True:
                new_value, action = self.get_field_input(stdscr, edit_y, edit_x, max_width, value_str)
                if action == 'switch_to_external':
                    new_value = self.edit_field_in_editor(value_str)
                    if new_value is not None:
                        self.set_edit_value(self.selected_field_idx, new_value)
                    break
                if action == 'quit_program':
                    self.quit_flag = True
                    break
                if new_value is not None:
                    self.set_edit_value(self.selected_field_idx, new_value)
                    value_str = new_value  # update for potential 'next' iteration
                if action != 'next':
                    break
                # Move to next field
                if self.selected_field_idx < len(self.current_columns) - 1:
                    self.selected_field_idx += 1
                    visible_height = height - 3
                    if self.selected_field_idx >= self.field_scroll_offset + visible_height:
                        self.field_scroll_offset = self.selected_field_idx - visible_height + 1
                    col_name = self.current_columns[self.selected_field_idx]
                    current_value = self.edit_values[self.selected_field_idx]
                    value_str = str(current_value) if current_value is not None else ""
                    col_prefix = f" {col_name}: "
                    edit_x = self.get_display_width(col_prefix)
                    max_width = width - edit_x - 10
                    edit_y = 2 + (self.selected_field_idx - self.field_scroll_offset)
                    if '\n' in value_str or self.get_display_width(value_str) > max_width:
                        new_value = self.edit_field_in_editor(current_value)
                        if new_value is not None:
                            self.set_edit_value(self.selected_field_idx, new_value)
                        break
                else:
                    break
            curses.curs_set(0)

    def set_edit_value(self, idx: int, new_value: str):
        """Set edit value with type conversion"""
        stripped = new_value.strip()
        if stripped.upper() == "NULL":
            self.edit_values[idx] = None
        elif stripped == "":
            self.edit_values[idx] = ""
        else:
            try:
                self.edit_values[idx] = int(stripped)
            except ValueError:
                try:
                    self.edit_values[idx] = float(stripped)
                except ValueError:
                    self.edit_values[idx] = new_value.rstrip('\n')

    def view_field_in_pager(self, field_value: str):
        """View field content in a read-only pager"""
        fd, temp_path = tempfile.mkstemp(prefix='sqlite_view_', suffix='.txt')
        try:
            content = str(field_value) if field_value is not None else "NULL"
            os.write(fd, content.encode('utf-8'))
            os.close(fd)
            curses.endwin()
            pager = os.environ.get('PAGER', 'less' if subprocess.call(['which', 'less'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) == 0 else 'more')
            subprocess.call([pager, temp_path])
            curses.doupdate()
        finally:
            try:
                os.unlink(temp_path)
            except:
                pass

    def edit_field_in_editor(self, field_value: Union[str, None]) -> Union[str, None]:
        """Edit field content in external editor"""
        fd, temp_path = tempfile.mkstemp(prefix='sqlite_edit_', suffix='.txt')
        try:
            content = str(field_value) if field_value is not None else ""
            os.write(fd, content.encode('utf-8'))
            os.close(fd)
            editor = os.environ.get('EDITOR', 'vi')
            editor_cmd = [editor]
            if 'vi' in editor.lower() or editor.endswith('vim'):
                editor_cmd += ['-c', 'normal G$', '-c', 'startinsert!']
            editor_cmd.append(temp_path)
            curses.endwin()
            result = subprocess.call(editor_cmd)
            curses.doupdate()
            if result == 0:
                with open(temp_path, 'r', encoding='utf-8') as f:
                    return f.read()
            return None
        finally:
            try:
                os.unlink(temp_path)
            except:
                pass

    def get_field_input(self, stdscr, y: int, x: int, max_width: int, initial_value: str) -> Tuple[Union[str, None], str]:
        """Advanced readline-style inline editor

        Supported keystrokes (Unix/bash/readline compatible):
          Arrows / ^B ^F          move character
          ^A                      home
          ^E                      end
          Backspace / ^H          delete left
          Delete / ^D             delete right
          ^W                      kill word backward
          ^K                      kill to end
          ^U                      kill to beginning
          ^T                      transpose characters
          ^L                      redraw line
          ^G                      abort edit (emacs style)
          ^C                      quit program
          Esc                     cancel
          Tab                     save + next field
          Enter                   save + exit
        """
        input_str = initial_value
        cursor_pos = len(input_str)
        while True:
            stdscr.move(y, x)
            stdscr.clrtoeol()
            self.safe_addstr(stdscr, y, 0, f" {self.current_columns[self.selected_field_idx]}: ", curses.color_pair(self.C_NORMAL))
            self.safe_addstr(stdscr, y, x, input_str, curses.color_pair(self.C_SELECTED))
            cur_x = x + self.get_display_width(input_str[:cursor_pos])
            stdscr.move(y, cur_x)
            stdscr.refresh()

            try:
                key = stdscr.get_wch()
            except KeyboardInterrupt:
                key = chr(3)

            if isinstance(key, str):
                if key == '\n':
                    return input_str, 'save'
                elif ord(key) == 27:  # Esc
                    return None, 'cancel'
                elif ord(key) == 9:  # Tab
                    return input_str, 'next'
                elif ord(key) == 1:  # ^A
                    cursor_pos = 0
                elif ord(key) == 5:  # ^E
                    cursor_pos = len(input_str)
                elif ord(key) == 2:  # ^B
                    cursor_pos = max(0, cursor_pos - 1)
                elif ord(key) == 6:  # ^F
                    cursor_pos = min(len(input_str), cursor_pos + 1)
                elif ord(key) == 4:  # ^D - delete right
                    if cursor_pos < len(input_str):
                        input_str = input_str[:cursor_pos] + input_str[cursor_pos + 1:]
                elif ord(key) == 7:  # ^G - abort edit (emacs style)
                    return None, 'cancel'
                elif ord(key) == 23:  # ^W
                    if cursor_pos > 0:
                        i = cursor_pos
                        while i > 0 and input_str[i - 1].isspace():
                            i -= 1
                        while i > 0 and not input_str[i - 1].isspace():
                            i -= 1
                        input_str = input_str[:i] + input_str[cursor_pos:]
                        cursor_pos = i
                elif ord(key) == 11:  # ^K
                    input_str = input_str[:cursor_pos]
                elif ord(key) == 21:  # ^U
                    input_str = input_str[cursor_pos:]
                    cursor_pos = 0
                elif ord(key) == 20:  # ^T
                    if cursor_pos > 0 and len(input_str) >= 2:
                        if cursor_pos == 0:
                            pass
                        else:
                            p = min(cursor_pos, len(input_str) - 1)
                            if p >= 1:
                                a, b = input_str[p - 1], input_str[p] if p < len(input_str) else input_str[p - 1]
                                input_str = input_str[:p - 1] + b + a + input_str[p + 1:]
                                cursor_pos = min(len(input_str), p + 1)
                elif ord(key) == 12:  # ^L
                    stdscr.redrawwin()
                    stdscr.refresh()
                    continue
                elif ord(key) == 3:  # ^C
                    return None, 'quit_program'
                elif key.isprintable():
                    cw = self.get_display_width(key)
                    if self.get_display_width(input_str) + cw <= max_width - 2:
                        input_str = input_str[:cursor_pos] + key + input_str[cursor_pos:]
                        cursor_pos += 1
                    else:
                        return input_str, 'switch_to_external'

            elif isinstance(key, int):
                if key == curses.KEY_LEFT:
                    cursor_pos = max(0, cursor_pos - 1)
                elif key == curses.KEY_RIGHT:
                    cursor_pos = min(len(input_str), cursor_pos + 1)
                elif key == curses.KEY_BACKSPACE or key == 127:
                    if cursor_pos > 0:
                        input_str = input_str[:cursor_pos - 1] + input_str[cursor_pos:]
                        cursor_pos -= 1
                elif key == curses.KEY_DC:
                    if cursor_pos < len(input_str):
                        input_str = input_str[:cursor_pos] + input_str[cursor_pos + 1:]
                elif key == curses.KEY_F1:
                    # Can't show help overlay from inline edit easily,
                    # so save and exit, then user can press F1
                    return input_str, 'save'

    # ----------------------------------------------------------------
    # Sort / Reorder overlay
    # ----------------------------------------------------------------

    def open_sort_dialog(self):
        """Initialize sort overlay state for the current view"""
        if self.current_view == 'tables':
            self.sort_context = 'tables'
            self.sort_items = [t[0] for t in self.tables]
        elif self.current_view == 'rows':
            self.sort_context = 'rows'
            self.sort_items = list(self.current_columns)
            # Load existing sort config
            row_sort = self.load_config(f'row_sort:{self.current_table_name}')
            if row_sort:
                self.sort_active_cols = [col for col, _ in row_sort if col in self.sort_items]
                self.sort_directions = {col: d for col, d in row_sort if col in self.sort_items}
            else:
                self.sort_active_cols = []
                self.sort_directions = {}
        elif self.current_view == 'edit':
            self.sort_context = 'columns'
            self.sort_items = list(self.current_columns)
        self.sort_selected_idx = 0
        self.sort_grabbed_idx = -1
        self.sort_scroll_offset = 0
        self.sort_mode = True

    def handle_sort_input(self, key):
        """Process keys in the sort overlay. Changes are saved immediately."""
        n = len(self.sort_items)
        if n == 0:
            if key in (27, ord('\n'), curses.KEY_ENTER):
                self.sort_mode = False
            return

        # q quits program from sort overlay
        if key == ord('q'):
            self.quit_flag = True
            self.sort_mode = False
            return

        # F1 opens help
        if key == curses.KEY_F1:
            self.help_mode = True
            self.help_scroll_offset = 0
            return

        # ^C quits program
        if key == 3:
            self.quit_flag = True
            self.sort_mode = False
            return

        # Esc: close
        if key == 27:
            self.sort_mode = False
            return

        # Navigation: j/k or arrows
        if key == curses.KEY_DOWN or key == ord('j'):
            if self.sort_grabbed_idx >= 0:
                # Move grabbed item down
                if self.sort_context == 'rows':
                    item = self.sort_items[self.sort_selected_idx]
                    if item in self.sort_active_cols:
                        idx = self.sort_active_cols.index(item)
                        if idx < len(self.sort_active_cols) - 1:
                            self.sort_active_cols[idx], self.sort_active_cols[idx + 1] = \
                                self.sort_active_cols[idx + 1], self.sort_active_cols[idx]
                            self.apply_sort_result()
                else:
                    if self.sort_grabbed_idx < n - 1:
                        gi = self.sort_grabbed_idx
                        self.sort_items[gi], self.sort_items[gi + 1] = self.sort_items[gi + 1], self.sort_items[gi]
                        self.sort_grabbed_idx = gi + 1
                        self.sort_selected_idx = gi + 1
                        self.apply_sort_result()
            else:
                if self.sort_selected_idx < n - 1:
                    self.sort_selected_idx += 1
        elif key == curses.KEY_UP or key == ord('k'):
            if self.sort_grabbed_idx >= 0:
                if self.sort_context == 'rows':
                    item = self.sort_items[self.sort_selected_idx]
                    if item in self.sort_active_cols:
                        idx = self.sort_active_cols.index(item)
                        if idx > 0:
                            self.sort_active_cols[idx], self.sort_active_cols[idx - 1] = \
                                self.sort_active_cols[idx - 1], self.sort_active_cols[idx]
                            self.apply_sort_result()
                else:
                    if self.sort_grabbed_idx > 0:
                        gi = self.sort_grabbed_idx
                        self.sort_items[gi], self.sort_items[gi - 1] = self.sort_items[gi - 1], self.sort_items[gi]
                        self.sort_grabbed_idx = gi - 1
                        self.sort_selected_idx = gi - 1
                        self.apply_sort_result()
            else:
                if self.sort_selected_idx > 0:
                    self.sort_selected_idx -= 1

        # Space / Enter: grab/drop (tables/columns) or toggle sort field (rows)
        elif key in (ord(' '), ord('\n'), curses.KEY_ENTER):
            if self.sort_context == 'rows':
                item = self.sort_items[self.sort_selected_idx]
                if item in self.sort_active_cols:
                    self.sort_active_cols.remove(item)
                    self.sort_directions.pop(item, None)
                else:
                    self.sort_active_cols.append(item)
                    self.sort_directions[item] = 'ASC'
            else:
                if self.sort_grabbed_idx >= 0:
                    self.sort_grabbed_idx = -1  # drop
                else:
                    self.sort_grabbed_idx = self.sort_selected_idx  # grab
            self.apply_sort_result()

        # a/d set ASC/DESC in rows mode
        elif key == ord('a') and self.sort_context == 'rows':
            item = self.sort_items[self.sort_selected_idx]
            if item in self.sort_active_cols:
                self.sort_directions[item] = 'ASC'
                self.apply_sort_result()
        elif key == ord('d') and self.sort_context == 'rows':
            item = self.sort_items[self.sort_selected_idx]
            if item in self.sort_active_cols:
                self.sort_directions[item] = 'DESC'
                self.apply_sort_result()

        # r: reset to defaults
        elif key == ord('r'):
            self.reset_sort_order()

        # Keep scroll in bounds
        if self.sort_selected_idx < self.sort_scroll_offset:
            self.sort_scroll_offset = self.sort_selected_idx
        elif self.sort_selected_idx >= self.sort_scroll_offset + 20:
            self.sort_scroll_offset = self.sort_selected_idx - 19

    def apply_sort_result(self):
        """Save config and refresh view data immediately"""
        if self.sort_context == 'tables':
            order = list(self.sort_items)
            self.save_config('table_order', order)
            self.table_display_order = order
            self.tables = self.get_tables()
            self.selected_table_idx = min(self.selected_table_idx, max(0, len(self.tables) - 1))
        elif self.sort_context == 'columns':
            order = list(self.sort_items)
            self.save_config(f'column_order:{self.current_table_name}', order)
            self.load_table_data(self.current_table_name)
            if self.current_table_data and self.selected_row_idx < len(self.current_table_data):
                row = self.current_table_data[self.selected_row_idx]
                self.edit_values = list(row[1:])
                self.edit_original_values = list(row[1:])
                self.selected_field_idx = min(self.selected_field_idx, max(0, len(self.current_columns) - 1))
        elif self.sort_context == 'rows':
            if self.sort_active_cols:
                sort_spec = [[col, self.sort_directions.get(col, 'ASC')] for col in self.sort_active_cols]
                self.save_config(f'row_sort:{self.current_table_name}', sort_spec)
            else:
                self.save_config(f'row_sort:{self.current_table_name}', [])
            self.load_table_data(self.current_table_name)
            self.selected_row_idx = 0
            self.row_scroll_offset = 0

    def reset_sort_order(self):
        """Reset to default order, clearing any saved config"""
        if self.sort_context == 'tables':
            self.save_config('table_order', None)
            self.table_display_order = None
            self.tables = self.get_tables()
            self.sort_items = [t[0] for t in self.tables]
            self.selected_table_idx = min(self.selected_table_idx, max(0, len(self.tables) - 1))
        elif self.sort_context == 'columns':
            self.save_config(f'column_order:{self.current_table_name}', None)
            self.load_table_data(self.current_table_name)
            self.sort_items = list(self.current_columns)
            if self.current_table_data and self.selected_row_idx < len(self.current_table_data):
                row = self.current_table_data[self.selected_row_idx]
                self.edit_values = list(row[1:])
                self.edit_original_values = list(row[1:])
                self.selected_field_idx = min(self.selected_field_idx, max(0, len(self.current_columns) - 1))
        elif self.sort_context == 'rows':
            self.save_config(f'row_sort:{self.current_table_name}', None)
            self.save_config(f'column_order:{self.current_table_name}', None)
            self.sort_active_cols = []
            self.sort_directions = {}
            self.load_table_data(self.current_table_name)
            self.sort_items = list(self.current_columns)
            self.selected_row_idx = 0
            self.row_scroll_offset = 0
        self.sort_selected_idx = 0
        self.sort_scroll_offset = 0
        self.sort_grabbed_idx = -1
        self.message = "Reset to default order."

    def draw_sort_overlay(self, stdscr, height, width):
        """Render the sort overlay box, styled like the help overlay"""
        items = self.sort_items
        n = len(items)

        # Calculate window dimensions
        box_w = min(52, width - 4)
        box_h = min(n + 4, height - 2)
        start_y = max(0, (height - box_h) // 2)
        start_x = max(0, (width - box_w) // 2)
        inner_w = box_w - 4

        help_attr = curses.color_pair(self.C_HELP)
        sel_attr = curses.color_pair(self.C_SELECTED)
        border_attr = curses.color_pair(self.C_TITLE) | curses.A_BOLD

        # Box drawing characters
        try:
            "\u2500".encode(locale.getpreferredencoding())
            TL, TR, BL, BR, HZ, VT = "\u250c", "\u2510", "\u2514", "\u2518", "\u2500", "\u2502"
        except (UnicodeEncodeError, LookupError):
            TL, TR, BL, BR, HZ, VT = "+", "+", "+", "+", "-", "|"

        # Title
        if self.sort_context == 'tables':
            title = " Table Display Order "
        elif self.sort_context == 'columns':
            title = " Column Display Order "
        else:
            title = " Sort by Column(s) "
        pad_total = box_w - 2 - len(title)
        pad_left = pad_total // 2
        pad_right = pad_total - pad_left
        top_border = TL + HZ * pad_left + title + HZ * pad_right + TR
        self.safe_addstr(stdscr, start_y, start_x, top_border, border_attr)

        def draw_row(y, content, attr=help_attr):
            self.safe_addstr(stdscr, y, start_x, VT, border_attr)
            self.safe_addstr(stdscr, y, start_x + 1, " " + content + " ", attr)
            self.safe_addstr(stdscr, y, start_x + box_w - 1, VT, border_attr)

        # Blank line after title
        blank = " " * inner_w
        draw_row(start_y + 1, blank)

        # Content area
        content_h = box_h - 4
        # Adjust scroll offset for content height
        if self.sort_selected_idx < self.sort_scroll_offset:
            self.sort_scroll_offset = self.sort_selected_idx
        elif self.sort_selected_idx >= self.sort_scroll_offset + content_h:
            self.sort_scroll_offset = self.sort_selected_idx - content_h + 1

        for i in range(content_h):
            cy = start_y + 2 + i
            idx = self.sort_scroll_offset + i
            if idx < n:
                item = items[idx]
                # Build display text
                if self.sort_context == 'rows':
                    if item in self.sort_active_cols:
                        priority = self.sort_active_cols.index(item) + 1
                        direction = self.sort_directions.get(item, 'ASC')
                        label = f"{priority}. {item} [{direction}]"
                    else:
                        label = f"   {item}"
                else:
                    grabbed = (idx == self.sort_grabbed_idx)
                    prefix = "\u2261 " if grabbed else "  "
                    label = prefix + item

                trunc = self.truncate_to_width(label, inner_w)
                trunc_w = self.get_display_width(trunc)
                display = trunc + ' ' * (inner_w - trunc_w)
                if idx == self.sort_selected_idx:
                    draw_row(cy, display, sel_attr)
                else:
                    draw_row(cy, display)
            else:
                draw_row(cy, blank)

        # Blank line before bottom border
        draw_row(start_y + box_h - 2, blank)

        # Bottom hint
        if self.sort_context == 'rows':
            hint = " Spc:Toggle a/d:Dir j/k:Move r:Reset Esc:OK "
        else:
            hint = " Spc:Grab/Drop j/k:Move r:Reset Esc:OK "
        pad_total = box_w - 2 - len(hint)
        pad_left = pad_total // 2
        pad_right = pad_total - pad_left
        bot_border = BL + HZ * pad_left + hint + HZ * pad_right + BR
        self.safe_addstr(stdscr, start_y + box_h - 1, start_x, bot_border, border_attr)

    # ----------------------------------------------------------------
    # Find / Filter
    # ----------------------------------------------------------------

    def enter_find_mode(self):
        """Activate the find bar, pre-filled with current filter"""
        self.find_mode = True
        self.find_input = self.find_filter
        self.find_history_idx = -1
        self.find_saved_input = ""

    def apply_find_filter(self):
        """Parse find_input and apply as active filter"""
        text = self.find_input.strip()
        if text == "" or text == ".":
            self.clear_find_filter()
            return
        # Add to history (avoid consecutive duplicates)
        if not self.find_history or self.find_history[-1] != text:
            self.find_history.append(text)
        self.find_filter = text
        if self.current_view == 'rows':
            where, params = self.parse_find_pattern(text)
            self.find_where = where
            self.find_params = params
            self.load_table_data(self.current_table_name)
            self.selected_row_idx = 0
            self.row_scroll_offset = 0
        elif self.current_view == 'tables':
            self.selected_table_idx = 0
            self.table_scroll_offset = 0

    def clear_find_filter(self):
        """Reset all filter state and reload data"""
        self.find_filter = ""
        self.find_where = ""
        self.find_params = []
        if self.current_view == 'rows':
            self.load_table_data(self.current_table_name)
            self.selected_row_idx = 0
            self.row_scroll_offset = 0
        elif self.current_view == 'tables':
            self.selected_table_idx = 0
            self.table_scroll_offset = 0

    def parse_find_pattern(self, pattern: str) -> Tuple[str, list]:
        """Parse a find pattern into a SQL WHERE clause and params.

        Plain text   -> OR across all columns (LIKE '%val%')
        col=val      -> exact match on column
        col~val      -> LIKE match on column
        Multiple terms separated by spaces are ANDed.
        """
        terms = pattern.split()
        clauses = []
        params = []
        for term in terms:
            if '~' in term:
                col, val = term.split('~', 1)
                if col in self.current_columns:
                    clauses.append(f'"{col}" LIKE ?')
                    params.append(f'%{val}%')
                else:
                    # treat as plain text if column not found
                    or_parts = []
                    for c in self.current_columns:
                        or_parts.append(f'"{c}" LIKE ?')
                        params.append(f'%{term}%')
                    clauses.append(f'({" OR ".join(or_parts)})')
            elif '=' in term:
                col, val = term.split('=', 1)
                if col in self.current_columns:
                    clauses.append(f'"{col}" = ?')
                    params.append(val)
                else:
                    or_parts = []
                    for c in self.current_columns:
                        or_parts.append(f'"{c}" LIKE ?')
                        params.append(f'%{term}%')
                    clauses.append(f'({" OR ".join(or_parts)})')
            else:
                or_parts = []
                for c in self.current_columns:
                    or_parts.append(f'"{c}" LIKE ?')
                    params.append(f'%{term}%')
                clauses.append(f'({" OR ".join(or_parts)})')
        where = " AND ".join(clauses)
        return where, params

    def draw_find_bar(self, stdscr, h, w):
        """Draw Find: input at bottom, with column hint on the line above"""
        # Show column names as hint on the line above the find bar
        if self.current_view == 'rows' and self.current_columns:
            hint = "Columns: " + ", ".join(self.current_columns) + "  (col=val col~val)"
            hint = self.truncate_to_width(hint, w - 1)
            self.safe_addstr_full_width(stdscr, h - 2, 0, " " + hint, curses.color_pair(self.C_STATUS))
        prompt = f"Find: {self.find_input}"
        self.safe_addstr_full_width(stdscr, h - 1, 0, prompt, curses.color_pair(self.C_STATUS))
        cursor_x = min(self.get_display_width(prompt), w - 1)
        stdscr.move(h - 1, cursor_x)

    # ----------------------------------------------------------------
    # Per-column Find dialog
    # ----------------------------------------------------------------

    def enter_find_dialog(self):
        """Open the per-column find dialog (rows view only)"""
        if self.current_view != 'rows' or not self.current_columns:
            return
        self.find_dialog = True
        self.find_dialog_focus = 0
        self.find_dialog_scroll = 0
        self.find_dialog_inputs = [""] * len(self.current_columns)
        self.find_dialog_and_flags = [False] * len(self.current_columns)
        # Pre-fill from active filter if there is one
        if self.find_where and self.find_params:
            self._prefill_dialog_from_filter()

    def _prefill_dialog_from_filter(self):
        """Try to reverse-map the active WHERE clause back into per-column values.

        Since we generate the WHERE ourselves, we know the structure:
        - AND fields appear as top-level '"col" LIKE ?' clauses
        - OR fields appear inside a parenthesized group '("col" LIKE ? OR ...)'
        """
        if not self.find_where:
            return
        # Find the parenthesized OR group, if any
        or_group_start = self.find_where.find('(')
        or_group_end = self.find_where.rfind(')')
        for i, col in enumerate(self.current_columns):
            marker = f'"{col}" LIKE ?'
            if marker in self.find_where:
                # Find which parameter corresponds to this column
                pos = self.find_where.index(marker)
                before = self.find_where[:pos]
                param_idx = before.count('LIKE ?')
                if param_idx < len(self.find_params):
                    val = self.find_params[param_idx]
                    if val.startswith('%') and val.endswith('%'):
                        val = val[1:-1]
                    self.find_dialog_inputs[i] = val
                    # Determine AND vs OR: if marker is inside parens, it's OR
                    if or_group_start >= 0 and pos > or_group_start and pos < or_group_end:
                        self.find_dialog_and_flags[i] = False
                    else:
                        self.find_dialog_and_flags[i] = True

    def apply_find_dialog(self):
        """Build WHERE clause from filled dialog fields and apply.

        OR fields (default) are grouped: (col1 LIKE ? OR col2 LIKE ? ...).
        AND fields are required: each becomes a top-level AND clause.
        """
        and_clauses = []
        or_clauses = []
        params_and = []
        params_or = []
        terms = []
        for i, col in enumerate(self.current_columns):
            val = self.find_dialog_inputs[i].strip()
            if val:
                if self.find_dialog_and_flags[i]:
                    and_clauses.append(f'"{col}" LIKE ?')
                    params_and.append(f'%{val}%')
                    terms.append(f'+{col}~{val}')
                else:
                    or_clauses.append(f'"{col}" LIKE ?')
                    params_or.append(f'%{val}%')
                    terms.append(f'{col}~{val}')

        all_clauses = list(and_clauses)
        if or_clauses:
            all_clauses.append("(" + " OR ".join(or_clauses) + ")")

        if all_clauses:
            self.find_where = " AND ".join(all_clauses)
            self.find_params = params_and + params_or
            self.find_filter = " ".join(terms)
        else:
            self.find_where = ""
            self.find_params = []
            self.find_filter = ""
        self.load_table_data(self.current_table_name)
        self.selected_row_idx = 0
        self.row_scroll_offset = 0

    def draw_find_dialog(self, stdscr, height, width):
        """Draw the per-column find dialog as a centered overlay"""
        cols = self.current_columns
        n = len(cols)

        # Calculate window dimensions
        box_w = min(60, width - 4)
        box_h = min(n + 4, height - 2)  # title + blank + content + blank + bottom
        start_y = max(0, (height - box_h) // 2)
        start_x = max(0, (width - box_w) // 2)
        inner_w = box_w - 4  # border + 1 padding each side

        help_attr = curses.color_pair(self.C_HELP)
        sel_attr = curses.color_pair(self.C_SELECTED)
        border_attr = curses.color_pair(self.C_TITLE) | curses.A_BOLD

        # Box drawing characters
        try:
            "\u2500".encode(locale.getpreferredencoding())
            TL, TR, BL, BR, HZ, VT = "\u250c", "\u2510", "\u2514", "\u2518", "\u2500", "\u2502"
        except (UnicodeEncodeError, LookupError):
            TL, TR, BL, BR, HZ, VT = "+", "+", "+", "+", "-", "|"

        # Top border with centered title
        title = " Find by Column "
        pad_total = box_w - 2 - len(title)
        pad_left = pad_total // 2
        pad_right = pad_total - pad_left
        top_border = TL + HZ * pad_left + title + HZ * pad_right + TR
        self.safe_addstr(stdscr, start_y, start_x, top_border, border_attr)

        def draw_row(y, content, attr=help_attr):
            self.safe_addstr(stdscr, y, start_x, VT, border_attr)
            self.safe_addstr(stdscr, y, start_x + 1, " " + content + " ", attr)
            self.safe_addstr(stdscr, y, start_x + box_w - 1, VT, border_attr)

        # Blank line after title
        blank = " " * inner_w
        draw_row(start_y + 1, blank)

        # Content area
        content_h = box_h - 4

        # Adjust scroll to keep focus visible
        if self.find_dialog_focus < self.find_dialog_scroll:
            self.find_dialog_scroll = self.find_dialog_focus
        elif self.find_dialog_focus >= self.find_dialog_scroll + content_h:
            self.find_dialog_scroll = self.find_dialog_focus - content_h + 1

        # Label width = max column name length + 2 (for ": ")
        max_label = 0
        for col in cols:
            cw = self.get_display_width(col)
            if cw > max_label:
                max_label = cw
        label_w = min(max_label + 2, inner_w // 2)  # cap at half the inner width
        indicator_w = 4  # "OR  " or "AND "
        input_w = inner_w - label_w - indicator_w

        cursor_y = -1
        cursor_x = -1

        for i in range(content_h):
            cy = start_y + 2 + i
            idx = self.find_dialog_scroll + i
            if idx < n:
                col = cols[idx]
                label = self.truncate_to_width(col, label_w - 2) + ": "
                label_dw = self.get_display_width(label)
                label = label + " " * (label_w - label_dw)

                val = self.find_dialog_inputs[idx]
                val_display = self.truncate_to_width(val, input_w)
                val_dw = self.get_display_width(val_display)
                val_padded = val_display + " " * (input_w - val_dw)

                is_and = self.find_dialog_and_flags[idx]
                indicator = "AND " if is_and else "OR  "

                if idx == self.find_dialog_focus:
                    # Draw label in help_attr, indicator, input area in sel_attr
                    self.safe_addstr(stdscr, cy, start_x, VT, border_attr)
                    self.safe_addstr(stdscr, cy, start_x + 1, " " + label, help_attr)
                    ind_x = start_x + 2 + label_w
                    ind_attr = sel_attr | curses.A_BOLD if is_and else help_attr
                    self.safe_addstr(stdscr, cy, ind_x, indicator, ind_attr)
                    input_x = ind_x + indicator_w
                    self.safe_addstr(stdscr, cy, input_x, val_padded, sel_attr)
                    self.safe_addstr(stdscr, cy, start_x + box_w - 1, VT, border_attr)
                    # Position cursor at end of text in focused field
                    cursor_y = cy
                    cursor_x = min(input_x + val_dw, start_x + box_w - 2)
                else:
                    ind_attr = help_attr | curses.A_BOLD if is_and else help_attr
                    self.safe_addstr(stdscr, cy, start_x, VT, border_attr)
                    self.safe_addstr(stdscr, cy, start_x + 1, " " + label, help_attr)
                    ind_x = start_x + 2 + label_w
                    self.safe_addstr(stdscr, cy, ind_x, indicator, ind_attr)
                    input_x = ind_x + indicator_w
                    self.safe_addstr(stdscr, cy, input_x, val_padded, help_attr)
                    self.safe_addstr(stdscr, cy, start_x + box_w - 1, VT, border_attr)
            else:
                draw_row(cy, blank)

        # Blank line before bottom border
        draw_row(start_y + box_h - 2, blank)

        # Bottom border with hint
        hint = " Tab:Next ^A:AND/OR ^R:Clear Enter:Apply Esc:Cancel "
        pad_total = box_w - 2 - len(hint)
        pad_left = pad_total // 2
        pad_right = pad_total - pad_left
        bot_border = BL + HZ * pad_left + hint + HZ * pad_right + BR
        self.safe_addstr(stdscr, start_y + box_h - 1, start_x, bot_border, border_attr)

        # Show cursor in focused field
        if cursor_y >= 0 and cursor_x >= 0:
            curses.curs_set(1)
            try:
                stdscr.move(cursor_y, cursor_x)
            except curses.error:
                pass

    def close(self):
        self.conn.close()


def main():
    if len(sys.argv) < 2 or len(sys.argv) > 3:
        print("Usage: browse-sqlite3 <database.db> [table]")
        sys.exit(1)

    db_path = sys.argv[1]
    initial_table = sys.argv[2] if len(sys.argv) == 3 else None

    try:
        browser = SQLiteBrowser(db_path)
        if initial_table:
            # Find the table (case-insensitive match)
            found = False
            for i, (name, _cols) in enumerate(browser.tables):
                if name.lower() == initial_table.lower():
                    browser.selected_table_idx = i
                    browser.load_table_data(name)
                    browser.current_view = 'rows'
                    found = True
                    break
            if not found:
                print(f"Table not found: {initial_table}")
                browser.close()
                sys.exit(1)
        curses.wrapper(browser.run)
        browser.close()
    except sqlite3.Error as e:
        print(f"Database error: {e}")
        sys.exit(1)
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
