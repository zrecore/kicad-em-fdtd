#include "viz/screen_reader.hpp"

#if defined(FDTD_SCREEN_READER) && defined(__APPLE__)
#include <atomic>
#include <mutex>

#include <accesskit.h>

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

namespace viz {

struct ScreenReader::Impl {
  accesskit_macos_subclassing_adapter* adapter = nullptr;
  std::atomic<bool> active{false}, needFull{false};
  std::mutex mutex; // the platform may call the handlers from its own thread
  std::vector<access::Action> actions;
  access::Tree last;
  const access::Tree* sending = nullptr;
  int lastFocused = -1;
};

namespace {

accesskit_role roleOf(access::Role r) {
  using access::Role;
  switch (r) {
  case Role::Group:
    return ACCESSKIT_ROLE_GENERIC_CONTAINER;
  case Role::Label:
    return ACCESSKIT_ROLE_LABEL;
  case Role::Status:
    return ACCESSKIT_ROLE_STATUS;
  case Role::Button:
    return ACCESSKIT_ROLE_BUTTON;
  case Role::CheckBox:
    return ACCESSKIT_ROLE_CHECK_BOX;
  case Role::TextInput:
    return ACCESSKIT_ROLE_TEXT_INPUT;
  case Role::ComboBox:
    return ACCESSKIT_ROLE_COMBO_BOX;
  case Role::Slider:
    return ACCESSKIT_ROLE_SLIDER;
  case Role::Tab:
    return ACCESSKIT_ROLE_TAB;
  case Role::ProgressBar:
    return ACCESSKIT_ROLE_PROGRESS_INDICATOR;
  case Role::Image:
    return ACCESSKIT_ROLE_IMAGE;
  }
  return ACCESSKIT_ROLE_GENERIC_CONTAINER;
}

accesskit_node* windowNode(const access::Tree* tree) {
  accesskit_node* root = accesskit_node_new(ACCESSKIT_ROLE_WINDOW);
  accesskit_node_set_label(root, "kicad-em-fdtd solver");
  if (tree)
    for (const access::Node& n : tree->nodes())
      accesskit_node_push_child(root, n.id);
  return root;
}

accesskit_tree_update* build(const access::Tree* tree) {
  const std::size_t count = tree ? tree->nodes().size() : 0;
  accesskit_tree_update* update =
      accesskit_tree_update_with_capacity_and_focus(count + 1, tree ? tree->focus() : access::kRootId);
  accesskit_tree_update_set_tree_info(update, accesskit_tree_info_new(access::kRootId));
  accesskit_tree_update_push_node(update, access::kRootId, windowNode(tree));
  if (!tree)
    return update;
  for (const access::Node& n : tree->nodes()) {
    using access::Role;
    accesskit_node* node = accesskit_node_new(roleOf(n.role));
    if (n.role == Role::Label || n.role == Role::Status) {
      accesskit_node_set_value(node, n.name.c_str()); // static text is the node's value
    } else {
      accesskit_node_set_label(node, n.name.c_str());
      if (!n.value.empty())
        accesskit_node_set_value(node, n.value.c_str());
    }
    if (!n.help.empty())
      accesskit_node_set_description(node, n.help.c_str());
    accesskit_node_set_bounds(node, {n.x, n.y, n.x + n.w, n.y + n.h});
    if (n.live)
      accesskit_node_set_live(node, ACCESSKIT_LIVE_POLITE);
    if (n.checkable)
      accesskit_node_set_toggled(node, n.checked ? ACCESSKIT_TOGGLED_TRUE : ACCESSKIT_TOGGLED_FALSE);
    if (n.role == Role::Tab)
      accesskit_node_set_selected(node, n.selected);
    if (access::interactive(n.role) && n.role != Role::TextInput)
      accesskit_node_add_action(node, ACCESSKIT_ACTION_CLICK);
    if (!n.key.empty())
      accesskit_node_add_action(node, ACCESSKIT_ACTION_SET_VALUE);
    accesskit_tree_update_push_node(update, n.id, node);
  }
  return update;
}

accesskit_tree_update*
activated(void* userdata) { // an assistive technology attached: a stub, the full tree follows
  auto* impl = static_cast<ScreenReader::Impl*>(userdata);
  impl->needFull = true;
  impl->active = true;
  return build(nullptr);
}

void requested(accesskit_action_request* request, void* userdata) {
  auto* impl = static_cast<ScreenReader::Impl*>(userdata);
  access::Action a;
  a.id = request->target_node;
  bool known = true;
  switch (request->action) {
  case ACCESSKIT_ACTION_CLICK:
    a.kind = access::Action::Kind::Click;
    break;
  case ACCESSKIT_ACTION_FOCUS:
    a.kind = access::Action::Kind::Focus;
    break;
  case ACCESSKIT_ACTION_SET_VALUE:
    a.kind = access::Action::Kind::SetValue;
    if (request->data.has_value && request->data.value.tag == ACCESSKIT_ACTION_DATA_VALUE &&
        request->data.value.value)
      a.text = request->data.value.value;
    break;
  default:
    known = false;
  }
  if (known) {
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->actions.push_back(std::move(a));
  }
  accesskit_action_request_free(request);
}

accesskit_tree_update* factory(void* userdata) {
  return build(static_cast<ScreenReader::Impl*>(userdata)->sending);
}

} // namespace

ScreenReader::ScreenReader() : impl_(std::make_unique<Impl>()) {}

ScreenReader::~ScreenReader() {
  if (impl_ && impl_->adapter)
    accesskit_macos_subclassing_adapter_free(impl_->adapter);
}

std::unique_ptr<ScreenReader> ScreenReader::create(GLFWwindow* window) {
  void* ns = window ? glfwGetCocoaWindow(window) : nullptr;
  if (!ns)
    return nullptr;
  std::unique_ptr<ScreenReader> reader(new ScreenReader());
  // GLFW puts keyboard focus on the window rather than its content view; VoiceOver needs to be told.
  static bool forwarded = false;
  if (!forwarded) {
    accesskit_macos_add_focus_forwarder_to_window_class("GLFWWindow");
    forwarded = true;
  }
  reader->impl_->adapter = accesskit_macos_subclassing_adapter_for_window(ns, activated, reader->impl_.get(),
                                                                          requested, reader->impl_.get());
  return reader;
}

bool ScreenReader::active() const {
  return impl_->active;
}

void ScreenReader::update(const access::Tree& tree, bool windowFocused) {
  if (!impl_->active || !impl_->adapter)
    return;
  if (int(windowFocused) != impl_->lastFocused) {
    impl_->lastFocused = windowFocused;
    if (auto* events =
            accesskit_macos_subclassing_adapter_update_view_focus_state(impl_->adapter, windowFocused))
      accesskit_macos_queued_events_raise(events);
  }
  if (!impl_->needFull.exchange(false) && tree == impl_->last)
    return;
  impl_->last = tree;
  impl_->sending = &impl_->last;
  if (auto* events =
          accesskit_macos_subclassing_adapter_update_if_active(impl_->adapter, factory, impl_.get()))
    accesskit_macos_queued_events_raise(events);
}

std::vector<access::Action> ScreenReader::takeActions() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<access::Action> out;
  out.swap(impl_->actions);
  return out;
}

} // namespace viz

#else // no adapter on this platform yet: the tree is still built for tests (--ui-a11y-dump)

namespace viz {
struct ScreenReader::Impl {};
ScreenReader::ScreenReader() = default;
ScreenReader::~ScreenReader() = default;
std::unique_ptr<ScreenReader> ScreenReader::create(GLFWwindow*) {
  return nullptr;
}
bool ScreenReader::active() const {
  return false;
}
void ScreenReader::update(const access::Tree&, bool) {}
std::vector<access::Action> ScreenReader::takeActions() {
  return {};
}
} // namespace viz

#endif
