# browse-sqlite3

Interactive terminal browser for SQLite databases with vi-style navigation and CGA colors (TUI).

Also available as `bs3`.

## Why browse-sqlite3?

Most SQLite tools fall into two camps: command-line shells that require
you to type SQL for everything, and GUI applications that require a
desktop environment.  browse-sqlite3 fills the gap: a full-screen
terminal application that lets you browse tables, scroll through rows,
and edit fields interactively -- no SQL required, no GUI needed.  It
runs anywhere you have a terminal and Python.

## Features

- **Three navigable views** -- table list, row browser, field editor --
  connected in a hierarchy (l/Enter to drill in, h/Left to go back)
- **Vi-style navigation** -- hjkl, JK for paging, Tab to advance
- **Readline-style inline editor** -- ^A/^E/^B/^F/^W/^K/^U/^T/^D/^G
- **Turbo Pascal IDE colors** -- exact CGA palette values (blue
  backgrounds, yellow text, cyan title bars) on terminals that support
  `curses.can_change_color()`
- **Multi-column sort** -- press `o` in the row browser to sort by one
  or more columns with ASC/DESC toggles; sort order is persisted
  per-table
- **Structured search** -- press `/` for free-text search across all
  columns, or `f` for a per-column find dialog with AND/OR toggles
- **Custom display order** -- rearrange tables, columns, and sort
  priority with a grab-and-move overlay; ordering is persisted in a
  `_browse_config` table
- **Safety mode** -- destructive actions (drop table, delete row, set
  NULL) require confirmation by default; `:unsafe` disables it
- **External editor/pager** -- press `E` to edit a field in `$EDITOR`,
  `v`/Enter to view in `$PAGER`
- **East Asian wide-character support** -- correct display-width
  accounting for CJK, emoji, combining marks, and zero-width characters
- **Optional C accelerator** -- `_displaywidth.c` provides 3-5x faster
  Unicode width calculations for non-ASCII content
- **Smooth scrolling** -- 30fps frame-paced input with axis-aware
  batching prevents key flooding during held navigation

## Screenshots

```
┌─────────────────────────────────────────────────────────┐
│  browse-sqlite3 — mydb.sqlite3                          │
│─────────────────────────────────────────────────────────│
│  ► users          (id, name, email, created_at)         │
│    posts          (id, user_id, title, body, date)      │
│    comments       (id, post_id, user_id, text)          │
│    tags           (id, name)                             │
│    post_tags      (post_id, tag_id)                      │
│                                                          │
│ SAFE | hjkl:Nav | Tab:→ | l:Open | o:Order | q:Quit    │
└─────────────────────────────────────────────────────────┘
```

## Building

```sh
./configure
make
make check
sudo make install
```

The C extension is built automatically if a C compiler is available.
If it fails or is absent, browse-sqlite3 falls back to pure Python
with no loss of functionality.

## Dependencies

| Dependency | Notes |
|------------|-------|
| Python 3.6+ | with `sqlite3` and `curses` modules (both in the standard library) |
| C compiler | optional, for the `_displaywidth` accelerator module |

No third-party Python packages are required.

## Usage

```sh
browse-sqlite3 mydb.sqlite3           # open at the table list
browse-sqlite3 mydb.sqlite3 users     # jump to the users table
bs3 mydb.sqlite3                      # same thing, shorter name
```

### Quick reference

| Key | Action |
|-----|--------|
| `j`/`k` | Move selection down/up |
| `J`/`K` | Page down/up |
| `l`/Enter | Drill into selected item |
| `h`/Left | Go back one level |
| `Tab` | Advance to next item |
| `o` | Open sort/order overlay |
| `/` | Search/filter |
| `f` | Per-column find dialog (row browser) |
| `e` | Edit field inline |
| `E` | Edit field in $EDITOR |
| `v`/Enter | View field in $PAGER |
| `d`/Del | Delete (respects safety mode) |
| `s` | Save changes (field editor) |
| `:safe` | Enable safety mode |
| `:unsafe` | Disable safety mode |
| `F1` | Context-sensitive help |
| `q`/Esc | Quit |

See `browse-sqlite3(1)` for the full manual.

## Configuration

browse-sqlite3 stores display preferences (table order, column order,
sort specifications) in a `_browse_config` table inside the database
itself.  This means your layout preferences travel with the database
file.

## Distribution packaging

Starter packaging for RPM, Debian, Arch, Alpine, Gentoo, Slackware,
FreeBSD, OpenBSD, NetBSD, and NixOS is provided under `dist/`.
See `README.distributions` for per-distro instructions.

## License

BSD-2-Clause.  See `COPYING` for details.
