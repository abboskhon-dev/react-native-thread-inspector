#include "Export.h"

#include "JsonWriter.h"
#include "RntiPlatform.h"

#include <algorithm>
#include <set>
#include <unistd.h>
#include <unordered_map>

namespace rnti {

namespace {

constexpr uint32_t kSyntheticTidBase = 0x7f000000;
constexpr uint32_t kTriggerTidBase = kSyntheticTidBase + 0xff00;

// Trigger sources can overlap in time, and Perfetto rejects partially overlapping X slices on one track.
uint32_t triggerTid(TriggerSource s) { return kTriggerTidBase + static_cast<uint32_t>(s); }

double us(uint64_t ns) { return static_cast<double>(ns) / 1000.0; }

uint32_t trackTid(const ResolvedSpan &s) {
  // Probe envelopes overlap the work they observe, so they get their own track.
  return s.category == Category::Watchdog ? kSyntheticTidBase + s.label : s.tid;
}

void writeFlags(JsonWriter &w, uint16_t f) {
  if (f & flags::Approximate) w.key("approximate").boolean(true);
  if (f & flags::LeftTruncated) w.key("left_truncated").boolean(true);
  if (f & flags::Incomplete) w.key("incomplete").boolean(true);
  if (f & flags::Sampled) w.key("sampled").boolean(true);
  if (f & flags::Error) w.key("error").boolean(true);
  if (f & flags::JsDelivery) w.key("js_delivery_time").boolean(true);
  if (f & flags::Envelope) w.key("observation_envelope").boolean(true);
}

std::string catName(Category c) { return std::string("rnti.") + categoryName(c); }

std::string triggerMeasurement(TriggerSource s) {
  switch (s) {
    case TriggerSource::JsWatchdog:
    case TriggerSource::MainWatchdog:
      return "probe acknowledgement latency (queue/service delay, not CPU time)";
    case TriggerSource::JsLongTask: return "completed long task wall time reported by the runtime";
    case TriggerSource::Manual: return "manual trigger";
  }
  return "";
}

} // namespace

std::string chromeTraceJson(const Incident &incident, const ResolvedTrace &trace, const Recorder &recorder,
                            const ExportMetadata &meta) {
  const auto pid = static_cast<uint64_t>(getpid());
  JsonWriter w;
  w.beginObject().key("traceEvents").beginArray();

  w.beginObject().key("ph").str("M").key("pid").u64(pid).key("name").str("process_name");
  w.key("args").beginObject().key("name").str("React Native app").endObject().endObject();

  std::set<uint32_t> tids;
  for (const auto &s : trace.spans) tids.insert(trackTid(s));
  for (const auto &e : trace.instants) tids.insert(e.tid);
  std::unordered_map<uint32_t, std::string> names;
  for (auto &[tid, name] : recorder.threadNames()) names[tid] = name;

  for (uint32_t tid : tids) {
    std::string name;
    if (tid >= kSyntheticTidBase) name = "watchdog: " + recorder.labelName(tid - kSyntheticTidBase);
    else if (auto it = names.find(tid); it != names.end()) name = it->second;
    else name = "thread " + std::to_string(tid);
    w.beginObject().key("ph").str("M").key("pid").u64(pid).key("tid").u64(tid).key("name").str("thread_name");
    w.key("args").beginObject().key("name").str(name).endObject().endObject();
  }
  std::set<TriggerSource> sources;
  for (const auto &t : incident.triggers) sources.insert(t.source);
  for (TriggerSource src : sources) {
    w.beginObject().key("ph").str("M").key("pid").u64(pid).key("tid").u64(triggerTid(src)).key("name").str("thread_name");
    w.key("args").beginObject().key("name").str(std::string("trigger: ") + triggerSourceName(src)).endObject().endObject();
  }

  for (const auto &s : trace.spans) {
    const std::string name = recorder.labelName(s.label);
    if (s.async) {
      for (int phase = 0; phase < 2; ++phase) {
        w.beginObject().key("ph").str(phase == 0 ? "b" : "e").key("cat").str(catName(s.category));
        w.key("name").str(name).key("id").str(std::to_string(s.spanId)).key("pid").u64(pid).key("tid").u64(s.tid);
        w.key("ts").num(us(phase == 0 ? s.startNs : s.endNs));
        if (phase == 0) {
          w.key("args").beginObject();
          writeFlags(w, s.flags);
          if (s.flowId) w.key("flow_id").u64(s.flowId);
          w.endObject();
        }
        w.endObject();
      }
      continue;
    }
    w.beginObject().key("ph").str("X").key("cat").str(catName(s.category)).key("name").str(name);
    w.key("pid").u64(pid).key("tid").u64(trackTid(s)).key("ts").num(us(s.startNs)).key("dur").num(us(s.endNs - s.startNs));
    w.key("args").beginObject().key("span_id").u64(s.spanId);
    if (s.flowId) w.key("flow_id").u64(s.flowId);
    writeFlags(w, s.flags);
    w.endObject().endObject();
  }

  // Flow arrows only for spans sharing an explicitly propagated ID.
  std::unordered_map<uint64_t, std::vector<const ResolvedSpan *>> flows;
  for (const auto &s : trace.spans) {
    if (s.flowId && !s.async) flows[s.flowId].push_back(&s);
  }
  for (auto &[id, spans] : flows) {
    if (spans.size() < 2) continue;
    for (size_t i = 0; i < spans.size(); ++i) {
      const char *ph = i == 0 ? "s" : (i + 1 == spans.size() ? "f" : "t");
      w.beginObject().key("ph").str(ph).key("cat").str("rnti.flow").key("name").str("flow");
      w.key("id").u64(id).key("pid").u64(pid).key("tid").u64(trackTid(*spans[i]));
      w.key("ts").num(us(spans[i]->startNs));
      if (i != 0) w.key("bp").str("e");
      w.endObject();
    }
  }

  for (const auto &e : trace.instants) {
    w.beginObject().key("ph").str("i").key("s").str("t").key("cat").str(catName(e.category));
    w.key("name").str(recorder.labelName(e.label)).key("pid").u64(pid).key("tid").u64(e.tid).key("ts").num(us(e.ts));
    w.key("args").beginObject().key("arg0").u64(e.arg0);
    writeFlags(w, e.flags);
    w.endObject().endObject();
  }

  for (const auto &e : trace.counters) {
    w.beginObject().key("ph").str("C").key("name").str(recorder.labelName(e.label)).key("pid").u64(pid);
    w.key("ts").num(us(e.ts)).key("args").beginObject().key("value").u64(e.arg0).endObject().endObject();
  }

  for (const auto &t : incident.triggers) {
    const uint64_t end = t.endNs ? t.endNs : incident.captureEndNs;
    w.beginObject().key("ph").str("X").key("cat").str("rnti.incident").key("name").str(triggerSourceName(t.source));
    w.key("pid").u64(pid).key("tid").u64(triggerTid(t.source)).key("ts").num(us(t.onsetNs));
    w.key("dur").num(us(end > t.onsetNs ? end - t.onsetNs : 0));
    w.key("args").beginObject().key("observed_tid").u64(t.tid).key("onset_approximate").boolean(t.onsetApproximate);
    w.key("recovered").boolean(t.endNs != 0).key("measurement").str(triggerMeasurement(t.source));
    w.endObject().endObject();
  }

  w.endArray();
  w.key("displayTimeUnit").str("ms");
  w.key("otherData").beginObject();
  w.key("incident_id").u64(incident.id).key("session_id").str(meta.sessionId);
  w.key("clock_domain").str(clockDomainName());
  for (const auto &[k, v] : meta.fields) w.key(k).str(v);
  w.endObject();
  w.endObject();
  return w.take();
}

std::string incidentReportJson(const Incident &incident, const ResolvedTrace &trace,
                               const std::vector<TriggerAnalysis> &analysis, const Recorder &recorder,
                               const ExportMetadata &meta) {
  std::unordered_map<uint32_t, std::string> names;
  for (auto &[tid, name] : recorder.threadNames()) names[tid] = name;
  auto threadName = [&](uint32_t tid) -> std::string {
    auto it = names.find(tid);
    return it == names.end() ? std::string() : it->second;
  };

  JsonWriter w;
  w.beginObject();
  w.key("schema").str("rnti.incident/1");
  w.key("id").u64(incident.id).key("session_id").str(meta.sessionId);
  w.key("clock").beginObject().key("domain").str(clockDomainName()).key("unit").str("ns").endObject();

  w.key("window").beginObject();
  w.key("start_ns").u64(incident.windowStartNs).key("detected_ns").u64(incident.detectedNs);
  w.key("capture_end_ns").u64(incident.captureEndNs);
  w.key("requested_prehistory_ns").u64(incident.requestedPrehistoryNs);
  w.key("prehistory_relative_to").str("detection time, which can be later than stall onset");
  w.key("end_reason").str(endReasonName(incident.endReason));
  w.endObject();

  // Screen names are static route names, never route params.
  w.key("screen").beginObject().key("at_detection");
  if (incident.screenLabel) w.str(recorder.labelName(incident.screenLabel)); else w.null();
  w.key("transitions").beginArray();
  for (const auto &e : trace.instants) {
    if (e.category != Category::Navigation) continue;
    w.beginObject().key("screen").str(recorder.labelName(e.label)).key("ts_ns").u64(e.ts).endObject();
  }
  w.endArray().endObject();

  w.key("triggers").beginArray();
  for (const auto &t : incident.triggers) {
    w.beginObject().key("source").str(triggerSourceName(t.source)).key("tid").u64(t.tid);
    w.key("thread_name").str(threadName(t.tid));
    w.key("onset_ns").u64(t.onsetNs).key("onset_lower_bound_ns").u64(t.onsetLowerBoundNs);
    w.key("onset_approximate").boolean(t.onsetApproximate).key("detected_ns").u64(t.detectedNs);
    w.key("end_ns");
    if (t.endNs) w.u64(t.endNs); else w.null();
    w.key("duration_ns");
    if (t.endNs) w.u64(t.endNs - t.onsetNs); else w.null();
    w.key("measurement").str(triggerMeasurement(t.source));
    w.endObject();
  }
  w.endArray();

  w.key("analysis").beginArray();
  for (const auto &a : analysis) {
    w.beginObject().key("trigger_index").u64(a.triggerIndex);
    w.key("interval_start_ns").u64(a.intervalStartNs).key("interval_end_ns").u64(a.intervalEndNs);
    w.key("recovered").boolean(a.recovered);
    w.key("attributed_ns").u64(a.attributedNs).key("uncertain_ns").u64(a.uncertainNs);
    w.key("unattributed_ns").u64(a.unattributedNs);
    w.key("findings").beginArray();
    for (const auto &f : a.findings) {
      w.beginObject().key("relation").str(relationName(f.relation));
      w.key("label").str(recorder.labelName(f.span.label)).key("category").str(categoryName(f.span.category));
      w.key("tid").u64(f.span.tid).key("thread_name").str(threadName(f.span.tid));
      w.key("start_ns").u64(f.span.startNs).key("end_ns").u64(f.span.endNs).key("overlap_ns").u64(f.overlapNs);
      w.key("wall_time_not_cpu").boolean(true);
      if (f.span.flowId) w.key("flow_id").u64(f.span.flowId);
      if (f.boundsUncertain) w.key("bounds_uncertain").boolean(true);
      writeFlags(w, f.span.flags);
      w.endObject();
    }
    w.endArray();
    w.key("unknowns").beginArray();
    for (const auto &u : a.unknowns) w.str(u);
    w.endArray();
    w.endObject();
  }
  w.endArray();

  w.key("coverage").beginObject();
  w.key("rings").beginArray();
  for (const auto &r : incident.snapshot.rings) {
    w.beginObject().key("ring").u64(r.ring).key("kind").str(r.ring == kControlRing ? "control" : "thread");
    w.key("owner_tid").u64(r.ownerTid).key("written").u64(r.stats.written);
    w.key("overwritten").u64(r.stats.overwritten).key("skipped").u64(r.stats.skipped);
    if (r.stats.lostAfterPin) {
      w.key("lost_during_incident").u64(r.stats.lostAfterPin);
      w.key("gap_from_ns").u64(r.stats.gapFromNs).key("gap_to_ns").u64(r.stats.gapToNs);
    }
    w.key("oldest_retained_ns").u64(r.stats.oldestNs).key("newest_retained_ns").u64(r.stats.newestNs);
    w.endObject();
  }
  w.endArray();
  w.key("dropped_no_ring").u64(incident.snapshot.droppedNoRing);
  w.key("dropped_labels").u64(incident.snapshot.droppedLabels);
  w.key("event_count").u64(incident.snapshot.events.size());
  w.endObject();

  w.key("metadata").beginObject();
  for (const auto &[k, v] : meta.fields) w.key(k).str(v);
  w.endObject();
  w.endObject();
  return w.take();
}

} // namespace rnti
