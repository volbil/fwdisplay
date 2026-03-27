/*
 * virtual_display.m -- create a macOS virtual display via private API
 */

#ifdef __APPLE__

#import "virtual_display.h"
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>

/* ---------- Private API declarations ---------- */

@interface CGVirtualDisplayMode : NSObject
- (instancetype)initWithWidth:(NSUInteger)width height:(NSUInteger)height refreshRate:(double)refreshRate;
@end

@interface CGVirtualDisplayDescriptor : NSObject
@property (nonatomic, retain, nullable) dispatch_queue_t queue;
@property (nonatomic, copy, nullable) NSString *name;
@property (nonatomic) NSUInteger maxPixelsWide;
@property (nonatomic) NSUInteger maxPixelsHigh;
@property (nonatomic) CGSize sizeInMillimeters;
@property (nonatomic) unsigned int vendorID;
@property (nonatomic) unsigned int productID;
@property (nonatomic) unsigned int serialNum;
@end

@interface CGVirtualDisplaySettings : NSObject
@property (nonatomic) int hiDPI;
@property (nonatomic, copy) NSArray<CGVirtualDisplayMode *> *modes;
@end

@interface CGVirtualDisplay : NSObject
- (nullable instancetype)initWithDescriptor:(CGVirtualDisplayDescriptor *)descriptor;
- (CGDirectDisplayID)displayID;
- (BOOL)applySettings:(CGVirtualDisplaySettings *)settings;
@end

/* ---------- Implementation ---------- */

static CGVirtualDisplay *s_vdisplay = nil;

CGDirectDisplayID vdisplay_create(unsigned int width, unsigned int height, int refresh_rate) {
    @autoreleasepool {
        CGVirtualDisplayDescriptor *desc = [[CGVirtualDisplayDescriptor alloc] init];
        desc.queue            = dispatch_get_main_queue();
        desc.name             = @"FWDisplay";
        desc.maxPixelsWide    = width;
        desc.maxPixelsHigh    = height;
        desc.sizeInMillimeters = CGSizeMake(197, 148); /* iPad 2 physical size */
        desc.vendorID         = 0xF0D1;
        desc.productID        = 0x0001;
        desc.serialNum        = 1;

        CGVirtualDisplay *vd = [[CGVirtualDisplay alloc] initWithDescriptor:desc];
        if (!vd) {
            fprintf(stderr, "[fwdisplay] ERROR: CGVirtualDisplay creation failed\n");
            return 0;
        }

        CGVirtualDisplayMode *mode =
            [[CGVirtualDisplayMode alloc] initWithWidth:width height:height refreshRate:refresh_rate];

        CGVirtualDisplaySettings *settings = [[CGVirtualDisplaySettings alloc] init];
        settings.hiDPI = 0;  /* non-retina, matches iPad 2 */
        settings.modes = @[mode];

        if (![vd applySettings:settings]) {
            fprintf(stderr, "[fwdisplay] ERROR: Failed to apply display settings\n");
            return 0;
        }

        CGDirectDisplayID did = [vd displayID];
        s_vdisplay = vd;  /* prevent ARC from releasing */

        return did;
    }
}

void vdisplay_destroy(void) {
    s_vdisplay = nil;
}

#endif /* __APPLE__ */
