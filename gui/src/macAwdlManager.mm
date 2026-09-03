// SPDX-License-Identifier: AGPL-3.0-only
#include "macAwdlManager.h"

#import <Security/Security.h>
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <IOKit/pwr_mgt/IOPMLib.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <string.h>

// Resolve a CAMetalLayer for a Qt NSView. Newer Qt/macOS combinations may
// expose a non-metal (or no) layer via -layer, which makes MoltenVK throw
// doesNotRecognizeSelector inside MVKSurface::getNaturalExtent. Install a
// fresh CAMetalLayer when needed and remember it: Qt may swap out the
// view's -layer behind our back, so later drawableSize syncs must target
// the layer we actually handed to VkMetalSurfaceEXT.
// Runs inside an autorelease pool because this may execute on a render
// thread without one.
static CAMetalLayer *g_surface_layer = nil;
static bool g_layer_is_ours = false; // true only for the addSublayer fallback

CAMetalLayer *mac_resolve_metal_layer(void *nsview)
{
    @autoreleasepool {
        if (!nsview)
            return nil;
        if (g_surface_layer)
            return g_surface_layer;
        NSView *view = (__bridge NSView *)nsview;
        [view setWantsLayer:YES];
        CALayer *layer = [view layer];
        if (!layer)
            return nil;

        // Qt 6.9+ wraps the real layer in a QContainerLayer:
        //   view.layer = QContainerLayer, its .contentLayer = CAMetalLayer.
        // That inner layer is Qt-managed: it stays in the layer tree and its
        // drawableSize/frame are synced by the container's layout. Use the
        // same resolution as QCocoaWindow::contentLayer().
        SEL cl_sel = sel_registerName("contentLayer");
        if ([layer respondsToSelector:cl_sel]) {
            CALayer *content = [layer performSelector:cl_sel];
            if ([content isKindOfClass:[CAMetalLayer class]]) {
                g_surface_layer = (CAMetalLayer *)content;
                g_layer_is_ours = false;
                NSLog(@"[chiaki] metal layer: QContainerLayer.contentLayer (%@, frame=%@, drawable=%@)",
                    content, NSStringFromRect(content.frame), NSStringFromSize(((CAMetalLayer *)content).drawableSize));
                return g_surface_layer;
            }
        }

        if ([layer isKindOfClass:[CAMetalLayer class]]) {
            g_surface_layer = (CAMetalLayer *)layer;
            g_layer_is_ours = false;
            NSLog(@"[chiaki] metal layer: view.layer directly (%@, superlayer=%@)",
                layer, [layer superlayer]);
            return g_surface_layer;
        }

        // Fallback: attach our own metal layer as a sublayer.
        CAMetalLayer *metal = [[CAMetalLayer alloc] init];
        [layer addSublayer:metal];
        g_surface_layer = metal;
        g_layer_is_ours = true;
        NSLog(@"[chiaki] metal layer: FALLBACK own sublayer under %@ (view layer class: %s)",
            layer, class_getName(object_getClass(layer)));
        return metal;
    }
}

static bool awdl_interface_is_up()
{
    FILE *p = popen("/sbin/ifconfig awdl0 2>/dev/null", "r");
    if (!p)
        return false;
    char buf[512];
    bool up = false;
    while (fgets(buf, sizeof(buf), p))
    {
        if (strstr(buf, "flags="))
        {
            up = strstr(buf, "<UP") != nullptr || strstr(buf, ",UP,") != nullptr;
            break;
        }
    }
    pclose(p);
    return up;
}

static bool run_privileged_ifconfig(const char *arg)
{
    AuthorizationRef auth = nullptr;
    OSStatus status = AuthorizationCreate(nullptr,
                                          kAuthorizationEmptyEnvironment,
                                          kAuthorizationFlagDefaults,
                                          &auth);
    if (status != errAuthorizationSuccess)
        return false;

    char *args[] = { const_cast<char *>("awdl0"), const_cast<char *>(arg), nullptr };
    status = AuthorizationExecuteWithPrivileges(auth,
                                                "/sbin/ifconfig",
                                                kAuthorizationFlagInteractionAllowed |
                                                    kAuthorizationFlagExtendRights,
                                                args,
                                                nullptr);
    AuthorizationFree(auth, kAuthorizationFlagDefaults);
    return status == errAuthorizationSuccess;
}

void mac_awdl_disable_on_start()
{
    if (awdl_interface_is_up())
        run_privileged_ifconfig("down");
}

void mac_awdl_restore_on_exit()
{
    if (!awdl_interface_is_up())
        run_privileged_ifconfig("up");
}

// Self-healing: re-attach the surface layer if Qt swapped out the view's
// backing layer (happens on unexpose/re-expose cycles) leaving our layer
// orphaned — presents would then go nowhere (white window).
void mac_ensure_surface_layer_attached(void *nsview)
{
    @autoreleasepool {
        if (!nsview || !g_layer_is_ours)
            return; // Qt-managed layer: nothing to do
        NSView *view = (__bridge NSView *)nsview;
        CAMetalLayer *layer = g_surface_layer;
        if (!layer)
            return;
        [view setWantsLayer:YES];
        CALayer *host = [view layer];
        if (!host)
            return;
        if (layer.superlayer != host)
        {
            if (layer.superlayer)
                [layer removeFromSuperlayer];
            [host addSublayer:layer];
        }
    }
}

// Keep the Vulkan surface layer in sync with the window size.
// When we resolved Qt's own CAMetalLayer (QContainerLayer path) the frame
// is managed by Qt; only the backing store size needs an update.
void mac_sync_layer_drawable_size(void *nsview, int w, int h)
{
    @autoreleasepool {
        if (w <= 0 || h <= 0)
            return;
        CAMetalLayer *layer = g_surface_layer;
        if (!layer)
            return;
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        if (g_layer_is_ours)
        {
            // our fallback sublayer: position + scale it ourselves
            CGFloat scale = 1.0;
            if (NSView *view = (__bridge NSView *)nsview)
                if (NSWindow *win = [view window])
                    scale = win.backingScaleFactor;
            [layer setFrame:CGRectMake(0, 0, w / scale, h / scale)];
            [layer setContentsScale:scale];
        }
        [layer setDrawableSize:CGSizeMake(w, h)];
        [CATransaction commit];
        NSLog(@"[chiaki] sync drawable %dx%d (ours=%d frame=%@ scale=%.1f)", w, h,
            (int)g_layer_is_ours, NSStringFromRect(layer.frame), layer.contentsScale);
    }
}

void *mac_keep_awake_begin()
{
    // prevent display sleep + system idle sleep while streaming
    IOPMAssertionID display_assertion = 0;
    IOPMAssertionID system_assertion = 0;
    IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep,
                                kIOPMAssertionLevelOn,
                                CFSTR("chiaki-ng display keep awake"),
                                &display_assertion);
    IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleSystemSleep,
                                kIOPMAssertionLevelOn,
                                CFSTR("chiaki-ng system keep awake"),
                                &system_assertion);
    // encode both ids in the handle (they are small integers)
    return reinterpret_cast<void *>(static_cast<uintptr_t>((display_assertion << 16) | (system_assertion & 0xffff)));
}

void mac_keep_awake_end(void *handle)
{
    uintptr_t v = reinterpret_cast<uintptr_t>(handle);
    IOPMAssertionID display_assertion = static_cast<IOPMAssertionID>(v >> 16);
    IOPMAssertionID system_assertion = static_cast<IOPMAssertionID>(v & 0xffff);
    if (display_assertion && display_assertion != static_cast<IOPMAssertionID>(-1))
        IOPMAssertionRelease(display_assertion);
    if (system_assertion && system_assertion != static_cast<IOPMAssertionID>(-1))
        IOPMAssertionRelease(system_assertion);
}
