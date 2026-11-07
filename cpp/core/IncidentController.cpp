#include "IncidentController.h"

#include <algorithm>
#include <map>
#include <set>

namespace rnti {

const char *triggerSourceName(TriggerSource s) noexcept {
  switch (s) {
    case TriggerSource::JsWatchdog: return "js_watchdog";
    case TriggerSource::MainWatchdog: return "main_watchdog";
    case TriggerSource::JsLongTask: return "js_long_task";
    case TriggerSource::Manual: return "manual";
  }
  return "unknown";
}

const char *endReasonName(EndReason r) noexcept {
  switch (r) {
    case EndReason::Recovered: return "recovered";
    case EndReason::MaxDuration: return "max_duration";
    case EndReason::Lifecycle: return "lifecycle";
    case EndReason::Disabled: return "disabled";
  }
  return "unknown";
}

namespace {

void mergeSnapshot(Snapshot &pinned, Snapshot &&tail) {
  std::set<std::pair<uint32_t, uint64_t>> seen;
  for (const auto &e : pinned.events) seen.emplace(e.ring, e.position);
  for (auto &e : tail.events) {
    if (seen.emplace(e.ring, e.position).second) pinned.events.push_back(e);
  }
  std::sort(pinned.events.begin(), pinned.events.end(), [](const SnapshotEvent &a, const SnapshotEvent &b) {
    if (a.event.ts != b.event.ts) return a.event.ts < b.event.ts;
    if (a.ring != b.ring) return a.ring < b.ring;
    return a.position < b.position;
  });

  // Keep prehistory coverage from the pin; extend newest/written from the tail.
  std::map<uint32_t, size_t> index;
  for (size_t i = 0; i < pinned.rings.size(); ++i) index[pinned.rings[i].ring] = i;
  for (const auto &r : tail.rings) {
    auto it = index.find(r.ring);
    if (it == index.end()) {
      pinned.rings.push_back(r);
      continue;
    }
    auto &p = pinned.rings[it->second];
    p.stats.written = r.stats.written;
    p.stats.newestNs = std::max(p.stats.newestNs, r.stats.newestNs);
    p.stats.skipped += r.stats.skipped;
    if (p.stats.oldestNs == 0) p.stats.oldestNs = r.stats.oldestNs;
    p.ownerTid = r.ownerTid;
  }
  pinned.toNs = tail.toNs;
  pinned.droppedNoRing = tail.droppedNoRing;
  pinned.droppedLabels = tail.droppedLabels;
}

} // namespace

IncidentController::IncidentController(Recorder &recorder, IncidentConfig config, Sink sink)
    : recorder_(recorder),
      config_(config),
      sink_(std::move(sink)),
      captureLabel_(recorder.intern("incident.capture")),
      finalizeLabel_(recorder.intern("incident.finalize")) {}

void IncidentController::emit(std::optional<Incident> incident) {
  if (incident && sink_) sink_(std::move(*incident));
}

void IncidentController::enable(uint64_t) {
  std::lock_guard<std::mutex> lk(mu_);
  if (state_ == ControllerState::Disabled) state_ = ControllerState::Armed;
}

void IncidentController::disable(uint64_t now) {
  std::optional<Incident> out;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (state_ == ControllerState::Capturing) out = finalize(EndReason::Disabled, now);
    state_ = ControllerState::Disabled;
  }
  emit(std::move(out));
}

void IncidentController::suspend(uint64_t now) {
  std::optional<Incident> out;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (state_ == ControllerState::Disabled || state_ == ControllerState::Suspended) return;
    if (state_ == ControllerState::Capturing) out = finalize(EndReason::Lifecycle, now);
    stateBeforeSuspend_ = state_;
    state_ = ControllerState::Suspended;
  }
  emit(std::move(out));
}

void IncidentController::resume(uint64_t now) {
  std::lock_guard<std::mutex> lk(mu_);
  if (state_ != ControllerState::Suspended) return;
  state_ = (stateBeforeSuspend_ == ControllerState::Cooldown && now < cooldownUntil_) ? ControllerState::Cooldown
                                                                                      : ControllerState::Armed;
}

void IncidentController::beginCapture(const Trigger &t, uint64_t windowFrom) {
  Incident inc;
  inc.id = nextId_++;
  inc.triggers.push_back(t);
  inc.detectedNs = t.detectedNs;
  inc.requestedPrehistoryNs = config_.prehistoryNs;
  inc.windowStartNs = windowFrom;
  // Pin prehistory now: a long hang can overwrite it before recovery.
  inc.snapshot = recorder_.snapshot(windowFrom, t.detectedNs);
  current_ = std::move(inc);
  state_ = ControllerState::Capturing;

  Event e;
  e.ts = t.detectedNs;
  e.kind = EventKind::Instant;
  e.category = Category::Incident;
  e.label = captureLabel_;
  e.arg0 = current_->id;
  e.arg1 = static_cast<uint64_t>(t.source);
  e.tid = t.tid;
  recorder_.recordControl(e);
}

std::optional<Incident> IncidentController::finalize(EndReason reason, uint64_t now) {
  if (!current_) return std::nullopt;
  Event e;
  e.ts = now;
  e.kind = EventKind::Instant;
  e.category = Category::Incident;
  e.label = finalizeLabel_;
  e.arg0 = current_->id;
  e.arg1 = static_cast<uint64_t>(reason);
  recorder_.recordControl(e);

  Incident inc = std::move(*current_);
  current_.reset();
  inc.endReason = reason;
  inc.captureEndNs = now;
  mergeSnapshot(inc.snapshot, recorder_.snapshot(inc.windowStartNs, now));
  lastCaptureEnd_ = now;
  cooldownUntil_ = now + config_.cooldownNs;
  state_ = ControllerState::Cooldown;
  return inc;
}

bool IncidentController::allEnded() const {
  return current_ && std::all_of(current_->triggers.begin(), current_->triggers.end(),
                                 [](const Trigger &t) { return t.endNs != 0; });
}

uint64_t IncidentController::lastEnd() const {
  uint64_t end = 0;
  for (const auto &t : current_->triggers) end = std::max(end, t.endNs);
  return end;
}

void IncidentController::stallBegan(TriggerSource source, uint32_t tid, uint64_t onsetNs,
                                    uint64_t onsetLowerBoundNs, uint64_t detectedNs) {
  std::lock_guard<std::mutex> lk(mu_);
  Trigger t{source, tid, onsetNs, onsetLowerBoundNs, true, detectedNs, 0};
  if (state_ == ControllerState::Armed) {
    const uint64_t from = detectedNs > config_.prehistoryNs ? detectedNs - config_.prehistoryNs : 0;
    beginCapture(t, from);
  } else if (state_ == ControllerState::Capturing) {
    for (const auto &existing : current_->triggers) {
      if (existing.source == source && existing.endNs == 0) return;
    }
    current_->triggers.push_back(t);
  } else {
    ++suppressed_;
  }
}

void IncidentController::stallEnded(TriggerSource source, uint64_t endNs) {
  std::lock_guard<std::mutex> lk(mu_);
  if (state_ != ControllerState::Capturing) return;
  for (auto it = current_->triggers.rbegin(); it != current_->triggers.rend(); ++it) {
    if (it->source == source && it->endNs == 0) {
      it->endNs = endNs;
      return;
    }
  }
}

void IncidentController::longTaskCompleted(uint32_t tid, uint64_t startNs, uint64_t durNs, uint64_t deliveredNs) {
  if (durNs < config_.longTaskThresholdNs) return;
  std::lock_guard<std::mutex> lk(mu_);
  Trigger t{TriggerSource::JsLongTask, tid, startNs, startNs, false, deliveredNs, startNs + durNs};
  if (state_ == ControllerState::Capturing) {
    current_->triggers.push_back(t);
  } else if (state_ == ControllerState::Armed) {
    const uint64_t from = deliveredNs > config_.prehistoryNs ? deliveredNs - config_.prehistoryNs : 0;
    beginCapture(t, std::min(from, startNs));
  } else {
    // A late entry for an already-saved incident is expected; count it rather than open a duplicate.
    ++suppressed_;
  }
}

void IncidentController::tick(uint64_t now) {
  std::optional<Incident> out;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (state_ == ControllerState::Capturing) {
      if (allEnded() && now >= lastEnd() + config_.tailNs) {
        out = finalize(EndReason::Recovered, now);
      } else if (now >= current_->detectedNs + config_.maxIncidentNs) {
        out = finalize(EndReason::MaxDuration, now);
      }
    } else if (state_ == ControllerState::Cooldown && now >= cooldownUntil_) {
      state_ = ControllerState::Armed;
    }
  }
  emit(std::move(out));
}

uint64_t IncidentController::nextDeadlineNs() const {
  std::lock_guard<std::mutex> lk(mu_);
  if (state_ == ControllerState::Capturing) {
    const uint64_t maxAt = current_->detectedNs + config_.maxIncidentNs;
    return allEnded() ? std::min(maxAt, lastEnd() + config_.tailNs) : maxAt;
  }
  if (state_ == ControllerState::Cooldown) return cooldownUntil_;
  return 0;
}

ControllerState IncidentController::state() const {
  std::lock_guard<std::mutex> lk(mu_);
  return state_;
}

uint64_t IncidentController::suppressedTriggers() const {
  std::lock_guard<std::mutex> lk(mu_);
  return suppressed_;
}

} // namespace rnti
