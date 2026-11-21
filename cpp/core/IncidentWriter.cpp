#include "IncidentWriter.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace rnti {

namespace {

bool writeFile(const fs::path &path, const std::string &content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  out.flush();
  return static_cast<bool>(out);
}

} // namespace

IncidentWriter::IncidentWriter(const Recorder &recorder, WriterConfig config, ExportMetadata meta,
                               SavedCallback onSaved)
    : recorder_(recorder), config_(std::move(config)), meta_(std::move(meta)), onSaved_(std::move(onSaved)) {
  thread_ = std::thread([this] { run(); });
}

IncidentWriter::~IncidentWriter() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    stop_ = true;
  }
  cv_.notify_one();
  thread_.join();
}

bool IncidentWriter::enqueue(Incident &&incident) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (queue_.size() >= config_.maxPending) {
      ++dropped_;
      return false;
    }
    queue_.push_back(std::move(incident));
  }
  cv_.notify_one();
  return true;
}

void IncidentWriter::flush() {
  std::unique_lock<std::mutex> lk(mu_);
  idle_.wait(lk, [this] { return queue_.empty() && !busy_; });
}

uint64_t IncidentWriter::dropped() const {
  std::lock_guard<std::mutex> lk(mu_);
  return dropped_;
}

uint64_t IncidentWriter::failed() const {
  std::lock_guard<std::mutex> lk(mu_);
  return failed_;
}

void IncidentWriter::run() {
  while (true) {
    Incident incident;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
      if (queue_.empty()) return;
      incident = std::move(queue_.front());
      queue_.pop_front();
      busy_ = true;
    }
    write(incident);
    {
      std::lock_guard<std::mutex> lk(mu_);
      busy_ = false;
    }
    idle_.notify_all();
  }
}

void IncidentWriter::write(const Incident &incident) {
  const std::string name = "incident-" + meta_.sessionId + "-" + std::to_string(incident.id);
  const fs::path dir(config_.directory);
  const fs::path tmp = dir / ("." + name + ".tmp");
  const fs::path final = dir / name;
  std::error_code ec;
  bool ok = false;
  try {
    const ResolvedTrace trace = resolve(incident);
    const auto analysis = analyze(incident, trace, jsStacks_.load(std::memory_order_relaxed));
    fs::create_directories(dir, ec);
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    ok = !ec && writeFile(tmp / "trace.json", chromeTraceJson(incident, trace, recorder_, meta_)) &&
         writeFile(tmp / "incident.json", incidentReportJson(incident, trace, analysis, recorder_, meta_));
    if (ok) {
      fs::remove_all(final, ec);
      fs::rename(tmp, final, ec);
      ok = !ec;
    }
  } catch (...) {
    ok = false;
  }
  if (!ok) {
    fs::remove_all(tmp, ec);
    std::lock_guard<std::mutex> lk(mu_);
    ++failed_;
    return;
  }
  prune();
  if (onSaved_) onSaved_(incident.id, final.string());
}

void IncidentWriter::prune() {
  std::error_code ec;
  std::vector<fs::directory_entry> entries;
  for (const auto &e : fs::directory_iterator(config_.directory, ec)) {
    if (e.is_directory(ec) && e.path().filename().string().rfind("incident-", 0) == 0) entries.push_back(e);
  }
  if (entries.size() <= config_.maxIncidents) return;
  std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
    std::error_code e1, e2;
    return a.last_write_time(e1) < b.last_write_time(e2);
  });
  for (size_t i = 0; i + config_.maxIncidents < entries.size(); ++i) fs::remove_all(entries[i].path(), ec);
}

} // namespace rnti
