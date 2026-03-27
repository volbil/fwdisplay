/*
 * cursor_mac.h — get cursor image and position on macOS
 */

#ifndef CURSOR_MAC_H
#define CURSOR_MAC_H

#include <stdint.h>

typedef struct {
    uint8_t *pixels;   /* BGRA premultiplied, caller must free() */
    int      width;
    int      height;
    int      hotspot_x;
    int      hotspot_y;
    double   cursor_x; /* global logical screen coords */
    double   cursor_y;
} cursor_info_t;

/* Get current cursor image and position. Returns 0 on success.
 * Caller must free(info->pixels) after use. */
int cursor_get(cursor_info_t *info);

#endif
