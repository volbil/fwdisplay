#import "FWDTCPServer.h"
#import <sys/socket.h>
#import <netinet/in.h>
#import <unistd.h>
#import <signal.h>

// Wire protocol:
//
// Sender → iPad (frames):
//   [ 4 bytes big-endian uint32: JPEG length ][ N bytes: JPEG data ]
//
// iPad → Sender (touch events):
//   [ 1 byte: msg type 0x01 ][ 1 byte: phase ][ 4 bytes: float BE x ][ 4 bytes: float BE y ]
//
// Both directions use the same full-duplex TCP connection.

static const size_t kHeaderSize = 4;
static const uint32_t kMaxFrameSize = 8 * 1024 * 1024;
static const uint8_t kMsgTypeTouch = 0x01;

@interface FWDTCPServer ()
@property (nonatomic, assign) uint16_t port;
@property (nonatomic, assign) int listenFd;
@property (nonatomic, assign) int clientFd;       // current connected client (-1 if none)
@property (nonatomic, assign) BOOL running;
@property (nonatomic, strong) dispatch_queue_t acceptQueue;
@property (nonatomic, strong) dispatch_queue_t readQueue;
@property (nonatomic, strong) dispatch_queue_t writeQueue;
// Reusable receive buffer to avoid repeated allocation
@property (nonatomic, assign) uint8_t *recvBuffer;
@property (nonatomic, assign) uint32_t recvBufferSize;
@end

@implementation FWDTCPServer

- (instancetype)initWithPort:(uint16_t)port {
    self = [super init];
    if (self) {
        _port = port;
        _listenFd = -1;
        _clientFd = -1;
        _acceptQueue = dispatch_queue_create("com.fwdisplay.accept", DISPATCH_QUEUE_SERIAL);
        _readQueue   = dispatch_queue_create("com.fwdisplay.read",   DISPATCH_QUEUE_SERIAL);
        _writeQueue  = dispatch_queue_create("com.fwdisplay.write",  DISPATCH_QUEUE_SERIAL);
        // Pre-allocate 256 KB receive buffer (grows if needed)
        _recvBufferSize = 256 * 1024;
        _recvBuffer = (uint8_t *)malloc(_recvBufferSize);

        // Ignore SIGPIPE — we handle send() errors via return codes
        signal(SIGPIPE, SIG_IGN);
    }
    return self;
}

- (void)dealloc {
    free(_recvBuffer);
}

- (void)start {
    self.running = YES;
    dispatch_async(self.acceptQueue, ^{
        [self runAcceptLoop];
    });
}

- (void)stop {
    self.running = NO;
    if (self.listenFd >= 0) {
        close(self.listenFd);
        self.listenFd = -1;
    }
}

#pragma mark - Accept loop

- (void)runAcceptLoop {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        NSLog(@"[FWDisplay] socket() failed: %s", strerror(errno));
        return;
    }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(self.port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        NSLog(@"[FWDisplay] bind() failed: %s", strerror(errno));
        close(fd);
        return;
    }

    if (listen(fd, 1) < 0) {
        NSLog(@"[FWDisplay] listen() failed: %s", strerror(errno));
        close(fd);
        return;
    }

    self.listenFd = fd;
    NSLog(@"[FWDisplay] Listening on port %d", self.port);

    while (self.running) {
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);
        int cfd = accept(fd, (struct sockaddr *)&clientAddr, &clientLen);
        if (cfd < 0) {
            if (self.running) {
                NSLog(@"[FWDisplay] accept() failed: %s", strerror(errno));
            }
            break;
        }

        NSLog(@"[FWDisplay] Client connected (fd=%d)", cfd);
        self.clientFd = cfd;

        // Read loop runs synchronously on readQueue; blocks until disconnect
        dispatch_sync(self.readQueue, ^{
            [self runReadLoopOnSocket:cfd];
        });

        self.clientFd = -1;
        close(cfd);
        NSLog(@"[FWDisplay] Client disconnected, waiting for next connection");

        // Notify disconnect
        FWDDisconnectHandler dh = self.disconnectHandler;
        if (dh) {
            dh();
        }
    }

    close(fd);
    self.listenFd = -1;
}

#pragma mark - Read loop

- (void)runReadLoopOnSocket:(int)fd {
    uint8_t header[kHeaderSize];

    while (self.running) {
        @autoreleasepool {
            // 1. Read 4-byte big-endian length header
            if (![self readExact:fd buffer:header length:kHeaderSize]) {
                break;
            }

            uint32_t frameLen = ((uint32_t)header[0] << 24) |
                                ((uint32_t)header[1] << 16) |
                                ((uint32_t)header[2] <<  8) |
                                ((uint32_t)header[3]);

            if (frameLen == 0 || frameLen > kMaxFrameSize) {
                NSLog(@"[FWDisplay] Invalid frame length %u, dropping connection", frameLen);
                break;
            }

            // Grow reusable buffer if needed
            if (frameLen > self.recvBufferSize) {
                free(self.recvBuffer);
                self.recvBufferSize = frameLen;
                self.recvBuffer = (uint8_t *)malloc(frameLen);
                if (!self.recvBuffer) {
                    NSLog(@"[FWDisplay] malloc failed for %u bytes", frameLen);
                    break;
                }
            }

            // 2. Read frame body into reusable buffer
            if (![self readExact:fd buffer:self.recvBuffer length:frameLen]) {
                break;
            }

            // 3. Deliver to handler — wrap in NSData (no copy, just pointer)
            NSData *frame = [NSData dataWithBytesNoCopy:self.recvBuffer
                                                 length:frameLen
                                           freeWhenDone:NO];
            FWDFrameHandler handler = self.frameHandler;
            if (handler) {
                handler(frame);
            }
        }
    }
}

#pragma mark - Touch event sending

- (void)sendTouchWithPhase:(uint8_t)phase x:(float)x y:(float)y {
    dispatch_async(self.writeQueue, ^{
        int fd = self.clientFd;
        if (fd < 0) return;

        // Pack: [type 1B][phase 1B][x 4B float BE][y 4B float BE] = 10 bytes
        uint8_t buf[10];
        buf[0] = kMsgTypeTouch;
        buf[1] = phase;

        // Float to big-endian bytes
        uint32_t fx, fy;
        memcpy(&fx, &x, 4);
        memcpy(&fy, &y, 4);
        fx = htonl(fx);
        fy = htonl(fy);
        memcpy(&buf[2], &fx, 4);
        memcpy(&buf[6], &fy, 4);

        size_t remaining = 10;
        uint8_t *ptr = buf;
        while (remaining > 0) {
            ssize_t n = send(fd, ptr, remaining, 0);
            if (n <= 0) break;
            ptr += n;
            remaining -= n;
        }
    });
}

#pragma mark - Helpers

- (BOOL)readExact:(int)fd buffer:(void *)buf length:(size_t)length {
    size_t remaining = length;
    uint8_t *ptr = (uint8_t *)buf;

    while (remaining > 0) {
        ssize_t n = recv(fd, ptr, remaining, 0);
        if (n <= 0) {
            if (n < 0) {
                NSLog(@"[FWDisplay] recv() error: %s", strerror(errno));
            }
            return NO;
        }
        ptr += n;
        remaining -= n;
    }
    return YES;
}

@end
