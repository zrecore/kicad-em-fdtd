// The accessibility tree of the window: what a screen reader is told about each control and piece of text.
// The UI records a node per widget every frame it is wanted; a platform adapter (screen_reader.hpp) hands the
// tree to VoiceOver, Narrator or Orca. Nothing here depends on a platform or on ImGui, so it is tested alone.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace viz::access {

enum class Role {
  Group,
  Label,
  Status,
  Button,
  CheckBox,
  TextInput,
  ComboBox,
  Slider,
  Tab,
  ProgressBar,
  Image
};

// Controls the user can operate must have a name; text and images are read as they are.
bool interactive(Role role);
const char* roleName(Role role);

struct Node {
  std::uint64_t id = 0; // 0: derived from the role and name (stable across frames)
  Role role = Role::Label;
  std::string name;                 // what it is called ("Band start")
  std::string value;                // what it holds ("2.4 GHz", "on")
  std::string help;                 // the longer explanation
  std::string key;                  // the setting it edits, when a screen reader may set its value
  float x = 0, y = 0, w = 0, h = 0; // window points, top-left origin
  bool focused = false, selected = false, checkable = false, checked = false, live = false;
  bool operator==(const Node&) const = default;
};

constexpr std::uint64_t kRootId = 1;

class Tree {
public:
  void clear();
  // Adds a node; one without an id gets a stable hash of role and name (repeats get a counter).
  void add(Node node);
  const std::vector<Node>& nodes() const { return nodes_; }
  std::uint64_t focus() const { return focus_; }
  const Node* find(std::uint64_t id) const;
  Node& mutableBack() { return nodes_.back(); } // adjust the node just added (state known only after it)
  bool operator==(const Tree& o) const { return focus_ == o.focus_ && nodes_ == o.nodes_; }

  // Controls without a name, which a screen reader could only announce as "button".
  std::vector<std::string> problems() const;
  // One line per node, for tests and the --ui-a11y-dump option.
  std::string dump() const;

private:
  std::vector<Node> nodes_;
  std::uint64_t focus_ = kRootId;
};

// A request from the assistive technology.
struct Action {
  enum class Kind { Click, Focus, SetValue } kind = Kind::Click;
  std::uint64_t id = 0;
  std::string text; // SetValue
};

} // namespace viz::access
