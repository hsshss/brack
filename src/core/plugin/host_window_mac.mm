// macOS: an editor goes into an NSView of ours, the content of a window of its own or a subview of
// the application's NSView (Placement::parent). Sizes are in points, as the plugins' are there:
// the scale stays 1, and the backing scale is the OS's business. Main thread only (host_thread_mac.cpp).
#import <AppKit/AppKit.h>

#include "plugin/host_window.h"

namespace brack {
class CocoaHostWindow;
}

// Flipped, so that the plugin's view sits at the top left, as on the other systems.
@interface BrackEditorView : NSView
@end

@implementation BrackEditorView
- (BOOL)isFlipped {
    return YES;
}
@end

@interface BrackEditorDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) brack::CocoaHostWindow* owner;
@end

namespace brack {

class CocoaHostWindow final : public HostWindow {
public:
    CocoaHostWindow(Callbacks cb, Placement placement) : cb_(std::move(cb)), placement_(std::move(placement)) {}

    ~CocoaHostWindow() override {
        delegate_.owner = nullptr;
        if (window_) {
            window_.delegate = nil;
            [window_ orderOut:nil];
            [window_ close];
        } else {
            [view_ removeFromSuperview];
        }
    }

    bool open(const std::string& title, bool resizable, std::string& error) {
        resizable_ = resizable;
        view_ = [[BrackEditorView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
        if (NSView* parent = (__bridge NSView*)placement_.parent) {
            if (![parent isKindOfClass:[NSView class]]) {
                error = "the parent is not an NSView";
                return false;
            }
            view_.hidden = YES;
            [parent addSubview:view_];
            placeInParent();
            return true;
        }
        window_ = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 400, 300)
                                              styleMask:styleMask()
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
        if (!window_) {
            error = "could not create editor window";
            return false;
        }
        window_.releasedWhenClosed = NO;
        window_.title = [NSString stringWithUTF8String:title.c_str()];
        window_.contentView = view_;
        delegate_ = [BrackEditorDelegate new];
        delegate_.owner = this;
        window_.delegate = delegate_;
        [window_ center];
        return true;
    }

    void* nativeHandle() const override { return (__bridge void*)view_; }
    double scale() const override { return 1.0; }

    void setClientSize(uint32_t w, uint32_t h) override {
        inHostResize_ = true;
        if (window_) {
            // Keeps the top left where it is.
            NSRect frame = [window_ frameRectForContentRect:NSMakeRect(0, 0, w, h)];
            const NSRect old = window_.frame;
            frame.origin = NSMakePoint(old.origin.x, NSMaxY(old) - frame.size.height);
            [window_ setFrame:frame display:YES];
        } else {
            [view_ setFrameSize:NSMakeSize(w, h)];
            placeInParent();
        }
        inHostResize_ = false;
        reportSize();
    }

    void setResizable(bool resizable) override {
        resizable_ = resizable;
        if (window_) window_.styleMask = styleMask();
    }

    void show() override {
        if (!window_) {
            view_.hidden = NO;
            return;
        }
        [window_ makeKeyAndOrderFront:nil];
        [NSApp activate];
    }
    void hide() override {
        if (window_) [window_ orderOut:nil];
        else view_.hidden = YES;
    }
    void abandon() override {
        cb_ = {};
        hide();
    }
    void setTitle(const std::string& title) override {
        if (window_) window_.title = [NSString stringWithUTF8String:title.c_str()];
    }

    void closeRequested() {
        if (cb_.closeRequested) cb_.closeRequested();
    }

    void onUserResize() {
        if (inHostResize_) return;
        if (!resizable_ || !cb_.resized) {
            reportSize();
            return;
        }
        const NSSize size = view_.frame.size;
        const uint32_t w = (uint32_t)size.width, h = (uint32_t)size.height;
        if (w == 0 || h == 0) return;
        uint32_t aw = w, ah = h;
        cb_.resized(aw, ah);
        if (aw != w || ah != h) setClientSize(aw, ah);
        else reportSize();
    }

private:
    NSWindowStyleMask styleMask() const {
        NSWindowStyleMask mask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
        if (resizable_) mask |= NSWindowStyleMaskResizable;
        return mask;
    }

    void placeInParent() {
        NSView* parent = view_.superview;
        const CGFloat y = parent.isFlipped ? 0 : NSHeight(parent.bounds) - NSHeight(view_.frame);
        [view_ setFrameOrigin:NSMakePoint(0, y)];
    }

    void reportSize() {
        if (!placement_.sized) return;
        const NSSize size = view_.frame.size;
        placement_.sized((uint32_t)size.width, (uint32_t)size.height);
    }

    Callbacks cb_;
    Placement placement_;
    NSWindow* window_ = nil;
    BrackEditorView* view_ = nil;
    BrackEditorDelegate* delegate_ = nil;
    bool resizable_ = false;
    bool inHostResize_ = false;
};

}  // namespace brack

@implementation BrackEditorDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    // Not closed here: the owner tears the editor down, and not from in here.
    if (self.owner) self.owner->closeRequested();
    return NO;
}
- (void)windowDidResize:(NSNotification*)notification {
    if (self.owner) self.owner->onUserResize();
}
@end

namespace brack {

HostWindow::Api HostWindow::api() { return Api::Cocoa; }

bool HostWindow::available(std::string&) { return true; }

std::unique_ptr<HostWindow> HostWindow::create(const std::string& title, bool resizable, Callbacks cb,
                                               const Placement& placement, std::string& error) {
    auto w = std::make_unique<CocoaHostWindow>(std::move(cb), placement);
    if (!w->open(title, resizable, error)) return nullptr;
    return w;
}

ParentDpiScope::ParentDpiScope(void*) {}
ParentDpiScope::~ParentDpiScope() = default;

}  // namespace brack
