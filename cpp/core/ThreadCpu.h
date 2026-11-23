#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rnti {

struct ThreadCpu {
  uint32_t tid = 0;
  std::string name;
  uint64_t cpuNs = 0;
  // Context switches so far; -1 where the platform has no per-thread count (iOS).
  int64_t voluntarySwitches = -1;
  int64_t involuntarySwitches = -1;
};

struct CpuSnapshot {
  uint64_t monoNs = 0;
  uint64_t processCpuNs = 0;
  std::vector<ThreadCpu> threads;
};

// Opaque id for reading the calling thread's CPU clock from another thread; 0 = unavailable.
uint64_t currentCpuHandle() noexcept;
// CPU time used so far by the thread behind `handle`; false if it cannot be read.
bool threadCpuNs(uint64_t handle, uint64_t &out) noexcept;

// CPU time used so far by the process and by each thread that is alive now.
CpuSnapshot cpuSnapshot();

// JSON object with the CPU time used between two snapshots; threads that exited in between are only in the process total.
std::string cpuDeltaJson(const CpuSnapshot &start, const CpuSnapshot &stop);

} // namespace rnti
