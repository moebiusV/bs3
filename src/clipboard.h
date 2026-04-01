/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 David Walther */
#ifndef CLIPBOARD_H
#define CLIPBOARD_H

typedef enum {
    CLIP_WSL,       /* Windows Subsystem for Linux — clip.exe / powershell.exe */
    CLIP_MACOS,     /* macOS — pbcopy / pbpaste */
    CLIP_WAYLAND,   /* Linux/BSD Wayland — wl-copy / wl-paste */
    CLIP_X11,       /* Linux/BSD X11 — xclip */
    CLIP_NONE,      /* No system clipboard available */
} ClipPlatform;

/* Detect and return the clipboard platform (cached after first call). */
ClipPlatform clipboard_platform(void);

/* Human-readable name for the detected platform. */
const char *clipboard_platform_name(ClipPlatform p);

/* Copy text to the system clipboard.
 * Returns 1 on success, 0 on failure. */
int clipboard_copy(const char *text);

/* Paste text from the system clipboard.
 * Returns a malloc'd string the caller must free, or NULL on failure. */
char *clipboard_paste(void);

#endif
