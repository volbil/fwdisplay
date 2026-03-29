/*
 * capture_mac.m — macOS screen capture via ScreenCaptureKit
 *
 * Uses SCStream (macOS 12.3+) for high-fps capture.
 * Frames are delivered asynchronously; capture_grab() returns the latest.
 * Cursor composited via AppKit NSCursor (see cursor_mac.m).
 * Requires Screen Recording permission in System Settings.
 */

#ifdef __APPLE__

#include "capture.h"
#include "cursor_mac.h"

#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

/* ---------- State ---------- */

static CGDirectDisplayID s_display_id;
static CGRect            s_display_bounds;
static double            s_scale;

static uint8_t          *s_buffer;       /* latest frame (BGRA) */
static size_t            s_buffer_size;
static int               s_pixel_width;
static int               s_pixel_height;
static int               s_stride;

static uint8_t          *s_back_buffer;  /* written by callback */
static uint8_t          *s_cursor_buf;   /* copy with cursor composited */
static pthread_mutex_t   s_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int      s_frame_ready;

static SCStream         *s_stream;
static dispatch_queue_t  s_capture_queue;

/* ---------- SCStreamOutput delegate ---------- */

@interface FWDCaptureDelegate : NSObject <SCStreamOutput>
@end

@implementation FWDCaptureDelegate

- (void)stream:(SCStream *)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type {
    if (type != SCStreamOutputTypeScreen) return;

    CVPixelBufferRef pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!pixelBuffer) return;

    CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);

    void *base = CVPixelBufferGetBaseAddress(pixelBuffer);
    size_t w = CVPixelBufferGetWidth(pixelBuffer);
    size_t h = CVPixelBufferGetHeight(pixelBuffer);
    size_t bpr = CVPixelBufferGetBytesPerRow(pixelBuffer);

    if (base && w > 0 && h > 0) {
        pthread_mutex_lock(&s_lock);

        /* Resize buffers if needed */
        size_t needed = h * w * 4;
        if (needed > s_buffer_size) {
            free(s_back_buffer);
            s_back_buffer = (uint8_t *)malloc(needed);
            free(s_buffer);
            s_buffer = (uint8_t *)malloc(needed);
            free(s_cursor_buf);
            s_cursor_buf = (uint8_t *)malloc(needed);
            s_buffer_size = needed;
        }

        s_pixel_width = (int)w;
        s_pixel_height = (int)h;
        s_stride = (int)(w * 4);

        /* Copy row by row (bpr may differ from w*4 due to padding) */
        for (size_t row = 0; row < h; row++) {
            memcpy(s_back_buffer + row * w * 4,
                   (uint8_t *)base + row * bpr,
                   w * 4);
        }

        s_frame_ready = 1;
        pthread_mutex_unlock(&s_lock);
    }

    CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
}

@end

static FWDCaptureDelegate *s_delegate;

/* ---------- Alpha-blend cursor onto framebuffer ---------- */

static void draw_cursor(uint8_t *buf, int buf_w, int buf_h) {
    cursor_info_t ci;
    if (cursor_get(&ci) != 0) return;

    double cx = ci.cursor_x;
    double cy = ci.cursor_y;
    CGRect db = s_display_bounds;
    if (cx < db.origin.x || cx >= db.origin.x + db.size.width ||
        cy < db.origin.y || cy >= db.origin.y + db.size.height) {
        free(ci.pixels);
        return;
    }

    int px = (int)((cx - db.origin.x) * s_scale) - ci.hotspot_x;
    int py = (int)((cy - db.origin.y) * s_scale) - ci.hotspot_y;

    for (int row = 0; row < ci.height; row++) {
        int dy = py + row;
        if (dy < 0 || dy >= buf_h) continue;

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
                dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = 255;
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

/* ---------- Public interface ---------- */

int capture_init(int display_index) {
    @autoreleasepool {
        /* Find the target display */
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

        /* Get shareable content synchronously */
        __block SCShareableContent *content = nil;
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);

        [SCShareableContent getShareableContentWithCompletionHandler:
            ^(SCShareableContent *shareableContent, NSError *error) {
                if (error) {
                    fprintf(stderr, "[fwdisplay] ERROR: ScreenCaptureKit: %s\n",
                            error.localizedDescription.UTF8String);
                } else {
                    content = shareableContent;
                }
                dispatch_semaphore_signal(sem);
            }];
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);

        if (!content) {
            fprintf(stderr, "[fwdisplay] ERROR: Screen capture failed.\n");
            fprintf(stderr, "[fwdisplay] Grant Screen Recording permission in System Settings.\n");
            return -1;
        }

        /* Find our display in the shareable displays */
        SCDisplay *targetDisplay = nil;
        for (SCDisplay *d in content.displays) {
            if (d.displayID == s_display_id) {
                targetDisplay = d;
                break;
            }
        }

        if (!targetDisplay) {
            fprintf(stderr, "[fwdisplay] ERROR: Display %d not found in shareable content\n",
                    display_index);
            return -1;
        }

        s_pixel_width = (int)targetDisplay.width;
        s_pixel_height = (int)targetDisplay.height;
        s_scale = (double)s_pixel_width / s_display_bounds.size.width;

        /* Allocate buffers */
        s_buffer_size = (size_t)s_pixel_width * s_pixel_height * 4;
        s_buffer = (uint8_t *)malloc(s_buffer_size);
        s_back_buffer = (uint8_t *)malloc(s_buffer_size);
        s_cursor_buf = (uint8_t *)malloc(s_buffer_size);
        s_stride = s_pixel_width * 4;
        if (!s_buffer || !s_back_buffer || !s_cursor_buf) return -1;

        /* Create stream configuration */
        SCContentFilter *filter = [[SCContentFilter alloc]
            initWithDisplay:targetDisplay excludingWindows:@[]];

        SCStreamConfiguration *config = [[SCStreamConfiguration alloc] init];
        config.width = s_pixel_width;
        config.height = s_pixel_height;
        config.minimumFrameInterval = CMTimeMake(1, 60);
        config.pixelFormat = kCVPixelFormatType_32BGRA;
        config.showsCursor = NO;  /* we composite cursor ourselves */
        config.queueDepth = 3;

        /* Create and start stream */
        s_stream = [[SCStream alloc] initWithFilter:filter
                                      configuration:config
                                           delegate:nil];

        s_delegate = [[FWDCaptureDelegate alloc] init];
        s_capture_queue = dispatch_queue_create("com.fwdisplay.capture",
                                                 DISPATCH_QUEUE_SERIAL);

        NSError *addError = nil;
        [s_stream addStreamOutput:s_delegate
                             type:SCStreamOutputTypeScreen
                   sampleHandlerQueue:s_capture_queue
                            error:&addError];
        if (addError) {
            fprintf(stderr, "[fwdisplay] ERROR: addStreamOutput: %s\n",
                    addError.localizedDescription.UTF8String);
            return -1;
        }

        __block NSError *startError = nil;
        dispatch_semaphore_t startSem = dispatch_semaphore_create(0);
        [s_stream startCaptureWithCompletionHandler:^(NSError *error) {
            startError = error;
            dispatch_semaphore_signal(startSem);
        }];
        dispatch_semaphore_wait(startSem, DISPATCH_TIME_FOREVER);

        if (startError) {
            fprintf(stderr, "[fwdisplay] ERROR: startCapture: %s\n",
                    startError.localizedDescription.UTF8String);
            return -1;
        }

        fprintf(stderr, "[fwdisplay] Capturing display %d: %dx%d (scale %.0fx) via ScreenCaptureKit\n",
                display_index, s_pixel_width, s_pixel_height, s_scale);
        return 0;
    }
}

int capture_grab(capture_frame_t *frame) {
    /* Swap back buffer → front buffer if a new frame is ready */
    pthread_mutex_lock(&s_lock);
    if (s_frame_ready) {
        uint8_t *tmp = s_buffer;
        s_buffer = s_back_buffer;
        s_back_buffer = tmp;
        s_frame_ready = 0;
    }
    int w = s_pixel_width;
    int h = s_pixel_height;
    int st = s_stride;

    if (!s_buffer || w == 0) {
        pthread_mutex_unlock(&s_lock);
        return -1;
    }

    /* Copy to cursor buffer so the original frame stays clean */
    memcpy(s_cursor_buf, s_buffer, (size_t)h * st);
    pthread_mutex_unlock(&s_lock);

    /* Composite cursor onto the copy */
    draw_cursor(s_cursor_buf, w, h);

    frame->pixels = s_cursor_buf;
    frame->width  = w;
    frame->height = h;
    frame->stride = st;
    return 0;
}

void capture_cleanup(void) {
    @autoreleasepool {
        if (s_stream) {
            dispatch_semaphore_t sem = dispatch_semaphore_create(0);
            [s_stream stopCaptureWithCompletionHandler:^(NSError *error) {
                (void)error;
                dispatch_semaphore_signal(sem);
            }];
            dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
            s_stream = nil;
        }
        s_delegate = nil;
        s_capture_queue = nil;

        free(s_buffer);
        free(s_back_buffer);
        free(s_cursor_buf);
        s_buffer = NULL;
        s_back_buffer = NULL;
        s_cursor_buf = NULL;
        s_buffer_size = 0;
        s_frame_ready = 0;
    }
}

#endif /* __APPLE__ */
