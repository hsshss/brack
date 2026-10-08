// macOS: NSOpenPanel and NSSavePanel as sheets on Brack's window. A sheet leaves the application's
// loop running, so the GUI goes on drawing without a thread of its own. Plugins are bundles there
// (folders) as well as files: the open panel lets one be chosen, and goes into other folders.
#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <objc/runtime.h>

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <memory>
#include <string>
#include <vector>

#include "file_dialog.h"

@interface BrackOpenFilter : NSObject <NSOpenSavePanelDelegate>
@property(nonatomic, strong) NSArray<NSString*>* extensions;
@end

@implementation BrackOpenFilter
- (BOOL)matches:(NSURL*)url {
    return [self.extensions containsObject:url.pathExtension.lowercaseString];
}
- (BOOL)panel:(id)sender shouldEnableURL:(NSURL*)url {
    NSNumber* folder = nil;
    [url getResourceValue:&folder forKey:NSURLIsDirectoryKey error:nil];
    return folder.boolValue || [self matches:url];
}
- (BOOL)panel:(id)sender validateURL:(NSURL*)url error:(NSError**)error {
    return [self matches:url];
}
@end

namespace brack {

namespace {

NSWindow* g_owner = nil;

NSArray<NSString*>* extensions(const std::string& pattern) {
    NSMutableArray<NSString*>* out = [NSMutableArray array];
    for (NSString* glob in [@(pattern.c_str()) componentsSeparatedByString:@";"])
        if ([glob hasPrefix:@"*."]) [out addObject:[glob substringFromIndex:2].lowercaseString];
    return out;
}

std::future<std::string> run(NSSavePanel* panel) {
    auto result = std::make_shared<std::promise<std::string>>();
    auto future = result->get_future();
    auto done = ^(NSModalResponse response) {
        result->set_value(response == NSModalResponseOK && panel.URL ? std::string(panel.URL.fileSystemRepresentation)
                                                                      : std::string());
    };
    if (g_owner) [panel beginSheetModalForWindow:g_owner completionHandler:done];
    else [panel beginWithCompletionHandler:done];
    return future;
}

}  // namespace

void setFileDialogOwner(GLFWwindow* owner) { g_owner = owner ? glfwGetCocoaWindow(owner) : nil; }

std::future<std::string> openFileDialog(std::string filterName, std::string pattern) {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    BrackOpenFilter* filter = [BrackOpenFilter new];
    filter.extensions = extensions(pattern);
    panel.delegate = filter;
    panel.message = @(filterName.c_str());
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = YES;  // bundles
    panel.allowsMultipleSelection = NO;
    objc_setAssociatedObject(panel, @selector(delegate), filter, OBJC_ASSOCIATION_RETAIN);  // the delegate is weak
    return run(panel);
}

std::future<std::string> saveFileDialog(std::string filterName, std::string pattern, std::string defaultExt) {
    NSSavePanel* panel = [NSSavePanel savePanel];
    panel.message = @(filterName.c_str());
    if (UTType* type = [UTType typeWithFilenameExtension:@(defaultExt.c_str())]) panel.allowedContentTypes = @[ type ];
    return run(panel);
}

}  // namespace brack
