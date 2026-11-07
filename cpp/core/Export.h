#pragma once

#include "Analysis.h"
#include "IncidentController.h"
#include "Recorder.h"

#include <map>
#include <string>

namespace rnti {

struct ExportMetadata {
  std::string sessionId;
  // Free-form build/capability facts, e.g. platform, rnVersion, appBuild, capability states.
  std::map<std::string, std::string> fields;
};

// Chrome Trace Event JSON (microseconds), importable by Perfetto.
std::string chromeTraceJson(const Incident &incident, const ResolvedTrace &trace, const Recorder &recorder,
                            const ExportMetadata &meta);

// Machine-readable summary; useful without a viewer.
std::string incidentReportJson(const Incident &incident, const ResolvedTrace &trace,
                               const std::vector<TriggerAnalysis> &analysis, const Recorder &recorder,
                               const ExportMetadata &meta);

} // namespace rnti
