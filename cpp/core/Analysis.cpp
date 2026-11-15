#include "Analysis.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace rnti {

const char *relationName(Relation r) noexcept {
  switch (r) {
    case Relation::DirectContainment: return "direct_containment";
    case Relation::ExplicitFlow: return "explicit_flow";
    case Relation::TemporalAssociation: return "temporal_association";
  }
  return "unknown";
}

ResolvedTrace resolve(const Incident &incident) {
  ResolvedTrace out;
  std::unordered_map<uint64_t, SnapshotEvent> open;
  open.reserve(64);

  for (const auto &se : incident.snapshot.events) {
    const Event &e = se.event;
    switch (e.kind) {
      case EventKind::Begin:
      case EventKind::AsyncBegin:
        open[e.spanId] = se;
        break;
      case EventKind::End:
      case EventKind::AsyncEnd: {
        ResolvedSpan s;
        s.async = e.kind == EventKind::AsyncEnd;
        s.endNs = e.ts;
        s.spanId = e.spanId;
        s.ring = se.ring;
        if (auto it = open.find(e.spanId); it != open.end()) {
          const Event &b = it->second.event;
          s.tid = b.tid;
          s.label = b.label;
          s.category = b.category;
          s.startNs = b.ts;
          s.flowId = b.flowId ? b.flowId : e.flowId;
          s.flags = b.flags | e.flags;
          open.erase(it);
        } else {
          s.tid = e.tid;
          s.label = e.label;
          s.category = e.category;
          s.startNs = std::min(incident.windowStartNs, e.ts);
          s.flowId = e.flowId;
          s.flags = e.flags | flags::LeftTruncated;
        }
        out.spans.push_back(s);
        break;
      }
      case EventKind::Complete: {
        ResolvedSpan s{e.tid, e.label, e.category, e.ts, e.ts + e.dur, e.flags, e.spanId, e.flowId, false, se.ring};
        out.spans.push_back(s);
        break;
      }
      case EventKind::Instant:
        out.instants.push_back(e);
        break;
      case EventKind::Counter:
        out.counters.push_back(e);
        break;
    }
  }

  for (const auto &[id, sb] : open) {
    const Event &b = sb.event;
    ResolvedSpan s{b.tid, b.label, b.category, b.ts, std::max(b.ts, incident.captureEndNs),
                   static_cast<uint16_t>(b.flags | flags::Incomplete), b.spanId, b.flowId,
                   b.kind == EventKind::AsyncBegin, sb.ring};
    out.spans.push_back(s);
  }
  std::sort(out.spans.begin(), out.spans.end(),
            [](const ResolvedSpan &a, const ResolvedSpan &b) { return a.startNs < b.startNs; });
  return out;
}

namespace {

uint64_t overlap(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1) {
  const uint64_t lo = std::max(a0, b0);
  const uint64_t hi = std::min(a1, b1);
  return hi > lo ? hi - lo : 0;
}

bool isEvidence(const ResolvedSpan &s) {
  return s.category != Category::Watchdog && s.category != Category::Incident && !(s.flags & flags::Envelope);
}

uint64_t unionLength(std::vector<std::pair<uint64_t, uint64_t>> intervals) {
  std::sort(intervals.begin(), intervals.end());
  uint64_t total = 0, curStart = 0, curEnd = 0;
  bool have = false;
  for (const auto &[s, e] : intervals) {
    if (!have || s > curEnd) {
      if (have) total += curEnd - curStart;
      curStart = s;
      curEnd = e;
      have = true;
    } else {
      curEnd = std::max(curEnd, e);
    }
  }
  if (have) total += curEnd - curStart;
  return total;
}

constexpr size_t kMaxContained = 20;
constexpr size_t kMaxAssociated = 10;

} // namespace

std::vector<TriggerAnalysis> analyze(const Incident &incident, const ResolvedTrace &trace) {
  std::unordered_set<uint32_t> lossyRings;
  for (const auto &r : incident.snapshot.rings) {
    if (r.stats.lostAfterPin > 0 || r.stats.skipped > 0 ||
        (r.stats.overwritten > 0 && r.stats.oldestNs > incident.windowStartNs))
      lossyRings.insert(r.ring);
  }
  std::vector<TriggerAnalysis> results;
  for (size_t i = 0; i < incident.triggers.size(); ++i) {
    const Trigger &t = incident.triggers[i];
    TriggerAnalysis a;
    a.triggerIndex = i;
    a.intervalStartNs = t.onsetNs;
    a.recovered = t.endNs != 0;
    a.intervalEndNs = a.recovered ? t.endNs : incident.captureEndNs;

    std::vector<SpanFinding> contained, associated;
    std::vector<std::pair<uint64_t, uint64_t>> clipped, uncertain;
    std::unordered_set<uint64_t> flows;
    for (const auto &s : trace.spans) {
      if (!isEvidence(s)) continue;
      const uint64_t ov = overlap(s.startNs, s.endNs, a.intervalStartNs, a.intervalEndNs);
      if (ov == 0) continue;
      if (t.tid != 0 && s.tid == t.tid && !s.async) {
        // A missing end after recovery, or any reconstructed bound on a ring that lost events, is not proof.
        const bool reconstructed = s.flags & (flags::Incomplete | flags::LeftTruncated);
        const bool unsure = reconstructed && (lossyRings.count(s.ring) || ((s.flags & flags::Incomplete) && a.recovered));
        contained.push_back({Relation::DirectContainment, s, ov, unsure});
        (unsure ? uncertain : clipped)
            .emplace_back(std::max(s.startNs, a.intervalStartNs), std::min(s.endNs, a.intervalEndNs));
        if (s.flowId) flows.insert(s.flowId);
      } else {
        associated.push_back({Relation::TemporalAssociation, s, ov});
      }
    }

    // Flow edges only come from propagated IDs, never from proximity.
    for (auto &f : associated) {
      if (f.span.flowId && flows.count(f.span.flowId)) f.relation = Relation::ExplicitFlow;
    }

    auto byOverlap = [](const SpanFinding &x, const SpanFinding &y) { return x.overlapNs > y.overlapNs; };
    std::sort(contained.begin(), contained.end(), byOverlap);
    std::stable_sort(associated.begin(), associated.end(), [](const SpanFinding &x, const SpanFinding &y) {
      if ((x.relation == Relation::ExplicitFlow) != (y.relation == Relation::ExplicitFlow))
        return x.relation == Relation::ExplicitFlow;
      return x.overlapNs > y.overlapNs;
    });
    if (contained.size() > kMaxContained) contained.resize(kMaxContained);
    if (associated.size() > kMaxAssociated) associated.resize(kMaxAssociated);

    const uint64_t length = a.intervalEndNs > a.intervalStartNs ? a.intervalEndNs - a.intervalStartNs : 0;
    a.attributedNs = std::min(length, unionLength(clipped));
    std::vector<std::pair<uint64_t, uint64_t>> any = clipped;
    any.insert(any.end(), uncertain.begin(), uncertain.end());
    const uint64_t covered = std::min(length, unionLength(std::move(any)));
    a.uncertainNs = covered - a.attributedNs;
    a.unattributedNs = length - covered;
    a.findings = std::move(contained);
    a.findings.insert(a.findings.end(), associated.begin(), associated.end());

    if (t.tid == 0) a.unknowns.emplace_back("Thread of the stalled work is unknown; containment was not evaluated.");
    if (t.onsetApproximate)
      a.unknowns.emplace_back("Onset is the probe enqueue time; the stall may have started earlier.");
    if (!a.recovered) a.unknowns.emplace_back("Recovery was not observed before capture ended.");
    if (t.endNs && t.detectedNs > t.endNs)
      a.unknowns.emplace_back("Detected only after recovery: the watchdog woke late, so nothing was captured while it was ongoing.");
    if (a.uncertainNs > 0)
      a.unknowns.emplace_back("Some span bounds were reconstructed across missing events; that time is reported as uncertain.");
    if (a.unattributedNs > 0)
      a.unknowns.emplace_back("Part of the interval has no instrumented span on the stalled thread.");
    a.unknowns.emplace_back("No stack samples were captured; work inside spans is not attributed.");
    for (const auto &r : incident.snapshot.rings) {
      if (r.stats.lostAfterPin > 0) {
        a.unknowns.emplace_back(std::to_string(r.stats.lostAfterPin) + " events on ring " + std::to_string(r.ring) +
                                " were overwritten during the incident; that interval is missing.");
      }
      if (r.stats.overwritten > 0 && r.stats.oldestNs > incident.windowStartNs) {
        a.unknowns.emplace_back("Some history before the requested window was overwritten on ring " +
                                std::to_string(r.ring) + ".");
      }
    }
    results.push_back(std::move(a));
  }
  return results;
}

} // namespace rnti
