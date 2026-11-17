// Shape of `incident.json`, schema `rnti.incident/1`. `_ns` fields are nanoseconds on the device's monotonic clock.

export type IncidentTriggerSource =
  'js_watchdog' | 'main_watchdog' | 'js_long_task' | 'manual';

export type IncidentEndReason =
  'recovered' | 'max_duration' | 'lifecycle' | 'disabled';

export type IncidentRelation =
  'direct_containment' | 'explicit_flow' | 'temporal_association';

export type IncidentCategory =
  | 'js'
  | 'native'
  | 'main'
  | 'frame'
  | 'navigation'
  | 'watchdog'
  | 'incident'
  | 'lifecycle'
  | 'turbomodule'
  | 'inspector'
  | 'user'
  | 'fabric'
  | 'unknown';

export type IncidentTrigger = {
  source: IncidentTriggerSource;
  // 0 when the stalled thread was never identified.
  tid: number;
  thread_name: string;
  onset_ns: number;
  onset_lower_bound_ns: number;
  onset_approximate: boolean;
  detected_ns: number;
  // null when recovery was not observed before capture ended.
  end_ns: number | null;
  duration_ns: number | null;
  // What the trigger measured, e.g. probe queue delay rather than CPU time.
  measurement: string;
};

export type IncidentFinding = {
  relation: IncidentRelation;
  label: string;
  category: IncidentCategory;
  tid: number;
  thread_name: string;
  start_ns: number;
  end_ns: number;
  overlap_ns: number;
  wall_time_not_cpu: true;
  flow_id?: number;
  // Span bounds were reconstructed across missing events.
  bounds_uncertain?: true;
  approximate?: true;
  left_truncated?: true;
  incomplete?: true;
  sampled?: true;
  error?: true;
  js_delivery_time?: true;
  observation_envelope?: true;
};

export type IncidentAnalysis = {
  // Index into `triggers`.
  trigger_index: number;
  interval_start_ns: number;
  interval_end_ns: number;
  recovered: boolean;
  // attributed + uncertain + unattributed = interval length.
  attributed_ns: number;
  uncertain_ns: number;
  unattributed_ns: number;
  findings: IncidentFinding[];
  unknowns: string[];
};

export type IncidentRingCoverage = {
  ring: number;
  kind: 'control' | 'thread';
  owner_tid: number;
  written: number;
  overwritten: number;
  skipped: number;
  lost_during_incident?: number;
  gap_from_ns?: number;
  gap_to_ns?: number;
  oldest_retained_ns: number;
  newest_retained_ns: number;
};

export type IncidentReport = {
  schema: 'rnti.incident/1';
  id: number;
  session_id: string;
  clock: { domain: string; unit: 'ns' };
  window: {
    start_ns: number;
    detected_ns: number;
    capture_end_ns: number;
    requested_prehistory_ns: number;
    prehistory_relative_to: string;
    end_reason: IncidentEndReason;
  };
  screen: {
    at_detection: string | null;
    transitions: { screen: string; ts_ns: number }[];
  };
  triggers: IncidentTrigger[];
  analysis: IncidentAnalysis[];
  coverage: {
    rings: IncidentRingCoverage[];
    dropped_no_ring: number;
    dropped_labels: number;
    event_count: number;
  };
  // Device, OS and build facts recorded by the platform module.
  metadata: Record<string, string>;
};
