/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 David Walther */
#include "clipboard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Platform detection ── */

/*
 * WSL: /proc/version contains "Microsoft" or "WSL".
 * macOS: __APPLE__ is defined at compile time.
 * Wayland: $WAYLAND_DISPLAY is set at runtime.
 * X11: $DISPLAY is set at runtime.
 */

static int is_wsl(void)
{
#ifdef __linux__
    FILE *f = fopen("/proc/version", "r");
    if (!f) return 0;
    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, "Microsoft") || strstr(buf, "WSL");
#else
    return 0;
#endif
}

ClipPlatform clipboard_platform(void)
{
    static int cached = 0;
    static ClipPlatform result;
    if (cached) return result;
    cached = 1;

#ifdef __APPLE__
    result = CLIP_MACOS;
#else
    if (is_wsl()) {
        result = CLIP_WSL;
    } else if (getenv("WAYLAND_DISPLAY")) {
        result = CLIP_WAYLAND;
    } else if (getenv("DISPLAY")) {
        result = CLIP_X11;
    } else {
        result = CLIP_NONE;
    }
#endif
    return result;
}

const char *clipboard_platform_name(ClipPlatform p)
{
    switch (p) {
    case CLIP_WSL:     return "WSL/Windows";
    case CLIP_MACOS:   return "macOS";
    case CLIP_WAYLAND: return "Wayland";
    case CLIP_X11:     return "X11";
    case CLIP_NONE:    return "none";
    }
    return "unknown";
}

/* ── Copy ── */

int clipboard_copy(const char *text)
{
    if (!text) text = "";
    FILE *fp = NULL;

    switch (clipboard_platform()) {
    case CLIP_WSL:
        fp = popen("clip.exe", "w");
        break;
    case CLIP_MACOS:
        fp = popen("pbcopy", "w");
        break;
    case CLIP_WAYLAND:
        fp = popen("wl-copy", "w");
        break;
    case CLIP_X11:
        fp = popen("xclip -selection clipboard", "w");
        break;
    case CLIP_NONE:
        return 0;
    }

    if (!fp) return 0;
    fputs(text, fp);
    return pclose(fp) == 0 ? 1 : 0;
}

/* ── Paste ── */

char *clipboard_paste(void)
{
    FILE *fp = NULL;

    switch (clipboard_platform()) {
    case CLIP_WSL:
        /* powershell outputs UTF-16 by default; -encoding utf8 gives plain bytes */
        fp = popen("powershell.exe -command \"[Console]::OutputEncoding=[System.Text.Encoding]::UTF8; Get-Clipboard\"", "r");
        break;
    case CLIP_MACOS:
        fp = popen("pbpaste", "r");
        break;
    case CLIP_WAYLAND:
        fp = popen("wl-paste --no-newline", "r");
        break;
    case CLIP_X11:
        fp = popen("xclip -selection clipboard -o", "r");
        break;
    case CLIP_NONE:
        return NULL;
    }

    if (!fp) return NULL;

    /* Read all output into a heap buffer. */
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) { pclose(fp); return NULL; }

    size_t n;
    while ((n = fread(buf + len, 1, cap - len - 1, fp)) > 0) {
        len += n;
        if (len + 1 >= cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) { free(buf); pclose(fp); return NULL; }
            buf = tmp;
        }
    }
    pclose(fp);
    buf[len] = '\0';

    /* powershell.exe appends \r\n; strip a single trailing \r\n if present */
    if (clipboard_platform() == CLIP_WSL && len >= 2 &&
        buf[len-2] == '\r' && buf[len-1] == '\n') {
        buf[len-2] = '\0';
    }

    return buf;
}
