#pragma once

#include "IncidentController.h"
#include "IncidentWriter.h"
#include "Recorder.h"
#include "Watchdog.h"

#include <atomic>
#include <functional>
#include <memory>

namespace rnti {

struct InspectorConfig {
  RecorderConfig recorder;
  IncidentConfig incident;
  WriterConfig writer;
  ExportMetadata meta;
  uint64_t probeIntervalNs = 100 * kMs;
  uint64_t jsThresholdNs = 200 * kMs;
  uint64_t mainThresholdNs = 200 * kMs;
};

// One per process, never destroyed: app and RN threads may record at any time.
class Inspector {
 public:
  using Poster = std::function<bool(std::function<void()>)>;

  // Returns the existing instance (config ignored) if already installed; `created` reports which.
  static Inspector &install(InspectorConfig config, Poster js, Poster main, IncidentWriter::SavedCallback onSaved,
                            bool &created);
  static Inspector *shared() noexcept { return instance_.load(std::memory_order_acquire); }

  void start(bool foreground);
  void stop();
  void setForeground(bool foreground);
  // Call from the RN JS thread so a stall before the first probe ack still has a thread.
  void markJsThread();
  void reportLongTask(uint64_t startNs, uint64_t durNs);

  Recorder &recorder() noexcept { return recorder_; }
  IncidentController &controller() noexcept { return controller_; }

 private:
  Inspector(InspectorConfig config, Poster js, Poster main, IncidentWriter::SavedCallback onSaved);
  Poster namedPoster(Poster post, int index, const char *threadName);

  static std::atomic<Inspector *> instance_;

  Recorder recorder_;
  std::unique_ptr<IncidentWriter> writer_;
  IncidentController controller_;
  Watchdog watchdog_;
  std::shared_ptr<std::atomic<bool>> named_[2];
  uint32_t longTaskLabel_ = 0;
};

} // namespace rnti
