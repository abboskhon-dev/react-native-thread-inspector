#include "Watchdog.h"

#include "RntiPlatform.h"
#include "ThreadCpu.h"

#include <chrono>
#include <limits>

namespace rnti {

namespace {
constexpr uint64_t kNever = std::numeric_limits<uint64_t>::max();
// CPU the observed thread may use after its ack is stamped, returning to its loop; more counts as work.
constexpr uint64_t kAckTailNs = 50 * 1000;
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

void Watchdog::hintThread(size_t channel, uint32_t tid, uint64_t cpuHandle) {
  if (channel >= channels_.size()) return;
  channels_[channel].ack->tidHint.store(tid, std::memory_order_relaxed);
  if (cpuHandle) channels_[channel].ack->cpuHandle.store(cpuHandle, std::memory_order_relaxed);
}

bool Watchdog::cpuMoved(Channel &ch) {
  uint64_t cpu = 0;
  const uint64_t handle = ch.ack->cpuHandle.load(std::memory_order_relaxed);
  if (!handle || !threadCpuNs(handle, cpu)) {
    ch.cpuKnown = false;
    return true;
  }
  const bool moved = !ch.cpuKnown || cpu > ch.cpuNs;
  ch.cpuKnown = true;
  ch.cpuNs = cpu;
  return moved;
}

void Watchdog::resetChannels(uint64_t now) {
  for (auto &ch : channels_) {
    ch.ack->expectedSeq.store(0);
    ch.outstanding = false;
    ch.stalled = false;
    ch.lastAckNs = 0;
    ch.nextProbeNs = now;
    ch.cpuKnown = false;
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
      ch.nextProbeNs = std::max(ch.enqueueNs + ch.config.intervalNs, ackNs);
      const uint64_t ackCpu = ch.ack->ackCpuNs.load(std::memory_order_relaxed);
      ch.cpuKnown = ackCpu != 0;
      ch.cpuNs = ackCpu + kAckTailNs;
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

  if (!ch.outstanding && now >= ch.nextProbeNs && ch.config.idleProbeNs &&
      now - ch.enqueueNs < ch.config.idleProbeNs && !cpuMoved(ch)) {
    ch.nextProbeNs = now + ch.config.intervalNs;
    return;
  }

  if (!ch.outstanding && now >= ch.nextProbeNs) {
    const uint64_t seq = ++ch.seq;
    ch.ack->expectedSeq.store(seq, std::memory_order_relaxed);
    ch.enqueueNs = nowNs();
    auto state = ch.ack;
    // No wake: the ack stamps its own time and the next poll collects it.
    const bool posted = ch.config.post([state, seq] {
      if (state->expectedSeq.load(std::memory_order_relaxed) != seq) return;
      const uint64_t handle = currentCpuHandle();
      state->cpuHandle.store(handle, std::memory_order_relaxed);
      state->ackNs.store(nowNs(), std::memory_order_relaxed);
      state->ackTid.store(currentTid(), std::memory_order_relaxed);
      uint64_t cpu = 0;
      state->ackCpuNs.store(threadCpuNs(handle, cpu) ? cpu : 0, std::memory_order_relaxed);
      state->ackedSeq.store(seq, std::memory_order_release);
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
      if (!ch.outstanding) {
        at = ch.nextProbeNs;
      } else {
        const uint64_t step = std::max<uint64_t>(ch.config.intervalNs, kMs);
        const uint64_t elapsed = now > ch.enqueueNs ? now - ch.enqueueNs : 0;
        at = ch.enqueueNs + (elapsed / step + 1) * step;
        if (!ch.stalled) at = std::min(at, ch.enqueueNs + ch.config.thresholdNs);
      }
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
