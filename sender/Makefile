TARGET = fwdisplay-sender

# Source files (common)
COMMON_SRC = sender.c usbmux.c

# Detect platform
UNAME := $(shell uname -s)

ifeq ($(UNAME),Darwin)
    PLATFORM_SRC = touch.c
    OBJC_SRC     = capture_mac.m virtual_display.m cursor_mac.m
    CFLAGS  = -O2 -Wall -Wextra
    OBJCFLAGS = -O2 -Wall -fobjc-arc
    LDFLAGS = -framework CoreGraphics -framework ApplicationServices -framework Foundation -framework AppKit -framework ScreenCaptureKit -framework CoreMedia -framework CoreVideo
    JPEG_CFLAGS  := $(shell pkg-config --cflags libturbojpeg 2>/dev/null)
    JPEG_LDFLAGS := $(shell pkg-config --libs libturbojpeg 2>/dev/null)
    # Fallback if pkg-config fails (Homebrew default)
    ifeq ($(JPEG_LDFLAGS),)
        JPEG_CFLAGS  = -I/opt/homebrew/include -I/usr/local/include
        JPEG_LDFLAGS = -L/opt/homebrew/lib -L/usr/local/lib -lturbojpeg
    endif
else ifeq ($(UNAME),Linux)
    PLATFORM_SRC = capture_linux.c
    OBJC_SRC     =
    CFLAGS  = -O2 -Wall -Wextra
    LDFLAGS = -lX11 -lXext
    JPEG_CFLAGS  := $(shell pkg-config --cflags libturbojpeg 2>/dev/null)
    JPEG_LDFLAGS := $(shell pkg-config --libs libturbojpeg 2>/dev/null)
    ifeq ($(JPEG_LDFLAGS),)
        JPEG_LDFLAGS = -lturbojpeg
    endif
else
    # Windows (MinGW)
    PLATFORM_SRC = capture_win.c
    OBJC_SRC     =
    CFLAGS  = -O2 -Wall -Wextra
    LDFLAGS = -lws2_32 -lgdi32
    JPEG_CFLAGS  =
    JPEG_LDFLAGS = -lturbojpeg
    TARGET = fwdisplay-sender.exe
endif

C_SRCS = $(COMMON_SRC) $(PLATFORM_SRC)
C_OBJS = $(C_SRCS:.c=.o)
M_OBJS = $(OBJC_SRC:.m=.o)
ALL_OBJS = $(C_OBJS) $(M_OBJS)

CC ?= cc

all: $(TARGET)

$(TARGET): $(ALL_OBJS)
	$(CC) $(ALL_OBJS) -o $@ $(LDFLAGS) $(JPEG_LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) $(JPEG_CFLAGS) -c $< -o $@

%.o: %.m
	$(CC) $(OBJCFLAGS) -c $< -o $@

clean:
	rm -f $(ALL_OBJS) $(TARGET)

.PHONY: all clean
