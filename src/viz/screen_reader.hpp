// Hands the accessibility tree (access.hpp) to the platform's screen reader through AccessKit: VoiceOver on
// macOS (wired up), Windows UI Automation (Narrator, NVDA) and AT-SPI (Orca) behind the same calls. The
// adapter only asks for the tree once an assistive technology attaches, so it costs nothing otherwise.
#pragma once
#include <memory>
#include <vector>

#include "viz/access.hpp"

struct GLFWwindow;

namespace viz {

class ScreenReader {
public:
  // Null where the platform has no adapter yet or screen-reader support is compiled out.
  static std::unique_ptr<ScreenReader> create(GLFWwindow* window);
  ~ScreenReader();
  ScreenReader(const ScreenReader&) = delete;
  ScreenReader& operator=(const ScreenReader&) = delete;

  bool active() const; // an assistive technology has attached (callable from any thread)
  // Sends the tree if it changed since the last call (or the window focus did).
  void update(const access::Tree& tree, bool windowFocused);
  // Requests made by the assistive technology since the last call.
  std::vector<access::Action> takeActions();

  struct Impl; // opaque; public so the adapter callbacks can name it

private:
  ScreenReader();
  std::unique_ptr<Impl> impl_;
};

} // namespace viz
