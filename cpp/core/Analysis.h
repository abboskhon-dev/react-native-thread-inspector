#pragma once

#include "IncidentController.h"

#include <vector>

namespace rnti {

struct ResolvedSpan {
  uint32_t tid = 0;
  uint32_t label = 0;
  Category category = Category::Inspector;
  uint64_t startNs = 0;
  uint64_t endNs = 0;
  uint16_t flags = 0;
  uint64_t spanId = 0;
  uint64_t flowId = 0;
  bool async = false;
};

struct ResolvedTrace {
  std::vector<ResolvedSpan> spans;
  std::vector<Event> instants;
  std::vector<Event> counters;
};

// Pairs begin/end records; unmatched ends are left-truncated, unmatched begins incomplete.
ResolvedTrace resolve(const Incident &incident);

enum class Relation : uint8_t { DirectContainment, ExplicitFlow, TemporalAssociation };
const char *relationName(Relation r) noexcept;

struct SpanFinding {
  Relation relation;
  ResolvedSpan span;
  uint64_t overlapNs = 0;
};

struct TriggerAnalysis {
  size_t triggerIndex = 0;
  uint64_t intervalStartNs = 0;
  uint64_t intervalEndNs = 0;
  bool recovered = true;
  uint64_t attributedNs = 0;
  uint64_t unattributedNs = 0;
  std::vector<SpanFinding> findings;
  std::vector<std::string> unknowns;
};

std::vector<TriggerAnalysis> analyze(const Incident &incident, const ResolvedTrace &trace);

} // namespace rnti
