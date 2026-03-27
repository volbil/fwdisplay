#import <Foundation/Foundation.h>

// Called on a background queue with the raw JPEG bytes of each received frame.
typedef void (^FWDFrameHandler)(NSData *jpegData);

// Called on a background queue when the client disconnects.
typedef void (^FWDDisconnectHandler)(void);

// Called on a background queue with a touch event to send to the sender.
// phase: 0=began, 1=moved, 2=ended, 3=cancelled
// x, y: normalized coordinates 0.0–1.0
typedef void (^FWDTouchCallback)(uint8_t phase, float x, float y);

@interface FWDTCPServer : NSObject

@property (nonatomic, copy) FWDFrameHandler frameHandler;
@property (nonatomic, copy) FWDDisconnectHandler disconnectHandler;

- (instancetype)initWithPort:(uint16_t)port;
- (void)start;
- (void)stop;

// Send a touch event to the connected sender.
// Thread-safe — can be called from any queue.
- (void)sendTouchWithPhase:(uint8_t)phase x:(float)x y:(float)y;

@end
