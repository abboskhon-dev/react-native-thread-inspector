#pragma once

#include "Event.h"
#include "EventRing.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rnti {

struct RecorderConfig {
  uint32_t eventsPerThread = 2048;
  uint32_t maxThreads = 16;
  uint32_t controlEvents = 1024;
  uint32_t maxLabels = 4096;
  uint32_t maxThreadNames = 128;
};

struct RingCoverage {
  uint32_t ring = 0;
  uint32_t ownerTid = 0;
  RingStats stats;
};

struct Snapshot {
  uint64_t fromNs = 0;
  uint64_t toNs = 0;
  uint64_t takenAtNs = 0;
  std::vector<SnapshotEvent> events;
  std::vector<RingCoverage> rings;
  uint64_t droppedNoRing = 0;
  uint64_t droppedLabels = 0;
};

constexpr uint32_t kControlRing = 0;
constexpr uint32_t kUnknownLabel = 0;

class Recorder {
 public:
  explicit Recorder(RecorderConfig config = {});
  ~Recorder();
  Recorder(const Recorder &) = delete;
  Recorder &operator=(const Recorder &) = delete;

  void setEnabled(bool enabled) noexcept { enabled_.store(enabled, std::memory_order_relaxed); }
  bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }

  // Not for hot paths: callers intern once and cache the ID.
  uint32_t intern(std::string_view label);
  std::string labelName(uint32_t id) const;

  void nameCurrentThread(std::string_view name);
  void nameThread(uint32_t tid, std::string_view name);
  std::vector<std::pair<uint32_t, std::string>> threadNames() const;

  uint64_t newSpanId() noexcept;

  void record(Event e) noexcept;
  // Reserved ring for watchdog, incident and lifecycle events so floods cannot evict them.
  void recordControl(Event e) noexcept;

  // async: elapsed span (e.g. across await); never counted as execution on its thread.
  uint64_t begin(Category category, uint32_t label, uint64_t flowId = 0, bool async = false) noexcept;
  void end(Category category, uint32_t label, uint64_t spanId, uint16_t eventFlags = 0, bool async = false) noexcept;
  void complete(Category category, uint32_t label, uint64_t startNs, uint64_t durNs, uint16_t eventFlags = 0,
                uint64_t flowId = 0) noexcept;
  void instant(Category category, uint32_t label, uint64_t arg0 = 0, uint16_t eventFlags = 0) noexcept;

  Snapshot snapshot(uint64_t fromNs, uint64_t toNs) const;

  const RecorderConfig &config() const noexcept { return config_; }
  uint64_t droppedNoRing() const noexcept { return droppedNoRing_.load(std::memory_order_relaxed); }

  struct Pool;

 private:
  EventRing *threadRing() noexcept;

  const RecorderConfig config_;
  const uint64_t id_;
  std::shared_ptr<Pool> pool_;
  EventRing control_;
  std::mutex controlMu_;
  std::atomic<bool> enabled_{true};
  std::atomic<uint64_t> nextSpanId_{1};
  std::atomic<uint64_t> droppedNoRing_{0};

  mutable std::mutex labelsMu_;
  std::vector<std::string> labels_;
  std::unordered_map<std::string, uint32_t> labelIds_;
  uint64_t droppedLabels_ = 0;

  mutable std::mutex namesMu_;
  std::unordered_map<uint32_t, std::string> threadNames_;
};

} // namespace rnti
