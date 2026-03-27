/*
 * touch.c — read touch events from iPad and simulate mouse input
 *
 * Left click:       single-finger tap
 * Right click:      two-finger tap (phase=4 from iPad)
 * Drag:             touch and move
 */

#ifdef __APPLE__

#include "touch.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define TOUCH_MSG_TYPE  0x01
#define TOUCH_MSG_SIZE  10

enum {
    PHASE_BEGAN      = 0,
    PHASE_MOVED      = 1,
    PHASE_ENDED      = 2,
    PHASE_CANCELLED  = 3,
    PHASE_RIGHTCLICK = 4,
};

static pthread_t s_thread;
static volatile int s_running;
static mux_socket_t s_sock;
static CGRect s_bounds;
static int s_mouse_down;

static int recv_all(mux_socket_t sock, void *buf, size_t len) {
    char *p = (char *)buf;
    while (len > 0) {
        ssize_t n = recv(sock, p, len, 0);
        if (n <= 0) return -1;
        p += n;
        len -= n;
    }
    return 0;
}

static float decode_be_float(const uint8_t *p) {
    uint32_t bits;
    memcpy(&bits, p, 4);
    bits = ntohl(bits);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static void post_mouse_event(CGEventType type, CGPoint pos, CGMouseButton button) {
    CGEventRef ev = CGEventCreateMouseEvent(NULL, type, pos, button);
    if (ev) {
        CGEventPost(kCGHIDEventTap, ev);
        CFRelease(ev);
    }
}

static void handle_touch(uint8_t phase, float nx, float ny) {
    CGPoint pos;
    pos.x = s_bounds.origin.x + nx * s_bounds.size.width;
    pos.y = s_bounds.origin.y + ny * s_bounds.size.height;

    switch (phase) {
        case PHASE_BEGAN:
            post_mouse_event(kCGEventLeftMouseDown, pos, kCGMouseButtonLeft);
            s_mouse_down = 1;
            break;

        case PHASE_MOVED:
            if (s_mouse_down) {
                post_mouse_event(kCGEventLeftMouseDragged, pos, kCGMouseButtonLeft);
            } else {
                post_mouse_event(kCGEventMouseMoved, pos, kCGMouseButtonLeft);
            }
            break;

        case PHASE_ENDED:
            post_mouse_event(kCGEventLeftMouseUp, pos, kCGMouseButtonLeft);
            s_mouse_down = 0;
            break;

        case PHASE_CANCELLED:
            if (s_mouse_down) {
                post_mouse_event(kCGEventLeftMouseUp, pos, kCGMouseButtonLeft);
                s_mouse_down = 0;
            }
            break;

        case PHASE_RIGHTCLICK: {
            /* Two-finger tap → right click at current cursor position */
            CGEventRef locEv = CGEventCreate(NULL);
            CGPoint cur = CGEventGetLocation(locEv);
            CFRelease(locEv);
            post_mouse_event(kCGEventRightMouseDown, cur, kCGMouseButtonRight);
            post_mouse_event(kCGEventRightMouseUp, cur, kCGMouseButtonRight);
            break;
        }
    }
}

static void *touch_thread(void *arg) {
    (void)arg;
    uint8_t buf[TOUCH_MSG_SIZE];

    fprintf(stderr, "[fwdisplay] Touch input active (two-finger tap = right click)\n");

    while (s_running) {
        if (recv_all(s_sock, buf, TOUCH_MSG_SIZE) < 0) {
            if (s_running) {
                fprintf(stderr, "[fwdisplay] Touch reader: connection closed\n");
            }
            break;
        }

        if (buf[0] != TOUCH_MSG_TYPE) continue;

        uint8_t phase = buf[1];
        float x = decode_be_float(&buf[2]);
        float y = decode_be_float(&buf[6]);

        handle_touch(phase, x, y);
    }

    return NULL;
}

void touch_start(mux_socket_t sock, CGRect display_bounds) {
    s_sock = sock;
    s_bounds = display_bounds;
    s_mouse_down = 0;
    s_running = 1;
    pthread_create(&s_thread, NULL, touch_thread, NULL);
}

void touch_stop(void) {
    s_running = 0;
    pthread_join(s_thread, NULL);
}

#endif /* __APPLE__ */
