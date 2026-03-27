/*
 * capture_win.c -- Windows screen capture via GDI
 *
 * Uses BitBlt for broad compatibility (Windows 7+).
 * Supports capturing a specific monitor by index.
 */

#ifdef _WIN32

#include "capture.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Monitor enumeration */
#define FWD_MAX_MONITORS 16

typedef struct {
    HMONITOR handle;
    RECT     rect;
    char     device[CCHDEVICENAME];
    int      is_primary;
} monitor_info_t;

static monitor_info_t s_monitors[FWD_MAX_MONITORS];
static int            s_monitor_count;

static HDC      s_hdc_screen;
static HDC      s_hdc_mem;
static HBITMAP  s_hbmp;
static int      s_width;
static int      s_height;
static int      s_offset_x;
static int      s_offset_y;
static uint8_t *s_buffer;

static BOOL CALLBACK monitor_enum_cb(HMONITOR hMonitor, HDC hdc,
                                      LPRECT lpRect, LPARAM lParam) {
    (void)hdc; (void)lpRect; (void)lParam;

    if (s_monitor_count >= FWD_MAX_MONITORS) return FALSE;

    MONITORINFOEXA mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    GetMonitorInfoA(hMonitor, (MONITORINFO *)&mi);

    monitor_info_t *m = &s_monitors[s_monitor_count];
    m->handle = hMonitor;
    m->rect = mi.rcMonitor;
    m->is_primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    memcpy(m->device, mi.szDevice, CCHDEVICENAME - 1);
    m->device[CCHDEVICENAME - 1] = '\0';

    s_monitor_count++;
    return TRUE;
}

/* Check if an adapter name looks like a virtual display driver */
static int is_virtual_adapter(const char *name, const char *key) {
    /* Work on uppercase copy */
    char upper[256];
    memset(upper, 0, sizeof(upper));
    strncpy(upper, name, sizeof(upper) - 1);
    _strupr(upper);

    char upper_key[512];
    memset(upper_key, 0, sizeof(upper_key));
    if (key) {
        strncpy(upper_key, key, sizeof(upper_key) - 1);
        _strupr(upper_key);
    }

    /* Known virtual display driver keywords */
    static const char *keywords[] = {
        "IDD", "INDIRECT", "VIRTUAL", "USBMMIDD",
        "VDD", "PARSEC", "SUNSHINE", "MOONLIGHT",
        "HEADLESS", "GHOST", "AMYUNI",
        NULL
    };

    for (int i = 0; keywords[i]; i++) {
        if (strstr(upper, keywords[i])) return 1;
        if (upper_key[0] && strstr(upper_key, keywords[i])) return 1;
    }
    return 0;
}

/* Detect virtual display by checking adapter info.
 * Returns monitor index or -1 if not found. */
int capture_find_virtual_display(void) {
    s_monitor_count = 0;
    EnumDisplayMonitors(NULL, NULL, monitor_enum_cb, 0);

    if (s_monitor_count <= 1) return -1;

    /* Enumerate all display adapters and match to our monitors */
    fprintf(stderr, "[fwdisplay] Scanning display adapters...\n");

    for (int i = 0; i < s_monitor_count; i++) {
        if (s_monitors[i].is_primary) continue;

        /* Get adapter info by enumerating with NULL (adapter level) */
        DISPLAY_DEVICEA adapter;
        memset(&adapter, 0, sizeof(adapter));
        adapter.cb = sizeof(adapter);

        /* Match adapter by device name (e.g. \\.\DISPLAY1 -> adapter index) */
        int adapter_idx = 0;
        int found_adapter = 0;
        while (EnumDisplayDevicesA(NULL, adapter_idx, &adapter, 0)) {
            if (strcmp(adapter.DeviceName, s_monitors[i].device) == 0) {
                found_adapter = 1;
                break;
            }
            adapter_idx++;
            memset(&adapter, 0, sizeof(adapter));
            adapter.cb = sizeof(adapter);
        }

        if (!found_adapter) continue;

        fprintf(stderr, "[fwdisplay]   [%d] %s: \"%s\" (flags=0x%lx)\n",
                i, adapter.DeviceName, adapter.DeviceString,
                (unsigned long)adapter.StateFlags);

        /* Check adapter name and registry key */
        if (is_virtual_adapter(adapter.DeviceString, adapter.DeviceKey)) {
            fprintf(stderr, "[fwdisplay] Detected virtual display: \"%s\" (monitor %d)\n",
                    adapter.DeviceString, i);
            return i;
        }

        /* Also check if adapter is NOT marked as physically connected
         * (virtual displays often lack DISPLAY_DEVICE_ATTACHED_TO_DESKTOP
         * or have unusual state flags) */
    }

    /* Fallback: if there's exactly one non-primary monitor, assume virtual */
    int non_primary_idx = -1;
    int non_primary_count = 0;
    for (int i = 0; i < s_monitor_count; i++) {
        if (!s_monitors[i].is_primary) {
            non_primary_idx = i;
            non_primary_count++;
        }
    }
    if (non_primary_count == 1) {
        fprintf(stderr, "[fwdisplay] Using non-primary monitor %d as virtual display\n",
                non_primary_idx);
        return non_primary_idx;
    }

    /* Multiple non-primary, can't determine which is virtual */
    fprintf(stderr, "[fwdisplay] Multiple monitors found, use --monitor N to select\n");
    return -1;
}

int capture_init(int display_index) {
    /* Enumerate monitors */
    s_monitor_count = 0;
    EnumDisplayMonitors(NULL, NULL, monitor_enum_cb, 0);

    fprintf(stderr, "[fwdisplay] Found %d monitor(s):\n", s_monitor_count);
    for (int i = 0; i < s_monitor_count; i++) {
        int w = s_monitors[i].rect.right - s_monitors[i].rect.left;
        int h = s_monitors[i].rect.bottom - s_monitors[i].rect.top;
        fprintf(stderr, "[fwdisplay]   [%d] %s %dx%d%s\n",
                i, s_monitors[i].device, w, h,
                s_monitors[i].is_primary ? " (primary)" : "");
    }

    if (display_index < 0 || display_index >= s_monitor_count) {
        fprintf(stderr, "[fwdisplay] ERROR: Monitor %d not found (have %d)\n",
                display_index, s_monitor_count);
        return -1;
    }

    RECT *r = &s_monitors[display_index].rect;
    s_offset_x = r->left;
    s_offset_y = r->top;
    s_width  = r->right - r->left;
    s_height = r->bottom - r->top;

    /* Get a DC for the whole virtual screen */
    s_hdc_screen = GetDC(NULL);
    s_hdc_mem = CreateCompatibleDC(s_hdc_screen);
    s_hbmp = CreateCompatibleBitmap(s_hdc_screen, s_width, s_height);
    SelectObject(s_hdc_mem, s_hbmp);

    s_buffer = (uint8_t *)malloc(s_width * s_height * 4);

    fprintf(stderr, "[fwdisplay] Capturing monitor %d: %dx%d at (%d,%d)\n",
            display_index, s_width, s_height, s_offset_x, s_offset_y);
    return 0;
}

int capture_grab(capture_frame_t *frame) {
    /* BitBlt from the virtual screen DC at the monitor's offset */
    BitBlt(s_hdc_mem, 0, 0, s_width, s_height,
           s_hdc_screen, s_offset_x, s_offset_y, SRCCOPY);

    BITMAPINFOHEADER bi;
    memset(&bi, 0, sizeof(bi));
    bi.biSize        = sizeof(bi);
    bi.biWidth       = s_width;
    bi.biHeight      = -s_height; /* top-down */
    bi.biPlanes      = 1;
    bi.biBitCount    = 32;
    bi.biCompression = BI_RGB;

    GetDIBits(s_hdc_mem, s_hbmp, 0, s_height,
              s_buffer, (BITMAPINFO *)&bi, DIB_RGB_COLORS);

    frame->pixels = s_buffer;
    frame->width  = s_width;
    frame->height = s_height;
    frame->stride = s_width * 4;
    return 0;
}

void capture_cleanup(void) {
    if (s_hbmp) DeleteObject(s_hbmp);
    if (s_hdc_mem) DeleteDC(s_hdc_mem);
    if (s_hdc_screen) ReleaseDC(NULL, s_hdc_screen);
    free(s_buffer);
    s_buffer = NULL;
    s_hbmp = NULL;
    s_hdc_mem = NULL;
    s_hdc_screen = NULL;
}

#endif /* _WIN32 */
