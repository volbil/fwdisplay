/*
 * capture_mac.c — macOS screen capture via CoreGraphics (runtime loaded)
 *
 * CGDisplayCreateImage is marked unavailable in the macOS 15+ SDK
 * but still exists at runtime. We load it via dlsym.
 * Cursor composited via AppKit NSCursor (see cursor_mac.m).
 * Requires Screen Recording permission in System Settings.
 */

#ifdef __APPLE__

#include "capture.h"
#include "cursor_mac.h"

#include <CoreGraphics/CoreGraphics.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Runtime-loaded function pointer */
typedef CGImageRef (*CGDisplayCreateImage_t)(CGDirectDisplayID);
static CGDisplayCreateImage_t pCGDisplayCreateImage = NULL;

static CGDirectDisplayID s_display_id;
static CGRect            s_display_bounds;  /* global logical coords */
static uint8_t          *s_buffer;
static size_t            s_buffer_size;
static int               s_pixel_width;
static int               s_pixel_height;
static double            s_scale;

int capture_init(int display_index) {
    void *cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_LAZY);
    if (cg) {
        pCGDisplayCreateImage = (CGDisplayCreateImage_t)dlsym(cg, "CGDisplayCreateImage");
    }
    if (!pCGDisplayCreateImage) {
        fprintf(stderr, "[fwdisplay] ERROR: CGDisplayCreateImage not available\n");
        return -1;
    }

    uint32_t max_displays = 16;
    CGDirectDisplayID displays[16];
    uint32_t count = 0;

    CGGetActiveDisplayList(max_displays, displays, &count);
    if ((uint32_t)display_index >= count) {
        fprintf(stderr, "[fwdisplay] Display %d not found (%u available)\n",
                display_index, count);
        return -1;
    }

    s_display_id = displays[display_index];
    s_display_bounds = CGDisplayBounds(s_display_id);

    /* Test capture to get actual pixel dimensions */
    CGImageRef test = pCGDisplayCreateImage(s_display_id);
    if (!test) {
        fprintf(stderr, "[fwdisplay] ERROR: Screen capture failed.\n");
        fprintf(stderr, "[fwdisplay] Grant Screen Recording permission in System Settings.\n");
        return -1;
    }
    s_pixel_width  = (int)CGImageGetWidth(test);
    s_pixel_height = (int)CGImageGetHeight(test);
    CGImageRelease(test);

    s_scale = (double)s_pixel_width / s_display_bounds.size.width;

    s_buffer_size = (size_t)s_pixel_width * s_pixel_height * 4;
    s_buffer = (uint8_t *)malloc(s_buffer_size);
    if (!s_buffer) return -1;

    fprintf(stderr, "[fwdisplay] Capturing display %d: %dx%d (scale %.0fx)\n",
            display_index, s_pixel_width, s_pixel_height, s_scale);
    return 0;
}

/* Alpha-blend cursor onto framebuffer */
static void draw_cursor(uint8_t *buf, int buf_w, int buf_h) {
    cursor_info_t ci;
    if (cursor_get(&ci) != 0) return;

    /* Check if cursor is on our display (global logical coords) */
    double cx = ci.cursor_x;
    double cy = ci.cursor_y;
    CGRect db = s_display_bounds;
    if (cx < db.origin.x || cx >= db.origin.x + db.size.width ||
        cy < db.origin.y || cy >= db.origin.y + db.size.height) {
        free(ci.pixels);
        return;
    }

    /* Convert to local pixel coords */
    int px = (int)((cx - db.origin.x) * s_scale) - ci.hotspot_x;
    int py = (int)((cy - db.origin.y) * s_scale) - ci.hotspot_y;

    /* Blend */
    for (int row = 0; row < ci.height; row++) {
        int dy = py + row;
        if (dy < 0 || dy >= buf_h) continue;

        /* cursor_mac.m renders into CGBitmapContext which is already top-down */
        uint8_t *srow = ci.pixels + row * ci.width * 4;
        uint8_t *drow = buf + dy * buf_w * 4;

        for (int col = 0; col < ci.width; col++) {
            int dx = px + col;
            if (dx < 0 || dx >= buf_w) continue;

            uint8_t *sp = srow + col * 4;
            uint8_t sa = sp[3];
            if (sa == 0) continue;

            uint8_t *dp = drow + dx * 4;
            if (sa == 255) {
                dp[0] = sp[0];
                dp[1] = sp[1];
                dp[2] = sp[2];
                dp[3] = 255;
            } else {
                uint8_t inv = 255 - sa;
                dp[0] = sp[0] + (dp[0] * inv) / 255;
                dp[1] = sp[1] + (dp[1] * inv) / 255;
                dp[2] = sp[2] + (dp[2] * inv) / 255;
                dp[3] = 255;
            }
        }
    }
    free(ci.pixels);
}

int capture_grab(capture_frame_t *frame) {
    CGImageRef img = pCGDisplayCreateImage(s_display_id);
    if (!img) return -1;

    int w = (int)CGImageGetWidth(img);
    int h = (int)CGImageGetHeight(img);

    size_t needed = (size_t)w * h * 4;
    if (needed > s_buffer_size) {
        free(s_buffer);
        s_buffer_size = needed;
        s_buffer = (uint8_t *)malloc(s_buffer_size);
        if (!s_buffer) { CGImageRelease(img); return -1; }
        s_pixel_width = w;
        s_pixel_height = h;
    }

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(
        s_buffer, w, h, 8, w * 4, cs,
        kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst);
    CGColorSpaceRelease(cs);

    if (!ctx) {
        CGImageRelease(img);
        return -1;
    }

    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
    CGContextRelease(ctx);
    CGImageRelease(img);

    /* Composite cursor */
    draw_cursor(s_buffer, w, h);

    frame->pixels = s_buffer;
    frame->width  = w;
    frame->height = h;
    frame->stride = w * 4;
    return 0;
}

void capture_cleanup(void) {
    free(s_buffer);
    s_buffer = NULL;
    s_buffer_size = 0;
}

#endif /* __APPLE__ */
