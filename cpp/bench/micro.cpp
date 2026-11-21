#include "RntiPlatform.h"
#include "Recorder.h"

#include <algorithm>
#include <cstdio>
#include <thread>
#include <vector>

using namespace rnti;

namespace {

constexpr int kOps = 2'000'000;
constexpr int kRounds = 7;

template <typename F>
double nsPerOp(F &&fn) {
  std::vector<double> rounds;
  for (int r = 0; r < kRounds; ++r) {
    const uint64_t t0 = nowNs();
    for (int i = 0; i < kOps; ++i) fn(i);
    rounds.push_back(static_cast<double>(nowNs() - t0) / kOps);
  }
  std::sort(rounds.begin(), rounds.end());
  return rounds[kRounds / 2];
}

} // namespace

int main() {
  Recorder rec;
  const uint32_t label = rec.intern("bench.label");

  std::printf("clock read:          %6.1f ns\n", nsPerOp([](int) { (void)nowNs(); }));
  std::printf("instant:             %6.1f ns\n",
              nsPerOp([&](int i) { rec.instant(Category::User, label, static_cast<uint64_t>(i)); }));
  std::printf("complete:            %6.1f ns\n",
              nsPerOp([&](int i) { rec.complete(Category::Js, label, static_cast<uint64_t>(i), 10); }));
  std::printf("begin+end pair:      %6.1f ns\n", nsPerOp([&](int) {
                rec.end(Category::Js, label, rec.begin(Category::Js, label));
              }));
  rec.setEnabled(false);
  std::printf("disabled begin+end:  %6.1f ns\n", nsPerOp([&](int) {
                rec.end(Category::Js, label, rec.begin(Category::Js, label));
              }));
  rec.setEnabled(true);

  for (int threads : {2, 4}) {
    std::vector<double> out(threads);
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) {
      pool.emplace_back([&, t] {
        out[t] = nsPerOp([&](int i) { rec.complete(Category::Native, label, static_cast<uint64_t>(i), 10); });
      });
    }
    for (auto &th : pool) th.join();
    std::printf("complete, %d threads: %6.1f ns (worst thread)\n", threads, *std::max_element(out.begin(), out.end()));
  }

  for (int i = 0; i < 4096; ++i) rec.complete(Category::Js, label, nowNs(), 10);
  const uint64_t t0 = nowNs();
  Snapshot s = rec.snapshot(0, UINT64_MAX);
  std::printf("snapshot:            %6.1f us (%zu events, %zu rings)\n", (nowNs() - t0) / 1000.0, s.events.size(),
              s.rings.size());
  std::printf("ring memory:         %u slots/thread x %zu B = %zu KiB per thread\n", rec.config().eventsPerThread,
              EventRing::kSlotBytes, rec.config().eventsPerThread * EventRing::kSlotBytes / 1024);
  return 0;
}
