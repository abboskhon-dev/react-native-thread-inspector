#include "Analysis.h"
#include "EventRing.h"
#include "Export.h"
#include "IncidentController.h"
#include "IncidentWriter.h"
#include "Platform.h"
#include "Recorder.h"
#include "Watchdog.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

using namespace rnti;
using namespace std::chrono_literals;

static int gFailures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++gFailures;                                                           \
    }                                                                        \
  } while (0)

// Serial task queue standing in for the RN JS thread.
class TaskThread {
 public:
  TaskThread() : thread_([this] { run(); }) {}
  ~TaskThread() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      stop_ = true;
    }
    cv_.notify_one();
    thread_.join();
  }
  bool post(std::function<void()> fn) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      tasks_.push_back(std::move(fn));
    }
    cv_.notify_one();
    return true;
  }
  void sync() {
    std::promise<void> p;
    auto f = p.get_future();
    post([&] { p.set_value(); });
    f.wait();
  }

 private:
  void run() {
    while (true) {
      std::function<void()> fn;
      {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return stop_ || !tasks_.empty(); });
        if (tasks_.empty()) return;
        fn = std::move(tasks_.front());
        tasks_.pop_front();
      }
      fn();
    }
  }
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> tasks_;
  bool stop_ = false;
  std::thread thread_;
};

struct IncidentInbox {
  std::mutex mu;
  std::condition_variable cv;
  std::vector<Incident> items;

  IncidentController::Sink sink() {
    return [this](Incident &&i) {
      {
        std::lock_guard<std::mutex> lk(mu);
        items.push_back(std::move(i));
      }
      cv.notify_all();
    };
  }
  bool waitFor(size_t n, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(mu);
    return cv.wait_for(lk, timeout, [&] { return items.size() >= n; });
  }
};

static uint64_t checksum(const Event &e) { return e.ts * 0x9E3779B97F4A7C15ull ^ e.spanId ^ (e.arg0 << 7); }

static void testRingBasic() {
  EventRing ring(8);
  for (uint64_t i = 1; i <= 5; ++i) ring.push({.ts = i * 10, .arg0 = i});
  std::vector<SnapshotEvent> out;
  auto st = ring.collect(0, UINT64_MAX, 1, out);
  CHECK(out.size() == 5);
  CHECK(st.overwritten == 0);
  CHECK(st.oldestNs == 10);
}

static void testRingWrap() {
  EventRing ring(8);
  for (uint64_t i = 1; i <= 20; ++i) ring.push({.ts = i * 10, .arg0 = i});
  std::vector<SnapshotEvent> out;
  auto st = ring.collect(0, UINT64_MAX, 1, out);
  CHECK(out.size() == 8);
  CHECK(st.overwritten == 12);
  CHECK(st.oldestNs == 130);
  CHECK(out.front().event.arg0 == 13);
}

static void testRingConcurrentSnapshots() {
  EventRing ring(256);
  std::atomic<bool> done{false};
  std::atomic<uint64_t> bad{0}, seen{0};
  std::thread writer([&] {
    for (uint64_t i = 1; i <= 300000; ++i) {
      Event e{.ts = i, .spanId = i * 3, .arg0 = i};
      e.arg1 = checksum(e);
      ring.push(e);
    }
    done = true;
  });
  std::vector<std::thread> readers;
  for (int r = 0; r < 2; ++r) {
    readers.emplace_back([&] {
      std::vector<SnapshotEvent> out;
      while (!done) {
        out.clear();
        ring.collect(0, UINT64_MAX, 1, out);
        uint64_t last = 0;
        for (const auto &se : out) {
          if (se.event.arg1 != checksum(se.event) || se.position < last) ++bad;
          last = se.position;
        }
        seen += out.size();
      }
    });
  }
  writer.join();
  for (auto &t : readers) t.join();
  CHECK(bad == 0);
  CHECK(seen > 0);
}

static void testRecorderThreads() {
  Recorder rec({.eventsPerThread = 512, .maxThreads = 4});
  const uint32_t label = rec.intern("work");
  // More sequential threads than rings: exited threads must hand their ring back.
  for (int round = 0; round < 3; ++round) {
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
      threads.emplace_back([&] {
        for (int i = 0; i < 50; ++i) rec.end(Category::Native, label, rec.begin(Category::Native, label));
      });
    }
    for (auto &t : threads) t.join();
  }
  auto snap = rec.snapshot(0, UINT64_MAX);
  CHECK(rec.droppedNoRing() == 0);
  uint64_t overwritten = 0;
  for (const auto &r : snap.rings) overwritten += r.stats.overwritten;
  CHECK(snap.events.size() + overwritten == 3 * 4 * 50 * 2);
}

static void testRecorderOverflow() {
  Recorder rec({.eventsPerThread = 64, .maxThreads = 1});
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  for (int t = 0; t < 2; ++t) {
    threads.emplace_back([&] {
      rec.instant(Category::User, 0);
      ++ready;
      while (!go) std::this_thread::yield();
    });
  }
  while (ready < 2) std::this_thread::yield();
  go = true;
  for (auto &t : threads) t.join();
  CHECK(rec.droppedNoRing() == 1);
}

struct Rig {
  Recorder rec;
  IncidentInbox inbox;
  IncidentController controller;
  Watchdog watchdog;
  TaskThread js;

  explicit Rig(IncidentConfig ic, RecorderConfig rc = {})
      : rec(rc), controller(rec, ic, inbox.sink()), watchdog(rec, controller) {
    js.post([this] { rec.nameCurrentThread("js"); });
    watchdog.addChannel({.name = "js",
                         .source = TriggerSource::JsWatchdog,
                         .post = [this](std::function<void()> ack) { return js.post(std::move(ack)); },
                         .intervalNs = 20 * kMs,
                         .thresholdNs = 100 * kMs});
  }
};

static IncidentConfig fastConfig() {
  return {.prehistoryNs = 2000 * kMs, .tailNs = 100 * kMs, .maxIncidentNs = 3000 * kMs, .cooldownNs = 500 * kMs,
          .longTaskThresholdNs = 200 * kMs};
}

static void testJsStallDetectedWhileBlocked() {
  Rig rig(fastConfig());
  const uint32_t before = rig.rec.intern("before");
  const uint32_t heavy = rig.rec.intern("Storage.readSync");
  rig.watchdog.start();
  std::this_thread::sleep_for(100ms);

  std::atomic<bool> detectedWhileBlocked{false};
  rig.js.post([&] {
    rig.rec.instant(Category::User, before);
    const uint64_t span = rig.rec.begin(Category::Native, heavy);
    const uint64_t start = nowNs();
    while (nowNs() - start < 300 * kMs) {
      if (rig.controller.state() == ControllerState::Capturing) detectedWhileBlocked = true;
      std::this_thread::sleep_for(5ms);
    }
    rig.rec.end(Category::Native, heavy, span);
  });

  CHECK(rig.inbox.waitFor(1, 3s));
  rig.watchdog.stop();
  CHECK(detectedWhileBlocked);
  if (rig.inbox.items.empty()) return;
  const Incident &inc = rig.inbox.items[0];
  CHECK(inc.endReason == EndReason::Recovered);
  CHECK(inc.triggers.size() == 1);
  const Trigger &t = inc.triggers[0];
  CHECK(t.source == TriggerSource::JsWatchdog);
  CHECK(t.endNs != 0);
  CHECK(t.tid != 0);
  CHECK(t.endNs - t.onsetNs >= 150 * kMs);

  auto trace = resolve(inc);
  auto analysis = analyze(inc, trace);
  CHECK(analysis.size() == 1);
  bool found = false;
  for (const auto &f : analysis[0].findings) {
    if (f.span.label == heavy && f.relation == Relation::DirectContainment && f.overlapNs >= 150 * kMs) found = true;
  }
  CHECK(found);
}

static void testLongTaskMergesIntoIncident() {
  Rig rig(fastConfig());
  rig.watchdog.start();
  std::this_thread::sleep_for(60ms);
  uint64_t taskStart = 0, taskEnd = 0;
  rig.js.post([&] {
    taskStart = nowNs();
    std::this_thread::sleep_for(300ms);
    taskEnd = nowNs();
  });
  rig.js.post([&] { rig.controller.longTaskCompleted(currentTid(), taskStart, taskEnd - taskStart, nowNs()); });
  CHECK(rig.inbox.waitFor(1, 3s));
  rig.watchdog.stop();
  if (rig.inbox.items.empty()) return;
  const Incident &inc = rig.inbox.items[0];
  CHECK(inc.triggers.size() == 2);
  bool hasLongTask = false;
  for (const auto &t : inc.triggers) {
    if (t.source == TriggerSource::JsLongTask) hasLongTask = !t.onsetApproximate && t.onsetNs == taskStart;
  }
  CHECK(hasLongTask);
  CHECK(rig.inbox.items.size() == 1);
}

static void testBackgroundSuppressesIncidents() {
  Rig rig(fastConfig());
  rig.watchdog.start();
  std::this_thread::sleep_for(60ms);
  rig.watchdog.setForeground(false);
  std::this_thread::sleep_for(30ms);
  rig.js.post([] { std::this_thread::sleep_for(300ms); });
  CHECK(!rig.inbox.waitFor(1, 600ms));
  CHECK(rig.controller.state() == ControllerState::Suspended);
  rig.watchdog.setForeground(true);
  std::this_thread::sleep_for(60ms);
  CHECK(rig.controller.state() == ControllerState::Armed);
  rig.watchdog.stop();
}

static void testStallBeforeFirstAck() {
  Rig rig(fastConfig());
  const uint32_t heavy = rig.rec.intern("startup.work");
  rig.js.post([&] {
    const uint64_t span = rig.rec.begin(Category::Js, heavy);
    std::this_thread::sleep_for(300ms);
    rig.rec.end(Category::Js, heavy, span);
  });
  rig.watchdog.start();
  CHECK(rig.inbox.waitFor(1, 3s));
  rig.watchdog.stop();
  if (rig.inbox.items.empty()) return;
  const Incident &inc = rig.inbox.items[0];
  CHECK(inc.triggers.size() == 1);
  CHECK(inc.triggers[0].tid != 0);
  auto trace = resolve(inc);
  auto analysis = analyze(inc, trace);
  bool contained = false;
  for (const auto &f : analysis[0].findings) contained |= f.span.label == heavy && f.relation == Relation::DirectContainment;
  CHECK(contained);
}

static void testBackgroundMidStallEndsLifecycle() {
  Rig rig(fastConfig());
  rig.watchdog.start();
  std::this_thread::sleep_for(60ms);
  rig.js.post([] { std::this_thread::sleep_for(600ms); });
  std::this_thread::sleep_for(300ms);
  CHECK(rig.controller.state() == ControllerState::Capturing);
  rig.watchdog.setForeground(false);
  CHECK(rig.inbox.waitFor(1, 1s));
  rig.js.sync();
  rig.watchdog.setForeground(true);
  std::this_thread::sleep_for(100ms);
  rig.watchdog.stop();
  CHECK(rig.inbox.items.size() == 1);
  if (rig.inbox.items.empty()) return;
  const Incident &inc = rig.inbox.items[0];
  CHECK(inc.endReason == EndReason::Lifecycle);
  CHECK(inc.triggers[0].endNs == 0);
}

static void testUnrecoveredStartupHangUsesHint() {
  Rig rig(fastConfig());
  uint32_t jsTid = 0;
  rig.js.post([&] { jsTid = currentTid(); });
  rig.js.sync();
  rig.js.post([] { std::this_thread::sleep_for(500ms); });
  rig.watchdog.hintThread(0, jsTid);
  rig.watchdog.start();
  std::this_thread::sleep_for(300ms);
  rig.watchdog.setForeground(false);
  CHECK(rig.inbox.waitFor(1, 1s));
  rig.js.sync();
  rig.watchdog.stop();
  if (rig.inbox.items.empty()) return;
  CHECK(rig.inbox.items[0].triggers[0].endNs == 0);
  CHECK(rig.inbox.items[0].triggers[0].tid == jsTid);
}

static void testPrehistoryPinnedDuringFlood() {
  Rig rig(fastConfig(), {.eventsPerThread = 64, .maxThreads = 8});
  const uint32_t marker = rig.rec.intern("flood.marker");
  const uint32_t noise = rig.rec.intern("flood.noise");
  std::atomic<bool> stopFlood{false};
  std::atomic<bool> markerWritten{false};
  std::thread flooder([&] {
    rig.rec.instant(Category::User, marker);
    markerWritten = true;
    // Flood only after detection: the live ring loses the marker, the pinned prehistory must not.
    while (!stopFlood && rig.controller.state() != ControllerState::Capturing) std::this_thread::sleep_for(1ms);
    for (int i = 0; i < 1000 && !stopFlood; ++i) rig.rec.instant(Category::User, noise);
  });
  while (!markerWritten) std::this_thread::yield();
  rig.watchdog.start();
  std::this_thread::sleep_for(20ms);
  rig.js.post([] { std::this_thread::sleep_for(500ms); });
  CHECK(rig.inbox.waitFor(1, 3s));
  stopFlood = true;
  flooder.join();
  rig.watchdog.stop();
  if (rig.inbox.items.empty()) return;
  bool hasMarker = false;
  for (const auto &e : rig.inbox.items[0].snapshot.events) hasMarker |= e.event.label == marker;
  CHECK(hasMarker);
}

static void testUnrecoveredHangHitsMaxDuration() {
  auto cfg = fastConfig();
  cfg.maxIncidentNs = 200 * kMs;
  Rig rig(cfg);
  rig.watchdog.start();
  std::this_thread::sleep_for(60ms);
  rig.js.post([] { std::this_thread::sleep_for(600ms); });
  CHECK(rig.inbox.waitFor(1, 2s));
  rig.watchdog.stop();
  if (rig.inbox.items.empty()) return;
  const Incident &inc = rig.inbox.items[0];
  CHECK(inc.endReason == EndReason::MaxDuration);
  CHECK(inc.triggers[0].endNs == 0);
  auto analysis = analyze(inc, resolve(inc));
  CHECK(!analysis.empty() && !analysis[0].recovered);
}

static void testExportAndWriter() {
  Rig rig(fastConfig());
  const uint32_t heavy = rig.rec.intern("Storage.readSync");
  const uint32_t worker = rig.rec.intern("db.query");
  rig.watchdog.start();
  std::this_thread::sleep_for(60ms);

  const auto dir = std::filesystem::temp_directory_path() / ("rnti-test-" + std::to_string(nowNs()));
  std::string savedPath;
  std::mutex savedMu;
  IncidentWriter writer(rig.rec, {.directory = dir.string()}, {.sessionId = "test", .fields = {{"platform", "host"}}},
                        [&](uint64_t, const std::string &p) {
                          std::lock_guard<std::mutex> lk(savedMu);
                          savedPath = p;
                        });

  const uint64_t flow = rig.rec.newSpanId();
  rig.js.post([&] {
    const uint64_t span = rig.rec.begin(Category::Native, heavy, flow);
    std::thread([&] { rig.rec.complete(Category::Native, worker, nowNs(), 50 * kMs, 0, flow); }).join();
    std::this_thread::sleep_for(300ms);
    rig.rec.end(Category::Native, heavy, span);
  });
  CHECK(rig.inbox.waitFor(1, 3s));
  rig.watchdog.stop();
  if (rig.inbox.items.empty()) return;
  CHECK(writer.enqueue(std::move(rig.inbox.items[0])));
  writer.flush();
  CHECK(writer.failed() == 0);

  std::string path;
  {
    std::lock_guard<std::mutex> lk(savedMu);
    path = savedPath;
  }
  CHECK(!path.empty());
  auto read = [](const std::filesystem::path &p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
  };
  const std::string trace = read(std::filesystem::path(path) / "trace.json");
  const std::string report = read(std::filesystem::path(path) / "incident.json");
  CHECK(trace.rfind("{\"traceEvents\":[", 0) == 0);
  CHECK(trace.find("Storage.readSync") != std::string::npos);
  CHECK(trace.find("\"ph\":\"s\"") != std::string::npos);
  CHECK(report.find("\"direct_containment\"") != std::string::npos);
  CHECK(report.find("\"explicit_flow\"") != std::string::npos);
  std::printf("  artifact: %s\n", path.c_str());
}

int main() {
  struct Case {
    const char *name;
    void (*fn)();
  } cases[] = {
      {"ring basic", testRingBasic},
      {"ring wrap", testRingWrap},
      {"ring concurrent snapshots", testRingConcurrentSnapshots},
      {"recorder thread rings reuse", testRecorderThreads},
      {"recorder overflow counted", testRecorderOverflow},
      {"js stall detected while blocked", testJsStallDetectedWhileBlocked},
      {"long task merges into incident", testLongTaskMergesIntoIncident},
      {"background suppresses incidents", testBackgroundSuppressesIncidents},
      {"stall before first ack names thread", testStallBeforeFirstAck},
      {"background mid-stall ends lifecycle", testBackgroundMidStallEndsLifecycle},
      {"unrecovered startup hang uses thread hint", testUnrecoveredStartupHangUsesHint},
      {"prehistory pinned during flood", testPrehistoryPinnedDuringFlood},
      {"unrecovered hang hits max duration", testUnrecoveredHangHitsMaxDuration},
      {"export and writer", testExportAndWriter},
  };
  for (const auto &c : cases) {
    const int before = gFailures;
    std::printf("%s\n", c.name);
    c.fn();
    std::printf("  %s\n", gFailures == before ? "ok" : "FAILED");
  }
  std::printf("%s (%d failures)\n", gFailures ? "FAILED" : "PASSED", gFailures);
  return gFailures ? 1 : 0;
}
