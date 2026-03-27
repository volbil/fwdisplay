/*
 * capture.h -- platform-specific screen capture
 *
 * macOS:   CGDisplayCreateImage (runtime loaded)
 * Windows: GDI BitBlt
 * Linux:   X11 XShmGetImage
 */

#ifndef CAPTURE_H
#define CAPTURE_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint8_t *pixels;     /* BGRA 8-bit per channel */
    int      width;
    int      height;
    int      stride;     /* bytes per row */
} capture_frame_t;

/* Initialize capture for the given display index (0 = primary).
 * Returns 0 on success. */
int capture_init(int display_index);

/* Grab one frame. Fills `frame` with a pointer to an internal buffer.
 * The buffer is valid until the next call to capture_grab().
 * Returns 0 on success. */
int capture_grab(capture_frame_t *frame);

/* Clean up. */
void capture_cleanup(void);

#ifdef _WIN32
/* Auto-detect a virtual display (IddSampleDriver etc).
 * Returns monitor index or -1 if not found. */
int capture_find_virtual_display(void);
#endif

#endif /* CAPTURE_H */
