#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rnti {

struct ThreadCpu {
  uint32_t tid = 0;
  std::string name;
  uint64_t cpuNs = 0;
};

struct CpuSnapshot {
  uint64_t monoNs = 0;
  uint64_t processCpuNs = 0;
  std::vector<ThreadCpu> threads;
};

// CPU time used so far by the process and by each thread that is alive now.
CpuSnapshot cpuSnapshot();

// JSON object with the CPU time used between two snapshots; threads that exited in between are only in the process total.
std::string cpuDeltaJson(const CpuSnapshot &start, const CpuSnapshot &stop);

} // namespace rnti
