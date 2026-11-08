#include "TurboModuleTracker.h"

#include <cstring>
#include <string>

namespace rnti {

namespace {

constexpr int kMaxDepth = 16;
constexpr int kCacheSize = 64;

struct Frame {
  TurboModuleTracker::Kind kind;
  uint32_t label;
  uint64_t span;
  int32_t id;
};

struct ThreadFrames {
  Frame frames[kMaxDepth];
  int depth = 0;
};

struct CacheEntry {
  const char *module = nullptr;
  const char *method = nullptr;
  TurboModuleTracker::Kind kind{};
  std::string moduleCopy;
  std::string methodCopy;
  uint32_t label = 0;
};

thread_local ThreadFrames tFrames;
thread_local CacheEntry tCache[kCacheSize];

Category categoryOf(TurboModuleTracker::Kind) { return Category::TurboModule; }

} // namespace

TurboModuleTracker::TurboModuleTracker(Recorder &recorder) : recorder_(recorder) {}

void TurboModuleTracker::setIgnoredModule(const char *moduleName, const char *allowedMethodPrefix) {
  std::strncpy(ignoredModule_, moduleName ? moduleName : "", sizeof(ignoredModule_) - 1);
  std::strncpy(allowedPrefix_, allowedMethodPrefix ? allowedMethodPrefix : "", sizeof(allowedPrefix_) - 1);
}

bool TurboModuleTracker::skip(const char *module, const char *method) const noexcept {
  if (!module) return true;
  if (!ignoredModule_[0] || std::strcmp(module, ignoredModule_) != 0) return false;
  return !method || !allowedPrefix_[0] || std::strncmp(method, allowedPrefix_, std::strlen(allowedPrefix_)) != 0;
}

uint32_t TurboModuleTracker::label(Kind kind, const char *module, const char *method) noexcept {
  const size_t h = (reinterpret_cast<uintptr_t>(module) ^ (reinterpret_cast<uintptr_t>(method) >> 3) ^
                    static_cast<uintptr_t>(kind)) % kCacheSize;
  CacheEntry &c = tCache[h];
  // Pointers can be reused for different strings, so the copies confirm a hit.
  if (c.label && c.module == module && c.method == method && c.kind == kind && c.moduleCopy == module &&
      (!method || c.methodCopy == method))
    return c.label;
  std::string name = std::string(module) + "." + (method ? method : "(async method)");
  if (kind == Kind::AsyncCall) name += " (js dispatch)";
  c.module = module;
  c.method = method;
  c.kind = kind;
  c.moduleCopy = module;
  c.methodCopy = method ? method : "";
  c.label = recorder_.intern(name);
  return c.label;
}

void TurboModuleTracker::open(Kind kind, const char *module, const char *method, int32_t id) noexcept {
  if (!enabled() || skip(module, method)) return;
  auto &t = tFrames;
  if (t.depth >= kMaxDepth) {
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint32_t l = label(kind, module, method);
  t.frames[t.depth++] = {kind, l, recorder_.begin(categoryOf(kind), l), id};
}

void TurboModuleTracker::close(Kind kind, const char *module, const char *method, int32_t id, bool failed) noexcept {
  auto &t = tFrames;
  if (t.depth == 0 && !enabled()) return;
  if (skip(module, method)) return;
  const uint32_t l = label(kind, module, method);
  for (int i = t.depth - 1; i >= 0; --i) {
    const Frame &f = t.frames[i];
    if (f.kind != kind || f.label != l || f.id != id) continue;
    // Frames above the match lost their terminal callback (e.g. an exception path).
    for (int j = t.depth - 1; j > i; --j) {
      const Frame &lost = t.frames[j];
      recorder_.end(categoryOf(lost.kind), lost.label, lost.span, flags::Incomplete);
    }
    recorder_.end(categoryOf(kind), l, f.span, failed ? flags::Error : 0);
    t.depth = i;
    return;
  }
  unmatched_.fetch_add(1, std::memory_order_relaxed);
}

void TurboModuleTracker::syncStart(const char *m, const char *f) noexcept { open(Kind::Sync, m, f, 0); }
void TurboModuleTracker::syncEnd(const char *m, const char *f, bool failed) noexcept {
  close(Kind::Sync, m, f, 0, failed);
}
void TurboModuleTracker::asyncCallStart(const char *m, const char *f) noexcept { open(Kind::AsyncCall, m, f, 0); }
void TurboModuleTracker::asyncCallEnd(const char *m, const char *f, bool failed) noexcept {
  close(Kind::AsyncCall, m, f, 0, failed);
}
void TurboModuleTracker::asyncExecStart(const char *m, const char *f, int32_t id) noexcept {
  open(Kind::AsyncExec, m, f, id);
}
void TurboModuleTracker::asyncExecEnd(const char *m, const char *f, int32_t id, bool failed) noexcept {
  close(Kind::AsyncExec, m, f, id, failed);
}

} // namespace rnti
