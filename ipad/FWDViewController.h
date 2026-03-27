#import <UIKit/UIKit.h>

@class FWDTCPServer;

@interface FWDViewController : UIViewController

// Weak reference to the server for sending touch events
@property (nonatomic, weak) FWDTCPServer *server;

- (void)displayImage:(UIImage *)image;
- (void)showWaitingStatus;
- (void)showLoadingIndicator;
- (void)hideLoadingIndicator;

@end
