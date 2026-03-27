/*
 * sender.c — FWDisplay sender
 *
 * Captures a screen, encodes to JPEG, and streams to iPad over USB.
 * Cross-platform: macOS, Linux, Windows.
 *
 * Usage: fwdisplay-sender [--monitor N] [--fps N] [--quality N]
 *                         [--width W] [--height H]
 */

#include "usbmux.h"
#include "capture.h"
#ifdef __APPLE__
#include "virtual_display.h"
#include "touch.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>

#include <turbojpeg.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#define usleep(us) Sleep((us) / 1000)
#else
#include <unistd.h>
#include <sys/time.h>
#include <arpa/inet.h>
#endif

#define DEFAULT_PORT     8765
#define DEFAULT_FPS      30
#define JPEG_QUALITY     100
#define DEFAULT_WIDTH    1024
#define DEFAULT_HEIGHT   768
#define DEFAULT_MONITOR  0

static volatile int g_running = 1;
static volatile mux_socket_t g_active_sock = MUX_INVALID_SOCKET;  /* for signal cleanup */

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
    /* Shutdown active sockets to unblock any recv()/send() calls */
    mux_socket_t s = g_active_sock;
    if (s != MUX_INVALID_SOCKET) {
#ifdef _WIN32
        shutdown(s, SD_BOTH);
#else
        shutdown(s, SHUT_RDWR);
#endif
    }
}

/* ---------- time helpers ---------- */

static double time_now(void) {
#ifdef _WIN32
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (double)cnt.QuadPart / (double)freq.QuadPart;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec * 1e-6;
#endif
}

/* ---------- frame sending ---------- */

static int send_all_bytes(mux_socket_t sock, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        int n = send(sock, p, (int)len, 0);
        if (n <= 0) return -1;
        p += n;
        len -= n;
    }
    return 0;
}

static int send_frame(mux_socket_t sock, const unsigned char *jpeg, unsigned long jpeg_size) {
    /* Wire format: [4-byte BE length][JPEG data] */
    uint32_t hdr = htonl((uint32_t)jpeg_size);
    if (send_all_bytes(sock, &hdr, 4) < 0) return -1;
    if (send_all_bytes(sock, jpeg, jpeg_size) < 0) return -1;
    return 0;
}

/* ---------- JPEG encoding ---------- */

typedef struct {
    tjhandle     handle;
    unsigned char *out_buf;
    unsigned long  out_size;
    int quality;
    int target_width;
    int target_height;
    unsigned char *scale_buf;
} encoder_t;

static int encoder_init(encoder_t *enc, int quality, int tw, int th) {
    memset(enc, 0, sizeof(*enc));
    enc->handle = tjInitCompress();
    if (!enc->handle) return -1;
    enc->quality = quality;
    enc->target_width = tw;
    enc->target_height = th;
    enc->scale_buf = NULL;
    return 0;
}

/* Simple nearest-neighbor downscale from BGRA src to BGRA dst */
static void scale_bgra(const uint8_t *src, int sw, int sh, int sstride,
                        uint8_t *dst, int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        int sy = y * sh / dh;
        const uint8_t *srow = src + sy * sstride;
        uint8_t *drow = dst + y * dw * 4;
        for (int x = 0; x < dw; x++) {
            int sx = x * sw / dw;
            const uint8_t *sp = srow + sx * 4;
            drow[x * 4 + 0] = sp[0];
            drow[x * 4 + 1] = sp[1];
            drow[x * 4 + 2] = sp[2];
            drow[x * 4 + 3] = sp[3];
        }
    }
}

static int encoder_encode(encoder_t *enc,
                           const uint8_t *bgra, int width, int height, int stride) {
    const uint8_t *src = bgra;
    int src_w = width, src_h = height, src_stride = stride;

    /* Downscale if needed */
    if (width != enc->target_width || height != enc->target_height) {
        if (!enc->scale_buf) {
            enc->scale_buf = (unsigned char *)malloc(enc->target_width * enc->target_height * 4);
        }
        scale_bgra(bgra, width, height, stride,
                   enc->scale_buf, enc->target_width, enc->target_height);
        src = enc->scale_buf;
        src_w = enc->target_width;
        src_h = enc->target_height;
        src_stride = enc->target_width * 4;
    }

    /* Let turbojpeg manage the output buffer (it allocates/reallocs as needed) */
    if (enc->out_buf) {
        tjFree(enc->out_buf);
        enc->out_buf = NULL;
    }
    enc->out_size = 0;

    int rc = tjCompress2(enc->handle,
                         src, src_w, src_stride, src_h,
                         TJPF_BGRA,
                         &enc->out_buf, &enc->out_size,
                         TJSAMP_420, enc->quality,
                         TJFLAG_FASTDCT);
    return rc;
}

static void encoder_cleanup(encoder_t *enc) {
    if (enc->handle) tjDestroy(enc->handle);
    if (enc->out_buf) tjFree(enc->out_buf);
    free(enc->scale_buf);
    memset(enc, 0, sizeof(*enc));
}

/* ---------- main ---------- */

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  --monitor N     Display index to capture (default: %d)\n"
        "  --fps N         Target frame rate (default: %d)\n"
        "  --quality N     JPEG quality 1-100 (default: %d)\n"
        "  --width W       Output width (default: %d)\n"
        "  --height H      Output height (default: %d)\n"
#ifdef __APPLE__
        "  --no-virtual    Don't create virtual display, capture --monitor instead\n"
#endif
        "\n"
#ifdef __APPLE__
        "On macOS, a virtual display is created automatically when connected.\n"
        "Use --no-virtual --monitor N to capture an existing display instead.\n"
#endif
#ifdef _WIN32
        "Prerequisites:\n"
        "  Apple Mobile Device Support (provides usbmuxd for USB communication):\n"
        "    https://github.com/koush/AppleMobileDeviceSupport/releases/tag/v14.5.0.7\n"
        "\n"
        "Windows does not support virtual display creation. The sender captures\n"
        "an existing monitor. To use as a second display, install a virtual\n"
        "display driver such as Virtual Display Driver (VDD):\n"
        "  https://github.com/VirtualDrivers/Virtual-Display-Driver/releases\n"
        "Then run: %s --monitor N  (where N is the virtual display index)\n"
#endif
#ifdef __linux__
        "Linux does not support virtual display creation. The sender captures\n"
        "an existing X11 screen. Use xrandr to create a virtual output if needed.\n"
#endif
        ,
        prog, DEFAULT_MONITOR, DEFAULT_FPS, JPEG_QUALITY,
        DEFAULT_WIDTH, DEFAULT_HEIGHT
#ifdef _WIN32
        , prog
#endif
        );
}

int main(int argc, char **argv) {
    int monitor = -1;  /* -1 = auto (create virtual display on macOS) */
    int fps     = DEFAULT_FPS;
    int quality = JPEG_QUALITY;
    int width   = DEFAULT_WIDTH;
    int height  = DEFAULT_HEIGHT;
#ifdef __APPLE__
    int no_virtual = 0;
#endif

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--monitor") == 0 && i + 1 < argc)
            monitor = atoi(argv[++i]);
        else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc)
            fps = atoi(argv[++i]);
        else if (strcmp(argv[i], "--quality") == 0 && i + 1 < argc)
            quality = atoi(argv[++i]);
        else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc)
            width = atoi(argv[++i]);
        else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc)
            height = atoi(argv[++i]);
#ifdef __APPLE__
        else if (strcmp(argv[i], "--no-virtual") == 0)
            no_virtual = 1;
#endif
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    fprintf(stderr, "[fwdisplay] Starting...\n");
    fprintf(stderr, "[fwdisplay]   Output: %dx%d @ %d fps, JPEG quality %d\n",
            width, height, fps, quality);

#ifdef __APPLE__
    int use_virtual = (!no_virtual && monitor < 0);
    if (use_virtual)
        fprintf(stderr, "[fwdisplay]   Mode: virtual display (auto-created)\n");
    else
        fprintf(stderr, "[fwdisplay]   Capturing monitor %d\n", monitor >= 0 ? monitor : 0);
#elif defined(_WIN32)
    if (monitor >= 0)
        fprintf(stderr, "[fwdisplay]   Capturing monitor %d\n", monitor);
    else
        fprintf(stderr, "[fwdisplay]   Mode: auto-detect virtual display\n");
#else
    fprintf(stderr, "[fwdisplay]   Capturing monitor %d\n", monitor >= 0 ? monitor : 0);
#endif

    /* Initialize JPEG encoder (reused across connections) */
    encoder_t enc;
    if (encoder_init(&enc, quality, width, height) < 0) {
        fprintf(stderr, "[fwdisplay] ERROR: Failed to init JPEG encoder\n");
        return 1;
    }

    double frame_interval = 1.0 / fps;

    /* Outer reconnect loop */
    while (g_running) {
        fprintf(stderr, "[fwdisplay] Waiting for iOS device on USB...\n");

        /* Connect to usbmuxd and wait for device */
        mux_socket_t listen_sock = mux_connect_daemon();
        if (listen_sock == MUX_INVALID_SOCKET) {
            fprintf(stderr, "[fwdisplay] ERROR: Cannot connect to usbmuxd\n");
#ifdef _WIN32
            fprintf(stderr, "[fwdisplay] Install Apple Mobile Device Support:\n");
            fprintf(stderr, "[fwdisplay]   https://github.com/koush/AppleMobileDeviceSupport/releases/tag/v14.5.0.7\n");
#else
            fprintf(stderr, "[fwdisplay] Is usbmuxd running?\n");
#endif
            usleep(3000000);
            continue;
        }

        g_active_sock = listen_sock;
        int device_id = mux_wait_for_device(listen_sock);
        g_active_sock = MUX_INVALID_SOCKET;
        mux_close(listen_sock);

        if (device_id < 0) {
            fprintf(stderr, "[fwdisplay] ERROR: Device discovery failed\n");
            usleep(2000000);
            continue;
        }
        fprintf(stderr, "[fwdisplay] Device found (ID: %d)\n", device_id);

        /* Connect to iPad app */
        fprintf(stderr, "[fwdisplay] Connecting to device port %d...\n", DEFAULT_PORT);
        mux_socket_t sock = mux_connect_device(listen_sock, device_id, DEFAULT_PORT);
        if (sock == MUX_INVALID_SOCKET) {
            fprintf(stderr, "[fwdisplay] Is the FWDisplay app running on the iPad?\n");
            usleep(2000000);
            continue;
        }
        fprintf(stderr, "[fwdisplay] Connected!\n");
        g_active_sock = sock;

        /* Create virtual display and init capture now that we have a connection */
        int capture_display = 0;
#ifdef __APPLE__
        if (use_virtual) {
            CGDirectDisplayID vdid = vdisplay_create(width, height, fps);
            if (vdid == 0) {
                fprintf(stderr, "[fwdisplay] ERROR: Failed to create virtual display\n");
                mux_close(sock);
                usleep(2000000);
                continue;
            }
            fprintf(stderr, "[fwdisplay] Virtual display created (ID: %u)\n", vdid);
            fprintf(stderr, "[fwdisplay]   %dx%d @ %d Hz\n", width, height, fps);

            usleep(1000000); /* give the system a moment to register */
            uint32_t max_d = 16;
            CGDirectDisplayID dlist[16];
            uint32_t dcount = 0;
            CGGetActiveDisplayList(max_d, dlist, &dcount);
            capture_display = -1;
            for (uint32_t i = 0; i < dcount; i++) {
                if (dlist[i] == vdid) {
                    capture_display = (int)i;
                    break;
                }
            }
            if (capture_display < 0) {
                fprintf(stderr, "[fwdisplay] ERROR: Virtual display not found\n");
                vdisplay_destroy();
                mux_close(sock);
                usleep(2000000);
                continue;
            }
        } else {
            capture_display = (monitor >= 0) ? monitor : 0;
        }
#elif defined(_WIN32)
        if (monitor >= 0) {
            capture_display = monitor;
        } else {
            /* Auto-detect virtual display */
            capture_display = capture_find_virtual_display();
            if (capture_display < 0) {
                fprintf(stderr, "[fwdisplay] No virtual display found, using primary monitor\n");
                capture_display = 0;
            }
        }
#else
        capture_display = (monitor >= 0) ? monitor : 0;
#endif

        if (capture_init(capture_display) < 0) {
            fprintf(stderr, "[fwdisplay] ERROR: Failed to init screen capture\n");
#ifdef __APPLE__
            if (use_virtual) vdisplay_destroy();
#endif
            mux_close(sock);
            usleep(2000000);
            continue;
        }

        fprintf(stderr, "[fwdisplay] Streaming - press Ctrl-C to stop.\n");

#ifdef __APPLE__
        /* Start touch input reader thread */
        {
            CGRect touch_bounds;
            uint32_t max_d2 = 16;
            CGDirectDisplayID dlist2[16];
            uint32_t dcount2 = 0;
            CGGetActiveDisplayList(max_d2, dlist2, &dcount2);
            if (capture_display >= 0 && (uint32_t)capture_display < dcount2) {
                touch_bounds = CGDisplayBounds(dlist2[capture_display]);
            } else {
                touch_bounds = CGRectMake(0, 0, width, height);
            }
            touch_start(sock, touch_bounds);
        }
#endif

        /* Inner streaming loop */
        uint64_t frame_count = 0;
        double start_time = time_now();
        int backoff = 0;

        while (g_running) {
            double t0 = time_now();

            /* Capture screen */
            capture_frame_t frame;
            if (capture_grab(&frame) < 0) {
                fprintf(stderr, "[fwdisplay] Capture failed, retrying...\n");
                usleep(100000);
                continue;
            }

            /* Encode to JPEG */
            if (encoder_encode(&enc, frame.pixels, frame.width, frame.height,
                               frame.stride) < 0) {
                fprintf(stderr, "[fwdisplay] JPEG encode failed\n");
                continue;
            }

            /* Send frame */
            if (send_frame(sock, enc.out_buf, enc.out_size) < 0) {
                fprintf(stderr, "[fwdisplay] Connection lost\n");
                break;
            }

            frame_count++;
            if (frame_count % (uint64_t)(fps * 5) == 0) {
                double elapsed = time_now() - start_time;
                double actual_fps = frame_count / elapsed;
                fprintf(stderr, "[fwdisplay] %llu frames | %.1f fps | %lu KB/frame\n",
                        (unsigned long long)frame_count, actual_fps,
                        (unsigned long)(enc.out_size / 1024));
            }

            /* Frame rate control */
            double dt = time_now() - t0;
            double sleep_time = frame_interval - dt;
            if (sleep_time > 0.001) {
                usleep((unsigned)(sleep_time * 1e6));
            }

            backoff = 0;
        }

        g_active_sock = MUX_INVALID_SOCKET;
#ifdef __APPLE__
        touch_stop();
#endif
        mux_close(sock);
        capture_cleanup();
#ifdef __APPLE__
        if (use_virtual) {
            vdisplay_destroy();
            fprintf(stderr, "[fwdisplay] Virtual display removed\n");
        }
#endif

        if (g_running) {
            /* Reconnect with backoff */
            backoff = backoff < 5 ? backoff + 1 : 5;
            fprintf(stderr, "[fwdisplay] Reconnecting in %d seconds...\n", backoff);
            for (int i = 0; i < backoff && g_running; i++)
                usleep(1000000);
        }
    }

    fprintf(stderr, "\n[fwdisplay] Shutting down.\n");
    encoder_cleanup(&enc);
    return 0;
}
