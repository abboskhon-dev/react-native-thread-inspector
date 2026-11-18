#include "Recorder.h"

#include "Platform.h"

#include <algorithm>

namespace rnti {

struct Recorder::Pool {
  std::mutex mu;
  std::vector<std::unique_ptr<EventRing>> rings;
  std::vector<bool> inUse;
  std::vector<uint32_t> ownerTid;
};

namespace {

std::atomic<uint64_t> gNextRecorderId{1};

struct ThreadBinding {
  uint64_t recorderId = 0;
  EventRing *ring = nullptr;
  uint32_t ringIndex = 0;
  uint32_t tid = 0;
  std::weak_ptr<Recorder::Pool> pool;

  void release() {
    if (auto p = pool.lock(); p && ring) {
      std::lock_guard<std::mutex> lk(p->mu);
      p->inUse[ringIndex - 1] = false;
    }
    recorderId = 0;
    ring = nullptr;
    pool.reset();
  }

  ~ThreadBinding() { release(); }
};

ThreadBinding &binding() {
  thread_local ThreadBinding b;
  if (b.tid == 0) b.tid = currentTid();
  return b;
}

} // namespace

Recorder::Recorder(RecorderConfig config)
    : config_(config),
      id_(gNextRecorderId.fetch_add(1)),
      pool_(std::make_shared<Pool>()),
      control_(config.controlEvents) {
  pool_->rings.resize(config_.maxThreads);
  pool_->inUse.assign(config_.maxThreads, false);
  pool_->ownerTid.assign(config_.maxThreads, 0);
  labels_.emplace_back("(unknown)");
}

Recorder::~Recorder() = default;

uint32_t Recorder::intern(std::string_view label) {
  std::lock_guard<std::mutex> lk(labelsMu_);
  std::string key(label);
  if (auto it = labelIds_.find(key); it != labelIds_.end()) return it->second;
  if (labels_.size() >= config_.maxLabels) {
    ++droppedLabels_;
    return kUnknownLabel;
  }
  const auto id = static_cast<uint32_t>(labels_.size());
  labels_.push_back(key);
  labelIds_.emplace(std::move(key), id);
  return id;
}

std::string Recorder::labelName(uint32_t id) const {
  std::lock_guard<std::mutex> lk(labelsMu_);
  return id < labels_.size() ? labels_[id] : labels_[0];
}

void Recorder::nameCurrentThread(std::string_view name) { nameThread(binding().tid, name); }

void Recorder::nameThread(uint32_t tid, std::string_view name) {
  std::lock_guard<std::mutex> lk(namesMu_);
  if (threadNames_.size() >= config_.maxThreadNames && !threadNames_.count(tid)) return;
  threadNames_[tid] = std::string(name);
}

std::vector<std::pair<uint32_t, std::string>> Recorder::threadNames() const {
  std::lock_guard<std::mutex> lk(namesMu_);
  return {threadNames_.begin(), threadNames_.end()};
}

EventRing *Recorder::threadRing() noexcept {
  ThreadBinding &b = binding();
  if (b.recorderId == id_) return b.ring;
  b.release();

  std::lock_guard<std::mutex> lk(pool_->mu);
  b.recorderId = id_;
  b.pool = pool_;
  for (uint32_t i = 0; i < config_.maxThreads; ++i) {
    if (pool_->inUse[i]) continue;
    if (!pool_->rings[i]) pool_->rings[i] = std::make_unique<EventRing>(config_.eventsPerThread);
    pool_->inUse[i] = true;
    pool_->ownerTid[i] = b.tid;
    b.ring = pool_->rings[i].get();
    b.ringIndex = i + 1;
    return b.ring;
  }
  b.ring = nullptr;
  return nullptr;
}

void Recorder::record(Event e) noexcept {
  if (!enabled()) return;
  if (e.tid == 0) e.tid = binding().tid;
  EventRing *ring = threadRing();
  if (!ring) {
    droppedNoRing_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  ring->push(e);
}

void Recorder::recordControl(Event e) noexcept {
  if (!enabled()) return;
  if (e.tid == 0) e.tid = binding().tid;
  std::lock_guard<std::mutex> lk(controlMu_);
  control_.push(e);
}

uint64_t Recorder::newSpanId() noexcept {
  // Per-thread ID blocks: a shared counter per span bounces one cache line across cores.
  constexpr uint64_t kBlock = 4096;
  struct Block {
    uint64_t recorderId = 0, next = 0, end = 0;
  };
  thread_local Block b;
  if (b.recorderId != id_ || b.next == b.end) {
    b.recorderId = id_;
    b.next = nextSpanId_.fetch_add(kBlock, std::memory_order_relaxed);
    b.end = b.next + kBlock;
  }
  return b.next++;
}

uint64_t Recorder::begin(Category category, uint32_t label, uint64_t flowId, bool async) noexcept {
  if (!enabled()) return 0;
  const uint64_t id = newSpanId();
  Event e;
  e.ts = nowNs();
  e.kind = async ? EventKind::AsyncBegin : EventKind::Begin;
  e.category = category;
  e.label = label;
  e.spanId = id;
  e.flowId = flowId;
  record(e);
  return id;
}

void Recorder::end(Category category, uint32_t label, uint64_t spanId, uint16_t eventFlags, bool async) noexcept {
  // Span id 0 comes from a begin while disabled; its End would resolve as a truncated span.
  if (!enabled() || spanId == 0) return;
  Event e;
  e.ts = nowNs();
  e.kind = async ? EventKind::AsyncEnd : EventKind::End;
  e.category = category;
  e.label = label;
  e.spanId = spanId;
  e.flags = eventFlags;
  record(e);
}

void Recorder::complete(Category category, uint32_t label, uint64_t startNs, uint64_t durNs, uint16_t eventFlags,
                        uint64_t flowId) noexcept {
  if (!enabled()) return;
  Event e;
  e.ts = startNs;
  e.dur = durNs;
  e.kind = EventKind::Complete;
  e.category = category;
  e.label = label;
  e.flags = eventFlags;
  e.flowId = flowId;
  e.spanId = newSpanId();
  record(e);
}

void Recorder::instant(Category category, uint32_t label, uint64_t arg0, uint16_t eventFlags) noexcept {
  if (!enabled()) return;
  Event e;
  e.ts = nowNs();
  e.kind = EventKind::Instant;
  e.category = category;
  e.label = label;
  e.arg0 = arg0;
  e.flags = eventFlags;
  record(e);
}

Snapshot Recorder::snapshot(uint64_t fromNs, uint64_t toNs) const {
  Snapshot s;
  s.fromNs = fromNs;
  s.toNs = toNs;
  s.takenAtNs = nowNs();

  s.rings.push_back({kControlRing, 0, control_.collect(fromNs, toNs, kControlRing, s.events)});
  {
    std::lock_guard<std::mutex> lk(pool_->mu);
    for (uint32_t i = 0; i < config_.maxThreads; ++i) {
      if (!pool_->rings[i]) continue;
      s.rings.push_back({i + 1, pool_->ownerTid[i], pool_->rings[i]->collect(fromNs, toNs, i + 1, s.events)});
    }
  }
  s.droppedNoRing = droppedNoRing();
  {
    std::lock_guard<std::mutex> lk(labelsMu_);
    s.droppedLabels = droppedLabels_;
  }
  std::sort(s.events.begin(), s.events.end(), [](const SnapshotEvent &a, const SnapshotEvent &b) {
    if (a.event.ts != b.event.ts) return a.event.ts < b.event.ts;
    if (a.ring != b.ring) return a.ring < b.ring;
    return a.position < b.position;
  });
  return s;
}

} // namespace rnti
