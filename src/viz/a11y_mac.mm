// macOS pieces of the accessibility support that need Objective-C.
#import <AppKit/AppKit.h>

namespace viz {

// The Body text style's preferred size against the 13 pt macOS default.
double macTextScale() {
  @autoreleasepool {
    NSFont* body = [NSFont preferredFontForTextStyle:NSFontTextStyleBody options:@{}];
    return body ? body.pointSize / 13.0 : 1.0;
  }
}

} // namespace viz
