#pragma once

#include "Event.h"

#include <atomic>
#include <memory>
#include <vector>

namespace rnti {

struct SnapshotEvent {
  Event event;
  uint32_t ring = 0;
  uint64_t position = 0;
};

struct RingStats {
  uint64_t written = 0;
  uint64_t overwritten = 0;
  uint64_t skipped = 0;
  uint64_t oldestNs = 0;
  uint64_t newestNs = 0;
  // Set when merging snapshots: positions overwritten between the pin and the tail.
  uint64_t lostAfterPin = 0;
  uint64_t gapFromNs = 0;
  uint64_t gapToNs = 0;
};

// Single producer, any number of concurrent readers; readers never wait for the producer.
class EventRing {
 public:
  explicit EventRing(uint32_t minCapacity);

  void push(const Event &e) noexcept;

  // Appends retained events overlapping [fromNs, toNs]; torn or lapped slots are counted as skipped.
  RingStats collect(uint64_t fromNs, uint64_t toNs, uint32_t ringIndex, std::vector<SnapshotEvent> &out) const;

  uint64_t capacity() const noexcept { return mask_ + 1; }

 private:
  static constexpr size_t kWords = sizeof(Event) / sizeof(uint64_t);

  struct Slot {
    std::atomic<uint64_t> seq{0};
    std::atomic<uint64_t> words[kWords]{};
  };

 public:
  static constexpr size_t kSlotBytes = sizeof(Slot);

 private:
  std::unique_ptr<Slot[]> slots_;
  uint64_t mask_ = 0;
  std::atomic<uint64_t> head_{0};
};

} // namespace rnti
