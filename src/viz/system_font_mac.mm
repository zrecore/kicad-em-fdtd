// The macOS system UI font (San Francisco), found through CoreText. Its file is a variable font; the font atlas
// reads the default instance, which is the regular weight. If that file cannot be found, a static system font.
#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>

#include <climits>

#include "viz/system_font.hpp"

namespace viz {

std::optional<SystemFont> systemUiFont() {
  @autoreleasepool {
    CTFontRef font = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 13.0, nullptr);
    if (font) {
      CFURLRef url = static_cast<CFURLRef>(CTFontCopyAttribute(font, kCTFontURLAttribute));
      CFStringRef display = CTFontCopyDisplayName(font);
      SystemFont out;
      if (url) {
        char buf[PATH_MAX];
        if (CFURLGetFileSystemRepresentation(url, true, reinterpret_cast<UInt8*>(buf), sizeof buf))
          out.path = buf;
        CFRelease(url);
      }
      if (display) {
        char name[256];
        if (CFStringGetCString(display, name, sizeof name, kCFStringEncodingUTF8))
          out.name = name;
        CFRelease(display);
      }
      CFRelease(font);
      if (!out.path.empty() && !firstExistingFile({out.path.c_str()}).empty())
        return out;
    }
  }
  // Static fonts that ship with macOS, in the order of preference.
  const std::string path = firstExistingFile({"/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/Helvetica.ttc",
                                              "/System/Library/Fonts/HelveticaNeue.ttc",
                                              "/Library/Fonts/Arial.ttf", "/System/Library/Fonts/Supplemental/Arial.ttf"});
  if (path.empty())
    return std::nullopt;
  return SystemFont{path, 0, "macOS system font"};
}

} // namespace viz
