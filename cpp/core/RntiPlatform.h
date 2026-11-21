#pragma once

#include <cstdint>
#include <ctime>

#if defined(__APPLE__)
#include <pthread.h>
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace rnti {

// One monotonic domain per process; matches System.nanoTime on Android and mach_absolute_time on Apple.
inline uint64_t nowNs() noexcept {
#if defined(__APPLE__)
  return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
#endif
}

inline const char *clockDomainName() noexcept {
#if defined(__APPLE__)
  return "CLOCK_UPTIME_RAW";
#else
  return "CLOCK_MONOTONIC";
#endif
}

inline uint32_t currentTid() noexcept {
#if defined(__APPLE__)
  uint64_t tid = 0;
  pthread_threadid_np(nullptr, &tid);
  return static_cast<uint32_t>(tid);
#else
  return static_cast<uint32_t>(syscall(SYS_gettid));
#endif
}

constexpr uint64_t kMs = 1000000ull;

} // namespace rnti
