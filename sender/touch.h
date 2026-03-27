/*
 * touch.h — read touch events from iPad and simulate mouse input
 *
 * Runs a background thread that reads 10-byte touch events from the socket
 * and translates them to mouse events on the host OS.
 *
 * Wire format (iPad → sender):
 *   [1 byte: msg type 0x01][1 byte: phase][4 bytes: float BE x][4 bytes: float BE y]
 *   phase: 0=began, 1=moved, 2=ended, 3=cancelled
 *   x, y: normalized 0.0–1.0
 */

#ifndef TOUCH_H
#define TOUCH_H

#include "usbmux.h"
#include <CoreGraphics/CoreGraphics.h>

/* Start the touch reader thread.
 * display_bounds: the screen region being captured (for coordinate mapping).
 * sock: the TCP socket (shared with frame sender — reads are safe alongside writes). */
void touch_start(mux_socket_t sock, CGRect display_bounds);

/* Stop the touch reader thread. */
void touch_stop(void);

#endif /* TOUCH_H */
