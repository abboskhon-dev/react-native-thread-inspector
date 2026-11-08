#pragma once

#include "Recorder.h"

#include <atomic>
#include <cstdint>

namespace rnti {

// Turns TurboModule perf-logger callbacks into spans. No RN dependency; the RN adapter forwards into it.
class TurboModuleTracker {
 public:
  explicit TurboModuleTracker(Recorder &recorder);

  void setEnabled(bool enabled) noexcept { enabled_.store(enabled, std::memory_order_relaxed); }
  bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }

  // Calls from the module that hosts this inspector are its own instrumentation, except demo methods.
  void setIgnoredModule(const char *moduleName, const char *allowedMethodPrefix);

  void syncStart(const char *module, const char *method) noexcept;
  void syncEnd(const char *module, const char *method, bool failed) noexcept;
  // JS-thread cost of converting and enqueueing an async call; not the native work itself.
  void asyncCallStart(const char *module, const char *method) noexcept;
  void asyncCallEnd(const char *module, const char *method, bool failed) noexcept;
  // Execution of the native method body; `id` pairs start and end only, not dispatch.
  // `method` may be null when the platform's pointer is not safe to read.
  void asyncExecStart(const char *module, const char *method, int32_t id) noexcept;
  void asyncExecEnd(const char *module, const char *method, int32_t id, bool failed) noexcept;

  uint64_t unmatchedEnds() const noexcept { return unmatched_.load(std::memory_order_relaxed); }
  uint64_t droppedDeep() const noexcept { return dropped_.load(std::memory_order_relaxed); }

  enum class Kind : uint8_t { Sync, AsyncCall, AsyncExec };

 private:
  bool skip(const char *module, const char *method) const noexcept;
  uint32_t label(Kind kind, const char *module, const char *method) noexcept;
  void open(Kind kind, const char *module, const char *method, int32_t id) noexcept;
  void close(Kind kind, const char *module, const char *method, int32_t id, bool failed) noexcept;

  Recorder &recorder_;
  std::atomic<bool> enabled_{false};
  std::atomic<uint64_t> unmatched_{0};
  std::atomic<uint64_t> dropped_{0};
  char ignoredModule_[64] = {0};
  char allowedPrefix_[32] = {0};
};

} // namespace rnti
