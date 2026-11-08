#include "Inspector.h"

#include <mutex>

namespace rnti {

std::atomic<Inspector *> Inspector::instance_{nullptr};

namespace {
std::mutex gInstallMu;
}

Inspector &Inspector::install(InspectorConfig config, Poster js, Poster main, IncidentWriter::SavedCallback onSaved,
                              bool &created) {
  std::lock_guard<std::mutex> lk(gInstallMu);
  if (Inspector *existing = shared()) {
    created = false;
    return *existing;
  }
  auto *ins = new Inspector(std::move(config), std::move(js), std::move(main), std::move(onSaved));
  instance_.store(ins, std::memory_order_release);
  created = true;
  return *ins;
}

Inspector::Inspector(InspectorConfig config, Poster js, Poster main, IncidentWriter::SavedCallback onSaved)
    : recorder_(config.recorder),
      writer_(std::make_unique<IncidentWriter>(recorder_, config.writer, config.meta, std::move(onSaved))),
      controller_(recorder_, config.incident, [this](Incident &&i) { writer_->enqueue(std::move(i)); }),
      watchdog_(recorder_, controller_),
      turboModules_(recorder_),
      turboModulesWanted_(config.turboModules) {
  named_[0] = std::make_shared<std::atomic<bool>>(false);
  named_[1] = std::make_shared<std::atomic<bool>>(false);
  longTaskLabel_ = recorder_.intern("js.long_task");
  foregroundLabel_ = recorder_.intern("app.foreground");
  backgroundLabel_ = recorder_.intern("app.background");
  watchdog_.addChannel({.name = "js", .source = TriggerSource::JsWatchdog, .post = namedPoster(std::move(js), 0, "RN JS"),
                        .intervalNs = config.probeIntervalNs, .thresholdNs = config.jsThresholdNs});
  watchdog_.addChannel({.name = "main", .source = TriggerSource::MainWatchdog,
                        .post = namedPoster(std::move(main), 1, "main"), .intervalNs = config.probeIntervalNs,
                        .thresholdNs = config.mainThresholdNs});
}

Inspector::Poster Inspector::namedPoster(Poster post, int index, const char *threadName) {
  auto named = named_[index];
  return [this, post = std::move(post), named, threadName](std::function<void()> ack) {
    return post([this, named, threadName, ack = std::move(ack)] {
      if (!named->exchange(true)) recorder_.nameCurrentThread(threadName);
      ack();
    });
  };
}

void Inspector::start(bool foreground) {
  recorder_.setEnabled(true);
  turboModules_.setEnabled(turboModulesWanted_);
  watchdog_.setForeground(foreground);
  watchdog_.start();
}

void Inspector::stop() {
  watchdog_.stop();
  turboModules_.setEnabled(false);
  controller_.disable(nowNs());
  recorder_.setEnabled(false);
}

void Inspector::setForeground(bool foreground) {
  recorder_.instant(Category::Lifecycle, foreground ? foregroundLabel_ : backgroundLabel_);
  watchdog_.setForeground(foreground);
}

void Inspector::markScreen(uint32_t label) {
  recorder_.instant(Category::Navigation, label);
  controller_.setScreen(label);
}

void Inspector::markJsThread() {
  if (!named_[0]->exchange(true)) recorder_.nameCurrentThread("RN JS");
  watchdog_.hintThread(0, currentTid());
}

void Inspector::reportLongTask(uint64_t startNs, uint64_t durNs) {
  if (startNs == 0 || durNs == 0) return;
  recorder_.complete(Category::Js, longTaskLabel_, startNs, durNs, flags::Approximate | flags::Envelope);
  controller_.longTaskCompleted(currentTid(), startNs, durNs, nowNs());
}

} // namespace rnti
