#include "ThreadCpu.h"

#include "JsonWriter.h"
#include "Platform.h"

#include <algorithm>
#include <ctime>
#include <unordered_map>

#if defined(__APPLE__)
#include <mach/mach.h>
#else
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#endif

namespace rnti {

namespace {

uint64_t toNs(const timespec &ts) {
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

#if defined(__APPLE__)

void readThreads(std::vector<ThreadCpu> &out) {
  thread_act_array_t threads = nullptr;
  mach_msg_type_number_t count = 0;
  if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) return;
  for (mach_msg_type_number_t i = 0; i < count; ++i) {
    thread_extended_info_data_t ext{};
    mach_msg_type_number_t extCount = THREAD_EXTENDED_INFO_COUNT;
    thread_identifier_info_data_t ident{};
    mach_msg_type_number_t identCount = THREAD_IDENTIFIER_INFO_COUNT;
    if (thread_info(threads[i], THREAD_EXTENDED_INFO, reinterpret_cast<thread_info_t>(&ext), &extCount) == KERN_SUCCESS &&
        thread_info(threads[i], THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(&ident), &identCount) == KERN_SUCCESS) {
      ext.pth_name[sizeof ext.pth_name - 1] = '\0';
      // Truncated like currentTid(), so the two can be compared.
      out.push_back({static_cast<uint32_t>(ident.thread_id), ext.pth_name, ext.pth_user_time + ext.pth_system_time});
    }
    mach_port_deallocate(mach_task_self(), threads[i]);
  }
  vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), count * sizeof(thread_act_t));
}

#else

void readThreads(std::vector<ThreadCpu> &out) {
  DIR *dir = opendir("/proc/self/task");
  if (!dir) return;
  while (const dirent *e = readdir(dir)) {
    char *end = nullptr;
    const unsigned long tid = std::strtoul(e->d_name, &end, 10);
    if (end == e->d_name || *end != '\0') continue;
    // The kernel's per-thread CPU clock id (CPUCLOCK_SCHED | CPUCLOCK_PERTHREAD_MASK), as pthread_getcpuclockid builds it.
    const auto clock = static_cast<clockid_t>((~static_cast<unsigned int>(tid) << 3) | 6u);
    timespec ts{};
    if (clock_gettime(clock, &ts) != 0) continue;
    const std::string dirPath = "/proc/self/task/" + std::string(e->d_name);
    ThreadCpu t{static_cast<uint32_t>(tid), {}, toNs(ts)};
    std::getline(std::ifstream(dirPath + "/comm"), t.name);
    std::ifstream status(dirPath + "/status");
    static constexpr char kVoluntary[] = "voluntary_ctxt_switches:";
    static constexpr char kInvoluntary[] = "nonvoluntary_ctxt_switches:";
    for (std::string line; std::getline(status, line);) {
      if (line.rfind(kVoluntary, 0) == 0) t.voluntarySwitches = std::strtoll(line.c_str() + sizeof kVoluntary - 1, nullptr, 10);
      else if (line.rfind(kInvoluntary, 0) == 0) t.involuntarySwitches = std::strtoll(line.c_str() + sizeof kInvoluntary - 1, nullptr, 10);
    }
    out.push_back(std::move(t));
  }
  closedir(dir);
}

#endif

} // namespace

CpuSnapshot cpuSnapshot() {
  CpuSnapshot snap;
  snap.monoNs = nowNs();
  timespec ts{};
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0) snap.processCpuNs = toNs(ts);
  readThreads(snap.threads);
  return snap;
}

std::string cpuDeltaJson(const CpuSnapshot &start, const CpuSnapshot &stop) {
  std::unordered_map<uint32_t, const ThreadCpu *> before;
  for (const auto &t : start.threads) before[t.tid] = &t;
  std::vector<ThreadCpu> used;
  for (const auto &t : stop.threads) {
    const auto it = before.find(t.tid);
    // A thread that started in between, or a reused id, counts from zero.
    const ThreadCpu *b = it != before.end() && it->second->cpuNs <= t.cpuNs ? it->second : nullptr;
    const uint64_t base = b ? b->cpuNs : 0;
    if (t.cpuNs <= base) continue;
    const auto delta = [&](int64_t ThreadCpu::*count) -> int64_t {
      const int64_t now = t.*count, then = b ? b->*count : 0;
      return now < 0 || then < 0 ? -1 : std::max<int64_t>(now - then, 0);
    };
    used.push_back({t.tid, t.name, t.cpuNs - base, delta(&ThreadCpu::voluntarySwitches),
                    delta(&ThreadCpu::involuntarySwitches)});
  }
  std::sort(used.begin(), used.end(), [](const ThreadCpu &a, const ThreadCpu &b) { return a.cpuNs > b.cpuNs; });

  JsonWriter w;
  w.beginObject();
  w.key("wall_ns").u64(stop.monoNs > start.monoNs ? stop.monoNs - start.monoNs : 0);
  w.key("process_ns").u64(stop.processCpuNs > start.processCpuNs ? stop.processCpuNs - start.processCpuNs : 0);
  w.key("threads").beginArray();
  for (const auto &t : used) {
    w.beginObject();
    w.key("tid").u64(t.tid);
    w.key("name").str(t.name);
    w.key("cpu_ns").u64(t.cpuNs);
    if (t.voluntarySwitches >= 0 && t.involuntarySwitches >= 0) {
      w.key("voluntary_switches").u64(static_cast<uint64_t>(t.voluntarySwitches));
      w.key("involuntary_switches").u64(static_cast<uint64_t>(t.involuntarySwitches));
    }
    w.endObject();
  }
  w.endArray();
  w.endObject();
  return w.take();
}

} // namespace rnti
