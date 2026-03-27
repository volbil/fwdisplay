/*
 * cursor_mac.m — get cursor image and position via AppKit (public API)
 */

#ifdef __APPLE__

#import "cursor_mac.h"
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

int cursor_get(cursor_info_t *info) {
    @autoreleasepool {
        /* Get cursor position (global screen coords, origin bottom-left) */
        NSPoint mouse_loc = [NSEvent mouseLocation];

        /* Convert to top-left origin for CoreGraphics coordinate space */
        NSScreen *mainScreen = [NSScreen screens].firstObject;
        if (!mainScreen) return -1;
        double screen_height = mainScreen.frame.size.height;

        info->cursor_x = mouse_loc.x;
        info->cursor_y = screen_height - mouse_loc.y;

        /* Get current cursor image */
        NSCursor *cursor = [NSCursor currentSystemCursor];
        if (!cursor) cursor = [NSCursor arrowCursor];

        NSImage *img = [cursor image];
        if (!img) return -1;

        NSPoint hotspot = [cursor hotSpot];
        info->hotspot_x = (int)hotspot.x;
        info->hotspot_y = (int)hotspot.y;

        /* Render NSImage to BGRA bitmap */
        NSSize size = [img size];
        int w = (int)size.width;
        int h = (int)size.height;
        if (w <= 0 || h <= 0) return -1;

        info->width  = w;
        info->height = h;
        info->pixels = (uint8_t *)calloc(w * h, 4);
        if (!info->pixels) return -1;

        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(
            info->pixels, w, h, 8, w * 4, cs,
            kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst);
        CGColorSpaceRelease(cs);
        if (!ctx) { free(info->pixels); info->pixels = NULL; return -1; }

        /* Draw NSImage into CG context */
        NSGraphicsContext *gc = [NSGraphicsContext graphicsContextWithCGContext:ctx flipped:NO];
        [NSGraphicsContext saveGraphicsState];
        [NSGraphicsContext setCurrentContext:gc];
        [img drawInRect:NSMakeRect(0, 0, w, h)
               fromRect:NSZeroRect
              operation:NSCompositingOperationSourceOver
               fraction:1.0];
        [NSGraphicsContext restoreGraphicsState];

        CGContextRelease(ctx);
        return 0;
    }
}

#endif /* __APPLE__ */
