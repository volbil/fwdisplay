/*
 * virtual_display.h — create a macOS virtual display via private API
 *
 * Uses the private CGVirtualDisplay API (macOS 12.4+).
 * The display appears in System Settings → Displays.
 */

#ifndef VIRTUAL_DISPLAY_H
#define VIRTUAL_DISPLAY_H

#include <CoreGraphics/CoreGraphics.h>

/* Create a virtual display with the given resolution and refresh rate.
 * Returns the CGDirectDisplayID, or 0 on failure.
 * The display lives until the process exits (or vdisplay_destroy is called). */
CGDirectDisplayID vdisplay_create(unsigned int width, unsigned int height, int refresh_rate);

/* Destroy the virtual display. */
void vdisplay_destroy(void);

#endif /* VIRTUAL_DISPLAY_H */
