#include "viz/access.hpp"

#include <cstdio>
#include <map>

namespace viz::access {

bool interactive(Role role) {
  switch (role) {
  case Role::Button:
  case Role::CheckBox:
  case Role::TextInput:
  case Role::ComboBox:
  case Role::Slider:
  case Role::Tab:
    return true;
  default:
    return false;
  }
}

const char* roleName(Role role) {
  switch (role) {
  case Role::Group:
    return "group";
  case Role::Label:
    return "text";
  case Role::Status:
    return "status";
  case Role::Button:
    return "button";
  case Role::CheckBox:
    return "checkbox";
  case Role::TextInput:
    return "textinput";
  case Role::ComboBox:
    return "combobox";
  case Role::Slider:
    return "slider";
  case Role::Tab:
    return "tab";
  case Role::ProgressBar:
    return "progressbar";
  case Role::Image:
    return "image";
  }
  return "?";
}

void Tree::clear() {
  nodes_.clear();
  focus_ = kRootId;
}

void Tree::add(Node node) {
  if (node.id == 0) {
    std::uint64_t h = 1469598103934665603ull; // FNV-1a over role and name
    auto mix = [&](unsigned char c) { h = (h ^ c) * 1099511628211ull; };
    mix(static_cast<unsigned char>(node.role));
    for (const char c : node.name)
      mix(static_cast<unsigned char>(c));
    std::uint64_t id = (h & 0xffffffffull) | (1ull << 40);
    while (find(id)) // the same name twice: the later one gets the next id
      ++id;
    node.id = id;
  }
  if (node.focused)
    focus_ = node.id;
  nodes_.push_back(std::move(node));
}

const Node* Tree::find(std::uint64_t id) const {
  for (const Node& n : nodes_)
    if (n.id == id)
      return &n;
  return nullptr;
}

std::vector<std::string> Tree::problems() const {
  std::vector<std::string> out;
  for (const Node& n : nodes_)
    if (interactive(n.role) && n.name.empty())
      out.push_back(std::string(roleName(n.role)) + " without a name at " + std::to_string(int(n.x)) + "," +
                    std::to_string(int(n.y)));
  return out;
}

std::string Tree::dump() const {
  std::string out;
  for (const Node& n : nodes_) {
    out += roleName(n.role);
    out += n.focused ? "*" : "";
    out += n.checkable ? (n.checked ? " [x] " : " [ ] ") : " ";
    out += "\"" + n.name + "\"";
    if (!n.value.empty())
      out += " = \"" + n.value + "\"";
    if (!n.help.empty())
      out += " (" + n.help + ")";
    out += "\n";
  }
  return out;
}

} // namespace viz::access
