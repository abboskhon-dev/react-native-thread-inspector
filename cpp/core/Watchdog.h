#pragma once

#include "IncidentController.h"
#include "Recorder.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rnti {

struct ProbeChannelConfig {
  std::string name;
  TriggerSource source = TriggerSource::JsWatchdog;
  // Enqueue `ack` on the observed thread; return false if it could not be scheduled.
  std::function<bool(std::function<void()> ack)> post;
  uint64_t intervalNs = 100 * kMs;
  uint64_t thresholdNs = 200 * kMs;
};

struct WatchdogConfig {
  uint64_t lateWakeThresholdNs = 50 * kMs;
};

// Detects unanswered probes from its own thread; never waits on the observed threads.
class Watchdog {
 public:
  Watchdog(Recorder &recorder, IncidentController &controller, WatchdogConfig config = {});
  ~Watchdog();

  void addChannel(ProbeChannelConfig channel); // before start()
  void start();
  void stop();
  void setForeground(bool foreground);

 private:
  struct Wake {
    std::mutex mu;
    std::condition_variable cv;
    bool pending = false;
    bool stop = false;
  };

  struct AckState {
    std::atomic<uint64_t> expectedSeq{0};
    std::atomic<uint64_t> ackedSeq{0};
    std::atomic<uint64_t> ackNs{0};
    std::atomic<uint32_t> ackTid{0};
  };

  struct Channel {
    ProbeChannelConfig config;
    std::shared_ptr<AckState> ack = std::make_shared<AckState>();
    uint32_t probeLabel = 0;
    uint32_t stallLabel = 0;
    bool outstanding = false;
    bool stalled = false;
    uint64_t seq = 0;
    uint64_t enqueueNs = 0;
    uint64_t lastAckNs = 0;
    uint32_t lastAckTid = 0;
    uint64_t nextProbeNs = 0;
  };

  void run();
  void service(Channel &ch, uint64_t now);
  void resetChannels(uint64_t now);
  uint64_t nextWakeNs(uint64_t now) const;

  Recorder &recorder_;
  IncidentController &controller_;
  const WatchdogConfig config_;
  const uint32_t lateLabel_;
  const uint32_t postFailLabel_;
  std::shared_ptr<Wake> wake_ = std::make_shared<Wake>();
  std::vector<Channel> channels_;
  std::atomic<bool> foreground_{true};
  std::thread thread_;
};

} // namespace rnti
