/*
 * usbmux.c — usbmuxd protocol client
 *
 * Protocol: 16-byte LE header + XML plist payload.
 * We hand-craft the XML since we only need "Listen" and "Connect".
 */

#include "usbmux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

/* ---------- wire format ---------- */

#pragma pack(push, 1)
typedef struct {
    uint32_t length;   /* total message length (header + payload) */
    uint32_t version;  /* 1 = plist */
    uint32_t type;     /* 8 = plist message */
    uint32_t tag;      /* request/response matching */
} mux_header_t;
#pragma pack(pop)

#define MUX_VERSION_PLIST 1
#define MUX_TYPE_PLIST    8
#define MUX_HEADER_SIZE   16
#define MUX_BUF_SIZE      65536

#ifdef _WIN32
#define USBMUXD_PORT 27015
#else
#define USBMUXD_SOCKET "/var/run/usbmuxd"
#endif

/* ---------- helpers ---------- */

static int send_all(mux_socket_t sock, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        int n = send(sock, p, (int)len, 0);
        if (n <= 0) return -1;
        p += n;
        len -= n;
    }
    return 0;
}

static int recv_all(mux_socket_t sock, void *buf, size_t len) {
    char *p = (char *)buf;
    while (len > 0) {
        int n = recv(sock, p, (int)len, 0);
        if (n <= 0) return -1;
        p += n;
        len -= n;
    }
    return 0;
}

static int mux_send_plist(mux_socket_t sock, uint32_t tag, const char *xml) {
    uint32_t xml_len = (uint32_t)strlen(xml);
    mux_header_t hdr;
    hdr.length  = MUX_HEADER_SIZE + xml_len;
    hdr.version = MUX_VERSION_PLIST;
    hdr.type    = MUX_TYPE_PLIST;
    hdr.tag     = tag;

    if (send_all(sock, &hdr, MUX_HEADER_SIZE) < 0) return -1;
    if (send_all(sock, xml, xml_len) < 0) return -1;
    return 0;
}

/* Receive a plist response. Caller must free *out_xml. Returns payload length or -1. */
static int mux_recv_plist(mux_socket_t sock, char **out_xml) {
    mux_header_t hdr;
    if (recv_all(sock, &hdr, MUX_HEADER_SIZE) < 0) return -1;

    uint32_t payload_len = hdr.length - MUX_HEADER_SIZE;
    if (payload_len == 0 || payload_len > MUX_BUF_SIZE) return -1;

    char *xml = (char *)malloc(payload_len + 1);
    if (!xml) return -1;
    if (recv_all(sock, xml, payload_len) < 0) { free(xml); return -1; }
    xml[payload_len] = '\0';

    *out_xml = xml;
    return (int)payload_len;
}

/* Tiny XML value extractor: find <key>KEY</key>\n<TYPE>VALUE</TYPE>
 * and copy VALUE into buf. Returns 0 on success. */
static int xml_get_value(const char *xml, const char *key, char *buf, size_t bufsz) {
    char needle[256];
    snprintf(needle, sizeof(needle), "<key>%s</key>", key);

    const char *p = strstr(xml, needle);
    if (!p) return -1;
    p += strlen(needle);

    /* skip whitespace */
    while (*p == '\n' || *p == '\r' || *p == '\t' || *p == ' ') p++;

    /* find opening tag end */
    const char *tag_start = strchr(p, '<');
    if (!tag_start) return -1;
    const char *tag_end = strchr(tag_start, '>');
    if (!tag_end) return -1;

    /* extract tag name for the closing tag */
    size_t tag_name_len = tag_end - tag_start - 1;
    char tag_name[64];
    if (tag_name_len >= sizeof(tag_name)) return -1;
    memcpy(tag_name, tag_start + 1, tag_name_len);
    tag_name[tag_name_len] = '\0';

    /* handle self-closing tags like <integer>123</integer> */
    const char *val_start = tag_end + 1;
    char closing[80];
    snprintf(closing, sizeof(closing), "</%s>", tag_name);
    const char *val_end = strstr(val_start, closing);
    if (!val_end) return -1;

    size_t val_len = val_end - val_start;
    if (val_len >= bufsz) return -1;
    memcpy(buf, val_start, val_len);
    buf[val_len] = '\0';
    return 0;
}

/* Check if a plist response contains <key>MessageType</key><string>VALUE</string> */
static int xml_has_message_type(const char *xml, const char *type) {
    char buf[128];
    if (xml_get_value(xml, "MessageType", buf, sizeof(buf)) < 0) return 0;
    return strcmp(buf, type) == 0;
}

static int xml_get_int(const char *xml, const char *key) {
    char buf[64];
    if (xml_get_value(xml, key, buf, sizeof(buf)) < 0) return -1;
    return atoi(buf);
}

/* ---------- public API ---------- */

mux_socket_t mux_connect_daemon(void) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    mux_socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) return MUX_INVALID_SOCKET;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(USBMUXD_PORT);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(sock);
        return MUX_INVALID_SOCKET;
    }
    return sock;
#else
    mux_socket_t sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return MUX_INVALID_SOCKET;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, USBMUXD_SOCKET, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return MUX_INVALID_SOCKET;
    }
    return sock;
#endif
}

int mux_wait_for_device(mux_socket_t sock) {
    /* Send Listen request */
    const char *listen_xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\"><dict>\n"
        "<key>ClientVersionString</key><string>fwdisplay</string>\n"
        "<key>MessageType</key><string>Listen</string>\n"
        "<key>ProgName</key><string>fwdisplay</string>\n"
        "</dict></plist>";

    if (mux_send_plist(sock, 1, listen_xml) < 0) return -1;

    /* Read Result (should be Number 0) */
    char *xml = NULL;
    if (mux_recv_plist(sock, &xml) < 0) return -1;

    int result = xml_get_int(xml, "Number");
    free(xml);
    if (result != 0) return -1;

    /* Wait for Attached notification */
    while (1) {
        xml = NULL;
        if (mux_recv_plist(sock, &xml) < 0) return -1;

        if (xml_has_message_type(xml, "Attached")) {
            int dev_id = xml_get_int(xml, "DeviceID");
            free(xml);
            if (dev_id >= 0) return dev_id;
        } else {
            free(xml);
        }
    }
}

mux_socket_t mux_connect_device(mux_socket_t sock, int device_id, uint16_t port) {
    (void)sock; /* unused — we open a fresh connection */

    /* usbmuxd expects the port in network byte order stored as an integer */
    uint16_t nport = htons(port);

    /* We need a fresh connection to usbmuxd for the Connect command,
     * because after Connect succeeds the socket becomes the TCP tunnel. */
    mux_socket_t csock = mux_connect_daemon();
    if (csock == MUX_INVALID_SOCKET) return MUX_INVALID_SOCKET;

    char xml[512];
    snprintf(xml, sizeof(xml),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\"><dict>\n"
        "<key>ClientVersionString</key><string>fwdisplay</string>\n"
        "<key>DeviceID</key><integer>%d</integer>\n"
        "<key>MessageType</key><string>Connect</string>\n"
        "<key>PortNumber</key><integer>%u</integer>\n"
        "<key>ProgName</key><string>fwdisplay</string>\n"
        "</dict></plist>",
        device_id, (unsigned)nport);

    if (mux_send_plist(csock, 2, xml) < 0) {
        mux_close(csock);
        return MUX_INVALID_SOCKET;
    }

    /* Read Result */
    char *resp = NULL;
    if (mux_recv_plist(csock, &resp) < 0) {
        mux_close(csock);
        return MUX_INVALID_SOCKET;
    }

    int result = xml_get_int(resp, "Number");
    free(resp);

    if (result != 0) {
        fprintf(stderr, "[fwdisplay] Connect refused (code %d)\n", result);
        mux_close(csock);
        return MUX_INVALID_SOCKET;
    }

    /* csock is now a transparent TCP tunnel to the device */
    return csock;
}

void mux_close(mux_socket_t sock) {
    if (sock == MUX_INVALID_SOCKET) return;
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}
