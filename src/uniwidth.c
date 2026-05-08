/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 David Walther */
#include "uniwidth.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <stdio.h>
#include <unistd.h>
#include <termios.h>

int g_eaw_ambiguous_width = 1;

/* Probe terminal EAW rendering by printing U+2014 (EM DASH, EAW=A) and
 * reading back the cursor column via ANSI DSR (ESC[6n).  Must be called
 * before initscr() while we still own the terminal fd directly. */
int terminal_probe_eaw_width(void)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return g_eaw_ambiguous_width;

    struct termios orig, raw;
    if (tcgetattr(STDIN_FILENO, &orig) != 0)
        return g_eaw_ambiguous_width;

    raw = orig;
    raw.c_lflag &= ~(unsigned)(ECHO | ICANON);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 2;   /* 200 ms read timeout */
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    /* DECSC, home, print EM DASH (UTF-8: E2 80 94), DSR */
    fputs("\0337\033[1;1H\xe2\x80\x94\033[6n", stdout);
    fflush(stdout);

    char buf[32];
    int n = 0;
    while (n < (int)sizeof(buf) - 1) {
        char c;
        if (read(STDIN_FILENO, &c, 1) != 1)
            break;
        buf[n++] = c;
        if (c == 'R')
            break;
    }
    buf[n] = '\0';

    fputs("\0338", stdout);  /* DECRC: restore cursor */
    fflush(stdout);

    tcsetattr(STDIN_FILENO, TCSANOW, &orig);

    /* Response is ESC [ row ; col R  (1-indexed columns).
     * col==2 means em-dash took 1 column; col==3 means 2 columns. */
    int row = 0, col = 0;
    if (n >= 4 && buf[0] == '\033' && buf[1] == '[')
        sscanf(buf + 2, "%d;%dR", &row, &col);

    g_eaw_ambiguous_width = (col >= 3) ? 2 : 1;
    return g_eaw_ambiguous_width;
}

/* Return 1 if ch is in a Unicode EAW=Ambiguous range that terminals may
 * render as either 1 or 2 columns depending on locale/configuration.
 * Only covers chars where wcwidth() returns 1 — EAW=W/F are caught earlier. */
static int is_eaw_ambiguous(wchar_t ch)
{
    /* Latin-1 Supplement: °, ±, ×, ÷, ¿, ¡, ½, etc. */
    if (ch >= 0x00A1 && ch <= 0x00FF) return 1;

    /* General Punctuation: hyphens, dashes, curly quotes, bullet, ellipsis */
    if (ch >= 0x2010 && ch <= 0x204D) return 1;

    /* Superscripts and Subscripts (², ³, ₂ etc.) */
    if (ch >= 0x2070 && ch <= 0x209F) return 1;

    /* Currency Symbols (€ etc.) */
    if (ch >= 0x20A0 && ch <= 0x20CF) return 1;

    /* Letterlike Symbols (℃ ™ ℹ Ω etc.) */
    if (ch >= 0x2100 && ch <= 0x214F) return 1;

    /* Number Forms (⅓ ½ etc.) */
    if (ch >= 0x2150 && ch <= 0x218F) return 1;

    /* Arrows */
    if (ch >= 0x2190 && ch <= 0x21FF) return 1;

    /* Mathematical Operators */
    if (ch >= 0x2200 && ch <= 0x22FF) return 1;

    /* Miscellaneous Technical */
    if (ch >= 0x2300 && ch <= 0x23FF) return 1;

    /* Enclosed Alphanumerics */
    if (ch >= 0x2460 && ch <= 0x24FF) return 1;

    /* Box Drawing, Block Elements, Geometric Shapes */
    if (ch >= 0x2500 && ch <= 0x25FF) return 1;

    /* Miscellaneous Symbols and Arrows (⭐ U+2B50, ⭕ U+2B55, etc.) */
    if (ch >= 0x2B00 && ch <= 0x2BFF) return 1;

    return 0;
}

int char_display_width(wchar_t ch)
{
    /* Fast path: printable ASCII */
    if (ch >= 0x20 && ch <= 0x7E)
        return 1;

    /* Control characters */
    if (ch < 0x20 || (ch >= 0x7F && ch <= 0x9F))
        return 0;

    /* Zero-width characters */
    if (ch >= 0xFE00 && ch <= 0xFE0F)  /* Variation selectors (FE0F = emoji VS) */
        return 0;
    if (ch == 0x200B || ch == 0x200C || ch == 0x200D || ch == 0xFEFF)
        return 0;
    /* Combining Diacritical Marks and common combining ranges */
    if ((ch >= 0x0300 && ch <= 0x036F) ||
        (ch >= 0x0483 && ch <= 0x0489) ||
        (ch >= 0x0591 && ch <= 0x05BD) ||
        (ch >= 0x0610 && ch <= 0x061A) ||
        (ch >= 0x064B && ch <= 0x065F) ||
        (ch == 0x0670) ||
        (ch >= 0x06D6 && ch <= 0x06DC) ||
        (ch >= 0x06DF && ch <= 0x06E4) ||
        (ch == 0x0E31) ||
        (ch >= 0x0E34 && ch <= 0x0E3A) ||
        (ch >= 0x20D0 && ch <= 0x20FF) ||
        (ch >= 0xFE20 && ch <= 0xFE2F))
        return 0;

    /* Everything at U+1F000 and above is pictographic/emoji — always 2 wide.
     * This covers Mahjong tiles, playing cards, enclosed alphanumeric
     * supplement (🅴🅵🆁), enclosed ideographic supplement, and all emoji. */
    if (ch >= 0x1F000)
        return 2;

    /* Misc Symbols + Dingbats always wide */
    if (ch >= 0x2600 && ch <= 0x27BF)
        return 2;

    /* Ask wcwidth() first — it correctly identifies EAW=W/F chars (⌚⏰ etc.)
     * as 2-wide regardless of locale ambiguity setting. */
    int w = wcwidth(ch);
    if (w < 0) return 0;
    if (w >= 2) return 2;

    /* w == 1: check if EAW=Ambiguous (terminal-dependent rendering) */
    if (is_eaw_ambiguous(ch))
        return g_eaw_ambiguous_width;

    return 1;
}

/* Peek at the next codepoint in a UTF-8 stream without advancing s.
 * Returns the codepoint or 0 on error. */
static wchar_t peek_next_codepoint(const char *s, size_t remaining)
{
    if (remaining == 0) return 0;
    mbstate_t st;
    memset(&st, 0, sizeof(st));
    wchar_t wc;
    size_t n = mbrtowc(&wc, s, remaining, &st);
    if (n == 0 || n == (size_t)-1 || n == (size_t)-2)
        return 0;
    return wc;
}

/* Effective display width of codepoint wc at position s (after wc was decoded
 * from bytes [s-n .. s)).  If wc has char_display_width==1 and is followed by
 * U+FE0F (emoji variation selector), the pair renders as emoji-width 2, so we
 * return 2 and advance *s / *remaining past the FE0F. */
static int effective_width(wchar_t wc, size_t cw_bytes,
                           const char **s, size_t *remaining)
{
    int cw = char_display_width(wc);
    if (cw == 1) {
        wchar_t next = peek_next_codepoint(*s, *remaining);
        if (next == 0xFE0F) {
            /* Consume the FE0F — it upgrades the preceding char to emoji-width */
            mbstate_t st;
            memset(&st, 0, sizeof(st));
            wchar_t dummy;
            size_t n = mbrtowc(&dummy, *s, *remaining, &st);
            if (n != 0 && n != (size_t)-1 && n != (size_t)-2) {
                *s += n;
                *remaining -= n;
            }
            return 2;
        }
    }
    (void)cw_bytes;
    return cw;
}

int str_display_width(const char *utf8)
{
    if (!utf8) return 0;

    /* Fast path: pure ASCII */
    const unsigned char *p = (const unsigned char *)utf8;
    int all_ascii = 1;
    int len = 0;
    while (p[len]) {
        if (p[len] > 0x7E) { all_ascii = 0; break; }
        len++;
    }
    if (all_ascii) return len;

    int width = 0;
    mbstate_t state;
    memset(&state, 0, sizeof(state));
    const char *s = utf8;
    size_t remaining = strlen(utf8);
    while (remaining > 0) {
        wchar_t wc;
        size_t n = mbrtowc(&wc, s, remaining, &state);
        if (n == 0) break;
        if (n == (size_t)-1 || n == (size_t)-2) {
            width++;
            s++;
            remaining--;
            memset(&state, 0, sizeof(state));
            continue;
        }
        s += n;
        remaining -= n;
        width += effective_width(wc, n, &s, &remaining);
    }
    return width;
}

char *str_truncate_to_width(const char *utf8, int max_w)
{
    if (!utf8) return xstrdup("");
    if (max_w <= 0) return xstrdup("");

    /* Fast path: pure ASCII */
    size_t slen = strlen(utf8);
    const unsigned char *p = (const unsigned char *)utf8;
    int all_ascii = 1;
    for (size_t i = 0; i < slen; i++) {
        if (p[i] > 0x7E) { all_ascii = 0; break; }
    }
    if (all_ascii) {
        if ((int)slen <= max_w) return xstrdup(utf8);
        return xstrndup(utf8, (size_t)max_w);
    }

    int w = 0;
    mbstate_t state;
    memset(&state, 0, sizeof(state));
    const char *s = utf8;
    const char *cut = utf8;
    size_t remaining = slen;
    while (remaining > 0) {
        wchar_t wc;
        size_t n = mbrtowc(&wc, s, remaining, &state);
        if (n == 0) break;
        if (n == (size_t)-1 || n == (size_t)-2) {
            if (w + 1 > max_w) break;
            w++;
            s++;
            remaining--;
            cut = s;
            memset(&state, 0, sizeof(state));
            continue;
        }
        s += n;
        remaining -= n;
        int cw = effective_width(wc, n, &s, &remaining);
        if (cw == 0) {
            cut = s;
            continue;
        }
        if (w + cw > max_w) break;
        w += cw;
        cut = s;
    }
    return xstrndup(utf8, (size_t)(cut - utf8));
}

const char *str_skip_display_cols(const char *utf8, int cols)
{
    if (!utf8 || cols <= 0) return utf8;

    /* Fast path: pure ASCII */
    size_t slen = strlen(utf8);
    const unsigned char *p = (const unsigned char *)utf8;
    int all_ascii = 1;
    for (size_t i = 0; i < slen; i++) {
        if (p[i] > 0x7E) { all_ascii = 0; break; }
    }
    if (all_ascii) {
        if (cols >= (int)slen) return utf8 + slen;
        return utf8 + cols;
    }

    int w = 0;
    mbstate_t state;
    memset(&state, 0, sizeof(state));
    const char *s = utf8;
    size_t remaining = slen;
    while (remaining > 0) {
        wchar_t wc;
        const char *char_start = s;
        size_t n = mbrtowc(&wc, s, remaining, &state);
        if (n == 0) break;
        if (n == (size_t)-1 || n == (size_t)-2) {
            w++;
            s++;
            remaining--;
            if (w >= cols) return s;
            memset(&state, 0, sizeof(state));
            continue;
        }
        s += n;
        remaining -= n;
        int cw = effective_width(wc, n, &s, &remaining);
        if (cw == 0)
            continue;
        if (w + cw > cols)
            return char_start;  /* don't consume a char that straddles the boundary */
        w += cw;
        if (w >= cols) {
            /* Skip any trailing zero-width chars */
            while (remaining > 0) {
                mbstate_t peek;
                memset(&peek, 0, sizeof(peek));
                wchar_t wc2;
                size_t n2 = mbrtowc(&wc2, s, remaining, &peek);
                if (n2 == 0 || n2 == (size_t)-1 || n2 == (size_t)-2)
                    break;
                if (char_display_width(wc2) != 0)
                    break;
                s += n2;
                remaining -= n2;
            }
            return s;
        }
    }
    return s;
}
