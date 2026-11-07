#pragma once

#include "Export.h"
#include "IncidentController.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace rnti {

struct WriterConfig {
  std::string directory;
  uint32_t maxIncidents = 20;
  uint32_t maxPending = 2;
};

// Persists incidents on its own thread: <dir>/incident-<session>-<id>/{trace.json,incident.json}.
class IncidentWriter {
 public:
  using SavedCallback = std::function<void(uint64_t id, const std::string &path)>;

  IncidentWriter(const Recorder &recorder, WriterConfig config, ExportMetadata meta, SavedCallback onSaved = {});
  ~IncidentWriter();

  bool enqueue(Incident &&incident);
  void flush();
  uint64_t dropped() const;
  uint64_t failed() const;

 private:
  void run();
  void write(const Incident &incident);
  void prune();

  const Recorder &recorder_;
  const WriterConfig config_;
  const ExportMetadata meta_;
  SavedCallback onSaved_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::condition_variable idle_;
  std::deque<Incident> queue_;
  bool busy_ = false;
  bool stop_ = false;
  uint64_t dropped_ = 0;
  uint64_t failed_ = 0;
  std::thread thread_;
};

} // namespace rnti
