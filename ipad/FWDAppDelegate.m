#import "FWDAppDelegate.h"
#import "FWDViewController.h"
#import "FWDTCPServer.h"

@interface FWDAppDelegate ()
// Frame dropping: only keep the latest frame
@property (nonatomic, strong) UIImage *pendingImage;
@property (nonatomic, assign) BOOL displayScheduled;
@end

@implementation FWDAppDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    [UIApplication sharedApplication].idleTimerDisabled = YES;

    self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
    self.window.backgroundColor = [UIColor blackColor];

    self.viewController = [[FWDViewController alloc] init];
    self.window.rootViewController = self.viewController;
    [self.window makeKeyAndVisible];

    // Start TCP server
    self.server = [[FWDTCPServer alloc] initWithPort:8765];
    self.viewController.server = self.server;

    __weak FWDAppDelegate *weakSelf = self;
    __weak FWDViewController *vc = self.viewController;

    self.server.frameHandler = ^(NSData *jpegData) {
        // Decode on the background read queue to avoid main queue work
        UIImage *image = [UIImage imageWithData:jpegData];
        if (!image) return;

        FWDAppDelegate *strongSelf = weakSelf;
        if (!strongSelf) return;

        // Store latest frame — overwrites any undelivered previous frame
        @synchronized(strongSelf) {
            strongSelf.pendingImage = image;
            if (strongSelf.displayScheduled) {
                return;
            }
            strongSelf.displayScheduled = YES;
        }

        // Schedule display on main queue — only one block in flight at a time
        dispatch_async(dispatch_get_main_queue(), ^{
            FWDAppDelegate *s = weakSelf;
            if (!s) return;

            UIImage *img;
            @synchronized(s) {
                img = s.pendingImage;
                s.pendingImage = nil;
                s.displayScheduled = NO;
            }

            if (img) {
                [vc displayImage:img];
                [vc hideLoadingIndicator];
            }
        });
    };

    self.server.disconnectHandler = ^{
        dispatch_async(dispatch_get_main_queue(), ^{
            FWDAppDelegate *s = weakSelf;
            if (!s) return;
            @synchronized(s) {
                s.pendingImage = nil;
            }
            [vc showWaitingStatus];
        });
    };

    [self.server start];

    NSLog(@"[FWDisplay] App launched, TCP server started on port 8765");
    return YES;
}

- (void)applicationDidBecomeActive:(UIApplication *)application {
    [UIApplication sharedApplication].idleTimerDisabled = YES;
}

- (void)applicationDidReceiveMemoryWarning:(UIApplication *)application {
    NSLog(@"[FWDisplay] Memory warning!");
    @synchronized(self) {
        self.pendingImage = nil;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        [self.viewController showLoadingIndicator];
    });
}

@end
