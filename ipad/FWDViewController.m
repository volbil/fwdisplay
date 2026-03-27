#import "FWDViewController.h"
#import "FWDTCPServer.h"

// Touch phases matching the wire protocol
enum {
    FWDTouchPhaseBegan     = 0,
    FWDTouchPhaseMoved     = 1,
    FWDTouchPhaseEnded     = 2,
    FWDTouchPhaseCancelled = 3,
    FWDTouchPhaseRightClick = 4,
};

@interface FWDViewController ()
@property (nonatomic, strong) UIImageView *imageView;
@property (nonatomic, strong) UILabel *statusLabel;
@property (nonatomic, strong) UIActivityIndicatorView *spinner;
@property (nonatomic, strong) NSTimer *spinnerTimer;
@end

@implementation FWDViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor blackColor];
    self.view.multipleTouchEnabled = YES;

    // Full-screen image view
    self.imageView = [[UIImageView alloc] initWithFrame:self.view.bounds];
    self.imageView.contentMode = UIViewContentModeScaleToFill;
    self.imageView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.imageView.backgroundColor = [UIColor blackColor];
    self.imageView.userInteractionEnabled = NO;
    [self.view addSubview:self.imageView];

    // Waiting label
    self.statusLabel = [[UILabel alloc] init];
    self.statusLabel.text = @"Waiting for connection…";
    self.statusLabel.textColor = [UIColor colorWithWhite:0.5 alpha:1.0];
    self.statusLabel.font = [UIFont systemFontOfSize:18.0];
    self.statusLabel.textAlignment = NSTextAlignmentCenter;
    self.statusLabel.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.statusLabel.frame = self.view.bounds;
    [self.view addSubview:self.statusLabel];

    // Loading spinner (shown during frame drops)
    self.spinner = [[UIActivityIndicatorView alloc]
        initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleWhiteLarge];
    self.spinner.center = CGPointMake(self.view.bounds.size.width - 30,
                                      self.view.bounds.size.height - 30);
    self.spinner.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin |
                                    UIViewAutoresizingFlexibleTopMargin;
    self.spinner.hidesWhenStopped = YES;
    self.spinner.alpha = 0.6;
    [self.view addSubview:self.spinner];

    // Two-finger tap → right click
    UITapGestureRecognizer *twoFingerTap = [[UITapGestureRecognizer alloc]
        initWithTarget:self action:@selector(handleTwoFingerTap:)];
    twoFingerTap.numberOfTouchesRequired = 2;
    twoFingerTap.numberOfTapsRequired = 1;
    [self.view addGestureRecognizer:twoFingerTap];
}

- (void)displayImage:(UIImage *)image {
    if (self.statusLabel.superview) {
        [self.statusLabel removeFromSuperview];
    }
    self.imageView.image = image;
}

- (void)showWaitingStatus {
    self.imageView.image = nil;
    [self hideLoadingIndicator];
    if (!self.statusLabel.superview) {
        self.statusLabel.frame = self.view.bounds;
        [self.view addSubview:self.statusLabel];
    }
}

- (void)showLoadingIndicator {
    if (!self.spinner.isAnimating) {
        [self.spinner startAnimating];
        [self.view bringSubviewToFront:self.spinner];
    }
    // Auto-hide after 1 second of no further drops
    [self.spinnerTimer invalidate];
    self.spinnerTimer = [NSTimer scheduledTimerWithTimeInterval:1.0
                                                        target:self
                                                      selector:@selector(hideLoadingIndicator)
                                                      userInfo:nil
                                                       repeats:NO];
}

- (void)hideLoadingIndicator {
    [self.spinnerTimer invalidate];
    self.spinnerTimer = nil;
    [self.spinner stopAnimating];
}

#pragma mark - Two-finger tap (right click)

- (void)handleTwoFingerTap:(UITapGestureRecognizer *)recognizer {
    if (recognizer.state != UIGestureRecognizerStateEnded) return;

    // Use midpoint of the two fingers
    CGPoint loc = [recognizer locationInView:self.view];
    CGSize size = self.view.bounds.size;

    float nx = (float)(loc.x / size.width);
    float ny = (float)(loc.y / size.height);
    if (nx < 0.0f) nx = 0.0f;
    if (nx > 1.0f) nx = 1.0f;
    if (ny < 0.0f) ny = 0.0f;
    if (ny > 1.0f) ny = 1.0f;

    [self.server sendTouchWithPhase:FWDTouchPhaseRightClick x:nx y:ny];
}

#pragma mark - Single-finger touch handling

- (void)sendTouchForPhase:(uint8_t)phase touches:(NSSet *)touches {
    UITouch *touch = [touches anyObject];
    CGPoint loc = [touch locationInView:self.view];
    CGSize size = self.view.bounds.size;

    float nx = (float)(loc.x / size.width);
    float ny = (float)(loc.y / size.height);
    if (nx < 0.0f) nx = 0.0f;
    if (nx > 1.0f) nx = 1.0f;
    if (ny < 0.0f) ny = 0.0f;
    if (ny > 1.0f) ny = 1.0f;

    [self.server sendTouchWithPhase:phase x:nx y:ny];
}

- (void)touchesBegan:(NSSet *)touches withEvent:(UIEvent *)event {
    [super touchesBegan:touches withEvent:event];
    // Only send single-finger touches
    if ([[event allTouches] count] == 1) {
        [self sendTouchForPhase:FWDTouchPhaseBegan touches:touches];
    }
}

- (void)touchesMoved:(NSSet *)touches withEvent:(UIEvent *)event {
    [super touchesMoved:touches withEvent:event];
    if ([[event allTouches] count] == 1) {
        [self sendTouchForPhase:FWDTouchPhaseMoved touches:touches];
    }
}

- (void)touchesEnded:(NSSet *)touches withEvent:(UIEvent *)event {
    [super touchesEnded:touches withEvent:event];
    if ([[event allTouches] count] == 1) {
        [self sendTouchForPhase:FWDTouchPhaseEnded touches:touches];
    }
}

- (void)touchesCancelled:(NSSet *)touches withEvent:(UIEvent *)event {
    [super touchesCancelled:touches withEvent:event];
    [self sendTouchForPhase:FWDTouchPhaseCancelled touches:touches];
}

#pragma mark - Orientation

- (BOOL)prefersStatusBarHidden {
    return YES;
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
    return UIInterfaceOrientationMaskLandscape;
}

- (UIInterfaceOrientation)preferredInterfaceOrientationForPresentation {
    return UIInterfaceOrientationLandscapeLeft;
}

@end
