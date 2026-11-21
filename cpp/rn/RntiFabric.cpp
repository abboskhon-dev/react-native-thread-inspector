#define RN_ALLOW_FRAMEWORKS 1

#include "RntiFabric.h"

#include "Inspector.h"
#include "RntiPlatform.h"

#include <react/renderer/mounting/ShadowTree.h>
#include <react/renderer/uimanager/UIManager.h>
#include <react/renderer/uimanager/UIManagerBinding.h>
#include <react/renderer/uimanager/UIManagerCommitHook.h>
#include <react/renderer/uimanager/UIManagerMountHook.h>

#include <mutex>

namespace rnti {
namespace {

using namespace facebook::react;

struct PendingCommit {
  SurfaceId surface = -1;
  uint64_t startNs = 0;
};

// Read-only: always returns the tree it was given.
class FabricObserver final : public UIManagerCommitHook, public UIManagerMountHook {
 public:
  void commitHookWasRegistered(const UIManager &) noexcept override {}
  void commitHookWasUnregistered(const UIManager &) noexcept override {}

  RootShadowNode::Unshared shadowTreeWillCommit(const ShadowTree &tree, const RootShadowNode::Shared &,
                                                const RootShadowNode::Unshared &newRoot,
                                                const ShadowTreeCommitOptions &) noexcept override {
    // Retries overwrite this, so a span covers only the attempt that succeeded.
    if (active()) pending() = {tree.getSurfaceId(), nowNs()};
    return newRoot;
  }

  void shadowTreeDidCommit(const ShadowTree &tree, const RootShadowNode::Shared &,
                           const std::vector<const LayoutableShadowNode *> &) noexcept override {
    PendingCommit &p = pending();
    Inspector *ins = active();
    if (ins && p.startNs && p.surface == tree.getSurfaceId()) {
      const uint64_t end = nowNs();
      ins->recorder().complete(Category::Fabric, labels(*ins).commit, p.startNs, end - p.startNs);
    }
    p = {};
  }

  void shadowTreeDidMount(const RootShadowNode::Shared &root, HighResTimeStamp) noexcept override {
    if (Inspector *ins = active(); ins && root)
      ins->recorder().instant(Category::Fabric, labels(*ins).mount, static_cast<uint64_t>(root->getSurfaceId()));
  }

 private:
  struct Labels {
    uint32_t commit = 0;
    uint32_t mount = 0;
  };

  static Inspector *active() noexcept {
    Inspector *ins = Inspector::shared();
    return ins && ins->fabricActive() ? ins : nullptr;
  }

  static PendingCommit &pending() noexcept {
    thread_local PendingCommit p;
    return p;
  }

  Labels &labels(Inspector &ins) noexcept {
    std::call_once(labelsOnce_, [&] {
      labels_.commit = ins.recorder().intern("fabric.commit_and_layout");
      labels_.mount = ins.recorder().intern("fabric.mount_reported");
    });
    return labels_;
  }

  std::once_flag labelsOnce_;
  Labels labels_;
};

// Leaked: a UIManager may call hooks until it is destroyed, and we never unregister.
FabricObserver &observer() {
  static auto *o = new FabricObserver();
  return *o;
}

} // namespace

bool installFabricObserver(facebook::jsi::Runtime &runtime) {
  auto binding = UIManagerBinding::getBinding(runtime);
  if (!binding) return false;
  // A runtime has exactly one UIManager, so a runtime marker dedupes across reloads.
  constexpr const char *kMarker = "__rntiFabricObserver";
  auto global = runtime.global();
  if (global.hasProperty(runtime, kMarker)) return true;
  global.setProperty(runtime, kMarker, true);
  UIManager &ui = binding->getUIManager();
  ui.registerCommitHook(observer());
  ui.registerMountHook(observer());
  return true;
}

} // namespace rnti
