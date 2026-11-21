#include "Watchdog.h"

#include "RntiPlatform.h"

#include <chrono>
#include <limits>

namespace rnti {

namespace {
constexpr uint64_t kNever = std::numeric_limits<uint64_t>::max();
}

Watchdog::Watchdog(Recorder &recorder, IncidentController &controller, WatchdogConfig config)
    : recorder_(recorder),
      controller_(controller),
      config_(config),
      lateLabel_(recorder.intern("watchdog.late_wake")),
      postFailLabel_(recorder.intern("watchdog.post_failed")) {}

Watchdog::~Watchdog() { stop(); }

void Watchdog::addChannel(ProbeChannelConfig config) {
  Channel ch;
  ch.probeLabel = recorder_.intern(config.name + ".probe");
  ch.stallLabel = recorder_.intern(config.name + ".stall_detected");
  ch.config = std::move(config);
  channels_.push_back(std::move(ch));
}

void Watchdog::start() {
  if (thread_.joinable()) return;
  {
    std::lock_guard<std::mutex> lk(wake_->mu);
    wake_->stop = false;
  }
  controller_.enable(nowNs());
  thread_ = std::thread([this] { run(); });
}

void Watchdog::stop() {
  if (!thread_.joinable()) return;
  {
    std::lock_guard<std::mutex> lk(wake_->mu);
    wake_->stop = true;
  }
  wake_->cv.notify_one();
  thread_.join();
}

void Watchdog::setForeground(bool foreground) {
  foreground_.store(foreground);
  {
    std::lock_guard<std::mutex> lk(wake_->mu);
    wake_->pending = true;
  }
  wake_->cv.notify_one();
}

void Watchdog::hintThread(size_t channel, uint32_t tid) {
  if (channel < channels_.size()) channels_[channel].ack->tidHint.store(tid, std::memory_order_relaxed);
}

void Watchdog::resetChannels(uint64_t now) {
  for (auto &ch : channels_) {
    ch.ack->expectedSeq.store(0);
    ch.outstanding = false;
    ch.stalled = false;
    ch.lastAckNs = 0;
    ch.nextProbeNs = now;
  }
}

void Watchdog::service(Channel &ch, uint64_t now) {
  if (ch.outstanding) {
    if (ch.ack->ackedSeq.load(std::memory_order_acquire) == ch.seq) {
      const uint64_t ackNs = ch.ack->ackNs.load(std::memory_order_relaxed);
      ch.lastAckTid = ch.ack->ackTid.load(std::memory_order_relaxed);
      Event e;
      e.ts = ch.enqueueNs;
      e.dur = ackNs > ch.enqueueNs ? ackNs - ch.enqueueNs : 0;
      e.kind = EventKind::Complete;
      e.category = Category::Watchdog;
      e.label = ch.probeLabel;
      e.tid = ch.lastAckTid;
      e.arg0 = ch.seq;
      recorder_.recordControl(e);
      if (ch.stalled) {
        controller_.stallEnded(ch.config.source, ackNs, ch.lastAckTid);
      } else if (ackNs - ch.enqueueNs >= ch.config.thresholdNs) {
        // Watchdog woke after the ack: the stall is only seen once it is over.
        controller_.stallBegan(ch.config.source, ch.lastAckTid, ch.enqueueNs, ch.lastAckNs, now);
        controller_.stallEnded(ch.config.source, ackNs, ch.lastAckTid);
      }
      ch.stalled = false;
      ch.outstanding = false;
      ch.lastAckNs = ackNs;
      ch.nextProbeNs = ackNs + ch.config.intervalNs;
    } else if (!ch.stalled && now - ch.enqueueNs >= ch.config.thresholdNs) {
      ch.stalled = true;
      if (!ch.lastAckTid) ch.lastAckTid = ch.ack->tidHint.load(std::memory_order_relaxed);
      Event e;
      e.ts = now;
      e.kind = EventKind::Instant;
      e.category = Category::Watchdog;
      e.label = ch.stallLabel;
      e.tid = ch.lastAckTid;
      e.arg0 = now - ch.enqueueNs;
      e.flags = flags::Approximate;
      recorder_.recordControl(e);
      controller_.stallBegan(ch.config.source, ch.lastAckTid, ch.enqueueNs, ch.lastAckNs, now);
    }
  }

  if (!ch.outstanding && now >= ch.nextProbeNs) {
    const uint64_t seq = ++ch.seq;
    ch.ack->expectedSeq.store(seq, std::memory_order_relaxed);
    ch.enqueueNs = nowNs();
    auto state = ch.ack;
    auto wake = wake_;
    const bool posted = ch.config.post([state, wake, seq] {
      if (state->expectedSeq.load(std::memory_order_relaxed) != seq) return;
      state->ackNs.store(nowNs(), std::memory_order_relaxed);
      state->ackTid.store(currentTid(), std::memory_order_relaxed);
      state->ackedSeq.store(seq, std::memory_order_release);
      {
        std::lock_guard<std::mutex> lk(wake->mu);
        wake->pending = true;
      }
      wake->cv.notify_one();
    });
    if (posted) {
      ch.outstanding = true;
    } else {
      recorder_.recordControl({.ts = now, .label = postFailLabel_, .kind = EventKind::Instant,
                               .category = Category::Watchdog});
      ch.nextProbeNs = now + ch.config.intervalNs;
    }
  }
}

uint64_t Watchdog::nextWakeNs(uint64_t now) const {
  uint64_t next = kNever;
  if (foreground_.load()) {
    for (const auto &ch : channels_) {
      uint64_t at;
      if (!ch.outstanding) at = ch.nextProbeNs;
      else if (!ch.stalled) at = ch.enqueueNs + ch.config.thresholdNs;
      else continue; // the ack itself wakes us
      next = std::min(next, std::max(at, now));
    }
  }
  if (const uint64_t d = controller_.nextDeadlineNs(); d != 0) next = std::min(next, std::max(d, now));
  return next;
}

void Watchdog::run() {
  recorder_.nameCurrentThread("rnti.watchdog");
  bool wasForeground = foreground_.load();
  resetChannels(nowNs());
  uint64_t scheduled = kNever;

  while (true) {
    const uint64_t now = nowNs();
    const bool fg = foreground_.load();
    if (fg != wasForeground) {
      if (fg) {
        controller_.resume(now);
      } else {
        controller_.suspend(now);
      }
      resetChannels(now);
      wasForeground = fg;
      scheduled = kNever;
    }
    if (fg && scheduled != kNever && now > scheduled + config_.lateWakeThresholdNs) {
      // Our own late wake points at process-wide scheduling delay, not a specific thread.
      recorder_.recordControl({.ts = now, .arg0 = now - scheduled, .label = lateLabel_,
                               .kind = EventKind::Instant, .category = Category::Watchdog});
    }
    if (fg) {
      for (auto &ch : channels_) service(ch, now);
    }
    controller_.tick(now);

    scheduled = nextWakeNs(nowNs());
    std::unique_lock<std::mutex> lk(wake_->mu);
    if (wake_->stop) break;
    if (!wake_->pending) {
      if (scheduled == kNever) {
        wake_->cv.wait(lk, [&] { return wake_->pending || wake_->stop; });
      } else {
        const uint64_t cur = nowNs();
        const auto waitNs = std::chrono::nanoseconds(scheduled > cur ? scheduled - cur : 0);
        wake_->cv.wait_for(lk, waitNs, [&] { return wake_->pending || wake_->stop; });
      }
    }
    if (wake_->pending) scheduled = kNever;
    wake_->pending = false;
    if (wake_->stop) break;
  }
}

} // namespace rnti
