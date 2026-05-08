#!/usr/bin/env python3
"""Scan every text cell in a SQLite database, collect unique non-ASCII
codepoints, and report their Unicode EAW property alongside the width
our C code would assign them."""

import sys
import sqlite3
import unicodedata

DB = sys.argv[1] if len(sys.argv) > 1 else \
    "/home/david/.local/share/ytran/youtube-transcripts.db"

# Replicate char_display_width() logic from uniwidth.c.
# g_eaw_ambiguous_width is what the probe returns; we test both 1 and 2.

def is_eaw_ambiguous_range(cp):
    return (
        (0x00A1 <= cp <= 0x00FF) or
        (0x2010 <= cp <= 0x204D) or
        (0x2100 <= cp <= 0x214F) or
        (0x2190 <= cp <= 0x21FF) or
        (0x2200 <= cp <= 0x22FF) or
        (0x2300 <= cp <= 0x23FF) or
        (0x2460 <= cp <= 0x24FF) or
        (0x2500 <= cp <= 0x25FF)
    )

def c_width(cp, eaw_mode=2):
    if 0x20 <= cp <= 0x7E:
        return 1
    if cp < 0x20 or (0x7F <= cp <= 0x9F):
        return 0
    if 0xFE00 <= cp <= 0xFE0F:
        return 0
    if cp in (0x200B, 0x200C, 0x200D, 0xFEFF):
        return 0
    # emoji always wide
    if 0x2600 <= cp <= 0x27BF:  return 2
    if 0x1F1E0 <= cp <= 0x1F1FF: return 2
    if 0x1F300 <= cp <= 0x1F9FF: return 2
    if 0x1FA00 <= cp <= 0x1FAFF: return 2
    # EAW=A ranges we cover
    if is_eaw_ambiguous_range(cp):
        return eaw_mode
    # fallback: use Python's wcwidth equivalent
    import unicodedata as ud
    eaw = ud.east_asian_width(chr(cp))
    if eaw in ('W', 'F'):
        return 2
    cat = ud.category(chr(cp))
    if cat in ('Mn', 'Me', 'Cf'):
        return 0
    return 1

# Scan database
con = sqlite3.connect(DB, detect_types=0)
con.text_factory = lambda b: b.decode("utf-8", errors="replace")
cur = con.cursor()

# Get all tables
cur.execute("SELECT name FROM sqlite_master WHERE type='table'")
tables = [r[0] for r in cur.fetchall()]

codepoints = {}  # cp -> set of (table, column)

for tbl in tables:
    cur.execute(f"PRAGMA table_info(\"{tbl}\")")
    cols = [(r[1], r[2]) for r in cur.fetchall()]
    text_cols = [c for c, t in cols if t.upper() in ('TEXT', 'VARCHAR', '') or t == '']
    if not text_cols:
        # Accept any column without a strict integer type
        text_cols = [c for c, t in cols
                     if 'INT' not in t.upper() and 'REAL' not in t.upper()
                        and 'BLOB' not in t.upper() and 'NUM' not in t.upper()]
    for col in text_cols:
        try:
            cur.execute(f"SELECT \"{col}\" FROM \"{tbl}\" WHERE \"{col}\" IS NOT NULL")
        except Exception:
            continue
        for (val,) in cur:
            if not isinstance(val, str):
                continue
            for ch in val:
                cp = ord(ch)
                if cp > 0x7E:
                    if cp not in codepoints:
                        codepoints[cp] = set()
                    codepoints[cp].add(f"{tbl}.{col}")

con.close()

# Report: show any chars where EAW=A but NOT covered by our ranges,
# or where our range covers a non-A char (potential over-truncation)
print(f"{'CP':>8}  {'CH':^4}  {'EAW':^3}  {'Cat':^3}  "
      f"{'OurW(eaw=1)':^11}  {'OurW(eaw=2)':^11}  {'PyW':^4}  Name")
print("-" * 100)

problems = []
covered_non_A = []
uncovered_A = []

for cp in sorted(codepoints):
    ch = chr(cp)
    try:
        eaw = unicodedata.east_asian_width(ch)
        cat = unicodedata.category(ch)
        name = unicodedata.name(ch, f"U+{cp:04X}")
    except Exception:
        eaw, cat, name = '?', '??', f"U+{cp:04X}"

    ow1 = c_width(cp, eaw_mode=1)
    ow2 = c_width(cp, eaw_mode=2)
    # Python's "correct" width: W/F=2, combining=0, else 1
    pyw = 2 if eaw in ('W', 'F') else (0 if cat in ('Mn', 'Me', 'Cf') else 1)

    in_our_range = is_eaw_ambiguous_range(cp)
    is_A = (eaw == 'A')

    flag = ''
    if is_A and not in_our_range:
        flag = '*** UNCOVERED A'
        uncovered_A.append((cp, ch, name, eaw, ow1, ow2))
    elif not is_A and in_our_range and ow2 != pyw:
        flag = '  ~ over-covered'
        covered_non_A.append((cp, ch, name, eaw, ow1, ow2))

    if flag or eaw in ('A', 'W', 'F'):
        print(f"U+{cp:04X}  {ch:^4}  {eaw:^3}  {cat:^3}  "
              f"{ow1:^11}  {ow2:^11}  {pyw:^4}  {name[:40]}  {flag}")

print()
print(f"Total unique non-ASCII codepoints in DB: {len(codepoints)}")
print(f"EAW=A chars NOT in our covered ranges (*** UNCOVERED A): {len(uncovered_A)}")
print(f"Non-A chars in our ranges (~ over-covered): {len(covered_non_A)}")
