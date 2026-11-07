#include "EventRing.h"

#include <algorithm>
#include <cstring>

namespace rnti {

const char *categoryName(Category c) noexcept {
  switch (c) {
    case Category::Js: return "js";
    case Category::Native: return "native";
    case Category::MainThread: return "main";
    case Category::Frame: return "frame";
    case Category::Navigation: return "navigation";
    case Category::Watchdog: return "watchdog";
    case Category::Incident: return "incident";
    case Category::Lifecycle: return "lifecycle";
    case Category::TurboModule: return "turbomodule";
    case Category::Inspector: return "inspector";
    case Category::User: return "user";
  }
  return "unknown";
}

EventRing::EventRing(uint32_t minCapacity) {
  uint64_t cap = 2;
  while (cap < minCapacity) cap <<= 1;
  slots_ = std::make_unique<Slot[]>(cap);
  mask_ = cap - 1;
}

void EventRing::push(const Event &e) noexcept {
  const uint64_t pos = head_.load(std::memory_order_relaxed);
  Slot &slot = slots_[pos & mask_];
  uint64_t words[kWords];
  std::memcpy(words, &e, sizeof(Event));

  slot.seq.store(2 * pos + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  for (size_t i = 0; i < kWords; ++i) slot.words[i].store(words[i], std::memory_order_relaxed);
  slot.seq.store(2 * pos + 2, std::memory_order_release);
  head_.store(pos + 1, std::memory_order_release);
}

RingStats EventRing::collect(uint64_t fromNs, uint64_t toNs, uint32_t ringIndex,
                             std::vector<SnapshotEvent> &out) const {
  RingStats stats;
  const uint64_t head = head_.load(std::memory_order_acquire);
  const uint64_t cap = mask_ + 1;
  const uint64_t start = head > cap ? head - cap : 0;
  stats.written = head;
  stats.overwritten = start;

  bool first = true;
  for (uint64_t pos = start; pos < head; ++pos) {
    const Slot &slot = slots_[pos & mask_];
    const uint64_t expected = 2 * pos + 2;
    if (slot.seq.load(std::memory_order_acquire) != expected) {
      ++stats.skipped;
      continue;
    }
    uint64_t words[kWords];
    for (size_t i = 0; i < kWords; ++i) words[i] = slot.words[i].load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (slot.seq.load(std::memory_order_relaxed) != expected) {
      ++stats.skipped;
      continue;
    }

    Event e;
    std::memcpy(&e, words, sizeof(Event));
    const uint64_t endTs = e.ts + e.dur;
    // Coverage starts at the oldest retained record's push time, approximated by its end.
    if (first) {
      stats.oldestNs = endTs;
      first = false;
    }
    stats.newestNs = std::max(stats.newestNs, endTs);
    if (endTs >= fromNs && e.ts <= toNs) out.push_back({e, ringIndex, pos});
  }
  return stats;
}

} // namespace rnti
