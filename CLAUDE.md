# browse-sqlite3

Python curses-based SQLite database browser.

## Build System

Hand-written POSIX configure script + Makefile.in (no autotools).
Standard GNU targets: all, install, uninstall, clean, distclean, check, dist.

## Rendering Rules

- Use `erase()` + `touchwin()` + `refresh()` for redraws, never `clear()`
- After `addstr()`, use `getyx()` for cursor position - never calculate
  padding from `get_display_width()`
- Display width: overcounting is safe (truncation), undercounting is
  fatal (overflow/corruption)

## Unicode Width

- `_char_width()` is the single source of truth for character widths
- Mn/Me/Cf categories = 0 (except variation selectors U+FE00-FE0F = 1)
- East Asian W/F = 2
- Category So at U+2600+ = 2 (emoji symbols)
- Check `isascii()` before dispatching to C extension
