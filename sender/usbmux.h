/*
 * usbmux.h — usbmuxd protocol client (macOS/Linux/Windows)
 *
 * Talks to the system usbmuxd daemon to discover iOS devices
 * and open TCP tunnels over USB.
 *
 * macOS/Linux: Unix socket /var/run/usbmuxd
 * Windows:     TCP localhost:27015
 */

#ifndef USBMUX_H
#define USBMUX_H

#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET mux_socket_t;
#define MUX_INVALID_SOCKET INVALID_SOCKET
#else
typedef int mux_socket_t;
#define MUX_INVALID_SOCKET (-1)
#endif

/* Connect to the usbmuxd daemon. Returns socket or MUX_INVALID_SOCKET. */
mux_socket_t mux_connect_daemon(void);

/* Wait for an iOS device to appear on USB. Returns device ID or -1. */
int mux_wait_for_device(mux_socket_t sock);

/* Open a TCP tunnel to `port` on device `device_id`.
 * Returns a new socket connected to the device, or MUX_INVALID_SOCKET.
 * The original `sock` is consumed (usbmuxd repurposes it as the tunnel). */
mux_socket_t mux_connect_device(mux_socket_t sock, int device_id, uint16_t port);

/* Close a mux socket. */
void mux_close(mux_socket_t sock);

#endif /* USBMUX_H */
