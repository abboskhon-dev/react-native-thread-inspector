import {
  TurboModuleRegistry,
  type CodegenTypes,
  type TurboModule,
} from 'react-native';

export type IncidentSavedEvent = {
  id: number;
  path: string;
};

export interface Spec extends TurboModule {
  // Android runs void TurboModule methods async on mqt_v_native; timing calls must return a value to stay on JS.
  start(configJson: string): string;
  stop(): void;
  nowNs(): number;
  intern(label: string): number;
  newFlowId(): number;
  beginSpan(labelId: number, flowId: number, async: boolean): number;
  endSpan(labelId: number, spanId: number, async: boolean): number;
  instant(labelId: number, arg: number): number;
  completeSpan(
    labelId: number,
    startNs: number,
    durationNs: number,
    flags: number
  ): number;
  reportLongTask(startNs: number, durationNs: number): number;
  markScreen(labelId: number): number;
  startHermesSampling(hz: number): string;
  stopHermesSampling(): string;
  listIncidents(): string[];
  readIncidentFile(path: string, file: string): string;
  deleteIncident(path: string): boolean;
  clearData(): number;
  simulateNativeBlock(ms: number): number;
  simulateMainThreadBlock(ms: number): void;
  simulateAsyncNativeWork(ms: number, flowId: number): Promise<void>;

  readonly onIncidentSaved: CodegenTypes.EventEmitter<IncidentSavedEvent>;
}

export default TurboModuleRegistry.getEnforcing<Spec>('ThreadInspector');
