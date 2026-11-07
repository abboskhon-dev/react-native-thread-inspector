#pragma once

#include <cstdint>
#include <type_traits>

namespace rnti {

enum class EventKind : uint8_t {
  Begin = 1,
  End,
  Complete,
  Instant,
  AsyncBegin,
  AsyncEnd,
  Counter,
};

enum class Category : uint8_t {
  Js = 1,
  Native,
  MainThread,
  Frame,
  Navigation,
  Watchdog,
  Incident,
  Lifecycle,
  TurboModule,
  Inspector,
  User,
};

namespace flags {
constexpr uint16_t Approximate = 1 << 0;
constexpr uint16_t LeftTruncated = 1 << 1;
constexpr uint16_t Incomplete = 1 << 2;
constexpr uint16_t Sampled = 1 << 3;
constexpr uint16_t Error = 1 << 4;
constexpr uint16_t JsDelivery = 1 << 5;
} // namespace flags

struct Event {
  uint64_t ts = 0;
  uint64_t dur = 0;
  uint64_t spanId = 0;
  uint64_t flowId = 0;
  uint64_t arg0 = 0;
  uint64_t arg1 = 0;
  uint32_t tid = 0;
  uint32_t label = 0;
  uint16_t runtimeGen = 0;
  uint16_t flags = 0;
  EventKind kind = EventKind::Instant;
  Category category = Category::Inspector;
  uint8_t clockSegment = 0;
  uint8_t reserved = 0;
};

static_assert(sizeof(Event) == 64, "Event must stay 64 bytes");
static_assert(std::is_trivially_copyable_v<Event>);

const char *categoryName(Category c) noexcept;

} // namespace rnti
