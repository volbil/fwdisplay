/*
 * capture_linux.c — Linux screen capture via X11 + XShm
 *
 * Captures the root window of the specified screen.
 * Requires: libx11-dev, libxext-dev
 */

#ifdef __linux__

#include "capture.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <sys/shm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Display          *s_display;
static Window            s_root;
static XShmSegmentInfo   s_shm_info;
static XImage           *s_image;
static int               s_width;
static int               s_height;
static uint8_t          *s_buffer;

int capture_init(int display_index) {
    s_display = XOpenDisplay(NULL);
    if (!s_display) {
        fprintf(stderr, "[fwdisplay] Cannot open X display\n");
        return -1;
    }

    int screen = DefaultScreen(s_display);
    s_root = RootWindow(s_display, screen);
    s_width  = DisplayWidth(s_display, screen);
    s_height = DisplayHeight(s_display, screen);

    /* Set up shared memory for fast capture */
    s_image = XShmCreateImage(s_display,
                              DefaultVisual(s_display, screen),
                              DefaultDepth(s_display, screen),
                              ZPixmap, NULL, &s_shm_info,
                              s_width, s_height);
    if (!s_image) {
        fprintf(stderr, "[fwdisplay] XShmCreateImage failed\n");
        XCloseDisplay(s_display);
        return -1;
    }

    s_shm_info.shmid = shmget(IPC_PRIVATE,
                               s_image->bytes_per_line * s_image->height,
                               IPC_CREAT | 0777);
    s_shm_info.shmaddr = s_image->data = (char *)shmat(s_shm_info.shmid, NULL, 0);
    s_shm_info.readOnly = False;

    XShmAttach(s_display, &s_shm_info);

    /* BGRA conversion buffer */
    s_buffer = (uint8_t *)malloc(s_width * s_height * 4);

    fprintf(stderr, "[fwdisplay] Capturing X11 screen: %dx%d\n", s_width, s_height);
    return 0;
}

int capture_grab(capture_frame_t *frame) {
    XShmGetImage(s_display, s_root, s_image, 0, 0, AllPlanes);

    /* X11 gives us BGRA (on most systems with 24/32 bit depth) */
    int src_stride = s_image->bytes_per_line;
    int dst_stride = s_width * 4;

    for (int y = 0; y < s_height; y++) {
        memcpy(s_buffer + y * dst_stride,
               s_image->data + y * src_stride,
               dst_stride);
    }

    frame->pixels = s_buffer;
    frame->width  = s_width;
    frame->height = s_height;
    frame->stride = dst_stride;
    return 0;
}

void capture_cleanup(void) {
    if (s_display) {
        XShmDetach(s_display, &s_shm_info);
        XDestroyImage(s_image);
        shmdt(s_shm_info.shmaddr);
        shmctl(s_shm_info.shmid, IPC_RMID, NULL);
        XCloseDisplay(s_display);
    }
    free(s_buffer);
}

#endif /* __linux__ */
