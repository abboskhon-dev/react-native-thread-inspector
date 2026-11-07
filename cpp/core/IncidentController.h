#pragma once

#include "Platform.h"
#include "Recorder.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace rnti {

enum class TriggerSource : uint8_t { JsWatchdog, MainWatchdog, JsLongTask, Manual };
const char *triggerSourceName(TriggerSource s) noexcept;

struct Trigger {
  TriggerSource source = TriggerSource::Manual;
  uint32_t tid = 0;
  // Watchdog onset is the probe enqueue time; the stall began somewhere in [onsetLowerBoundNs, onsetNs].
  uint64_t onsetNs = 0;
  uint64_t onsetLowerBoundNs = 0;
  bool onsetApproximate = true;
  uint64_t detectedNs = 0;
  uint64_t endNs = 0; // 0 = recovery not observed
};

enum class EndReason : uint8_t { Recovered, MaxDuration, Lifecycle, Disabled };
const char *endReasonName(EndReason r) noexcept;

struct Incident {
  uint64_t id = 0;
  std::vector<Trigger> triggers;
  uint64_t windowStartNs = 0;
  uint64_t detectedNs = 0;
  uint64_t captureEndNs = 0;
  uint64_t requestedPrehistoryNs = 0;
  EndReason endReason = EndReason::Recovered;
  uint32_t screenLabel = 0; // 0 = no screen reported
  Snapshot snapshot;
};

struct IncidentConfig {
  uint64_t prehistoryNs = 5000 * kMs;
  uint64_t tailNs = 1000 * kMs;
  uint64_t maxIncidentNs = 10000 * kMs;
  uint64_t cooldownNs = 30000 * kMs;
  uint64_t longTaskThresholdNs = 200 * kMs;
};

enum class ControllerState : uint8_t { Disabled, Armed, Capturing, Cooldown, Suspended };

class IncidentController {
 public:
  using Sink = std::function<void(Incident &&)>;

  IncidentController(Recorder &recorder, IncidentConfig config, Sink sink);

  void enable(uint64_t now);
  void disable(uint64_t now);
  void suspend(uint64_t now);
  void resume(uint64_t now);

  void stallBegan(TriggerSource source, uint32_t tid, uint64_t onsetNs, uint64_t onsetLowerBoundNs,
                  uint64_t detectedNs);
  void stallEnded(TriggerSource source, uint64_t endNs, uint32_t ackTid = 0);
  void longTaskCompleted(uint32_t tid, uint64_t startNs, uint64_t durNs, uint64_t deliveredNs);
  void tick(uint64_t now);
  void setScreen(uint32_t label) noexcept { screen_.store(label, std::memory_order_relaxed); }

  // Earliest time tick() has work to do; 0 when none.
  uint64_t nextDeadlineNs() const;
  ControllerState state() const;
  uint64_t suppressedTriggers() const;

 private:
  void beginCapture(const Trigger &t, uint64_t windowFrom);
  std::optional<Incident> finalize(EndReason reason, uint64_t now);
  bool allEnded() const;
  uint64_t lastEnd() const;
  void emit(std::optional<Incident> incident);

  Recorder &recorder_;
  std::atomic<uint32_t> screen_{0};
  const IncidentConfig config_;
  Sink sink_;
  const uint32_t captureLabel_;
  const uint32_t finalizeLabel_;

  mutable std::mutex mu_;
  ControllerState state_ = ControllerState::Disabled;
  ControllerState stateBeforeSuspend_ = ControllerState::Armed;
  std::optional<Incident> current_;
  uint64_t nextId_ = 1;
  uint64_t cooldownUntil_ = 0;
  uint64_t lastCaptureEnd_ = 0;
  uint64_t suppressed_ = 0;
};

} // namespace rnti
