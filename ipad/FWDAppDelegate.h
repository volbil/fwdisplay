#import <UIKit/UIKit.h>

@class FWDViewController;
@class FWDTCPServer;

@interface FWDAppDelegate : UIResponder <UIApplicationDelegate>

@property (nonatomic, strong) UIWindow *window;
@property (nonatomic, strong) FWDViewController *viewController;
@property (nonatomic, strong) FWDTCPServer *server;

@end
