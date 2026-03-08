#include "uniwidth.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <locale.h>

int char_display_width(wchar_t ch)
{
    /* Fast path: printable ASCII */
    if (ch >= 0x20 && ch <= 0x7E)
        return 1;

    /* Control characters */
    if (ch < 0x20 || (ch >= 0x7F && ch <= 0x9F))
        return 0;

    /* Variation selectors: width 1 (they widen narrow emoji) */
    if (ch >= 0xFE00 && ch <= 0xFE0F)
        return 1;

    /* Zero-width characters */
    /* Combining marks, format chars, ZWJ, etc. */
    if (ch == 0x200B || ch == 0x200C || ch == 0x200D || ch == 0xFEFF)
        return 0;
    /* Combining Diacritical Marks and common combining ranges */
    if ((ch >= 0x0300 && ch <= 0x036F) ||  /* Combining Diacritical Marks */
        (ch >= 0x0483 && ch <= 0x0489) ||  /* Cyrillic combining */
        (ch >= 0x0591 && ch <= 0x05BD) ||  /* Hebrew combining */
        (ch >= 0x0610 && ch <= 0x061A) ||  /* Arabic combining */
        (ch >= 0x064B && ch <= 0x065F) ||  /* Arabic combining */
        (ch >= 0x0670 && ch == 0x0670) ||  /* Arabic combining */
        (ch >= 0x06D6 && ch <= 0x06DC) ||  /* Arabic combining */
        (ch >= 0x06DF && ch <= 0x06E4) ||  /* Arabic combining */
        (ch >= 0x0E31 && ch == 0x0E31) ||  /* Thai combining */
        (ch >= 0x0E34 && ch <= 0x0E3A) ||  /* Thai combining */
        (ch >= 0x20D0 && ch <= 0x20FF) ||  /* Combining Diacritical Marks for Symbols */
        (ch >= 0xFE20 && ch <= 0xFE2F))    /* Combining Half Marks */
        return 0;

    /* Emoji symbols: terminals render wide despite neutral ea_width.
     * Matches the Python logic: category So at U+2600+ = 2.
     * This catches hearts, stars, weather symbols, etc. */
    if (ch >= 0x2600 && ch <= 0x27BF)
        return 2;
    /* Miscellaneous Symbols and Pictographs, Emoticons, etc. */
    if (ch >= 0x1F300 && ch <= 0x1F9FF)
        return 2;
    /* Transport and Map Symbols */
    if (ch >= 0x1F680 && ch <= 0x1F6FF)
        return 2;
    /* Supplemental Symbols */
    if (ch >= 0x1FA00 && ch <= 0x1FA6F)
        return 2;
    if (ch >= 0x1FA70 && ch <= 0x1FAFF)
        return 2;
    /* Dingbats */
    if (ch >= 0x2700 && ch <= 0x27BF)
        return 2;
    /* Regional indicators */
    if (ch >= 0x1F1E0 && ch <= 0x1F1FF)
        return 2;

    /* Use wcwidth() for everything else (handles CJK, fullwidth, etc.) */
    int w = wcwidth(ch);
    if (w < 0) return 0;  /* non-printable */
    return w;
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

    /* Decode UTF-8 codepoints */
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
            /* Invalid byte: skip it, count as width 1 */
            width++;
            s++;
            remaining--;
            memset(&state, 0, sizeof(state));
            continue;
        }
        width += char_display_width(wc);
        s += n;
        remaining -= n;
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

    /* Walk codepoints */
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
        int cw = char_display_width(wc);
        if (cw == 0) {
            /* Always include zero-width chars */
            s += n;
            remaining -= n;
            cut = s;
            continue;
        }
        if (w + cw > max_w) break;
        w += cw;
        s += n;
        remaining -= n;
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

    /* Walk codepoints */
    int w = 0;
    mbstate_t state;
    memset(&state, 0, sizeof(state));
    const char *s = utf8;
    size_t remaining = slen;
    while (remaining > 0) {
        wchar_t wc;
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
        int cw = char_display_width(wc);
        if (cw == 0) {
            s += n;
            remaining -= n;
            continue;
        }
        if (w + cw > cols)
            return s;
        w += cw;
        s += n;
        remaining -= n;
        if (w >= cols) {
            /* Skip trailing zero-width chars */
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
