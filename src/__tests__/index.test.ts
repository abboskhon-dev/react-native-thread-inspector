/// <reference types="jest" />
import type * as Api from '../index';

const mockNative = {
  start: jest.fn(),
  stop: jest.fn(),
  nowNs: jest.fn(),
  intern: jest.fn(),
  beginSpan: jest.fn(),
  endSpan: jest.fn(),
  instant: jest.fn(),
  reportLongTask: jest.fn(),
  markScreen: jest.fn(),
};

jest.mock('../NativeThreadInspector', () => ({
  __esModule: true,
  default: mockNative,
}));

let rnti: typeof Api;
let nextId: number;

beforeEach(() => {
  jest.resetModules();
  jest.resetAllMocks();
  jest.restoreAllMocks();
  mockNative.start.mockReturnValue('{}');
  mockNative.nowNs.mockReturnValue(0);
  nextId = 0;
  mockNative.intern.mockImplementation(() => ++nextId);
  mockNative.beginSpan.mockImplementation(() => 100 + nextId);
  delete (globalThis as any).PerformanceObserver;
  rnti = jest.requireActual('../index');
});

function stubClock(brackets: [j0: number, nativeNs: number, j1: number][]) {
  const js = brackets.flatMap(([j0, , j1]) => [j0, j1]);
  jest
    .spyOn((globalThis as any).performance, 'now')
    .mockImplementation(() => js.shift()!);
  mockNative.nowNs.mockImplementation(() => brackets.shift()![1]);
}

describe('calibrateClock', () => {
  it('uses the narrowest JS bracket around the mockNative read', () => {
    stubClock([
      [10, 50e6, 14],
      [20, 1020.5e6, 21],
      [30, 70e6, 32],
    ]);
    expect(rnti.calibrateClock(3)).toEqual({
      offsetNs: 1000e6,
      uncertaintyNs: 0.5e6,
    });
    expect(rnti.jsTimeToNativeNs(5)).toBe(1005e6);
  });
});

describe('long task observer', () => {
  it('is unavailable without PerformanceObserver', () => {
    expect(rnti.start().jsLongTasks).toEqual({
      status: 'unavailable',
      reason: 'PerformanceObserver missing',
    });
  });

  it('is disabled when longTasks is false', () => {
    expect(rnti.start({ longTasks: false }).jsLongTasks).toEqual({
      status: 'disabled',
      reason: 'not requested',
    });
  });

  it('is unavailable when longtask entries are not supported', () => {
    (globalThis as any).PerformanceObserver = class {
      static supportedEntryTypes = ['mark'];
    };
    expect(rnti.start().jsLongTasks).toEqual({
      status: 'unavailable',
      reason: 'longtask entry type not supported',
    });
  });

  it('reports long tasks on the mockNative clock and disconnects on stop', () => {
    let callback: (list: any) => void = () => {};
    const disconnect = jest.fn();
    (globalThis as any).PerformanceObserver = class {
      static supportedEntryTypes = ['longtask'];
      constructor(cb: (list: any) => void) {
        callback = cb;
      }
      observe() {}
      disconnect = disconnect;
    };
    stubClock(Array.from({ length: 7 }, () => [0, 1000e6, 0]));

    expect(rnti.start().jsLongTasks).toEqual({ status: 'available' });
    callback({ getEntries: () => [{ startTime: 2, duration: 80 }] });
    expect(mockNative.reportLongTask).toHaveBeenCalledWith(1002e6, 80e6);

    rnti.stop();
    expect(disconnect).toHaveBeenCalled();
    expect(mockNative.stop).toHaveBeenCalled();
  });
});

describe('labels', () => {
  it('interns each label once', () => {
    rnti.mark('a');
    rnti.mark('a', 7);
    rnti.mark('b');
    expect(mockNative.intern.mock.calls).toEqual([['a'], ['b']]);
    expect(mockNative.instant.mock.calls).toEqual([
      [1, 0],
      [1, 7],
      [2, 0],
    ]);
  });
});

describe('measureSync', () => {
  it('ends the span when the function throws', () => {
    expect(() =>
      rnti.measureSync('work', () => {
        throw new Error('boom');
      })
    ).toThrow('boom');
    expect(mockNative.beginSpan).toHaveBeenCalledWith(1, 0, false);
    expect(mockNative.endSpan).toHaveBeenCalledWith(1, 101, false);
  });
});

describe('trackNavigation', () => {
  function container(initial?: string) {
    const listeners: Record<string, (e: any) => void> = {};
    const off = jest.fn();
    let route = initial;
    return {
      off,
      emit: (event: string, e?: any) => listeners[event]!(e),
      go: (name: string) => {
        route = name;
      },
      addListener: (event: string, listener: (e: any) => void) => {
        listeners[event] = listener;
        return off;
      },
      getCurrentRoute: () => (route ? { name: route } : undefined),
    };
  }

  const screens = () =>
    mockNative.markScreen.mock.calls.map(
      ([id]: [number]) => mockNative.intern.mock.calls[id - 1]![0]
    );

  it('marks the initial screen and each change once', () => {
    const c = container('Home');
    rnti.trackNavigation(c);
    c.emit('state');
    c.go('Details');
    c.emit('state');
    expect(screens()).toEqual(['Home', 'Details']);
  });

  it('spans an action until the next state', () => {
    const c = container('Home');
    rnti.trackNavigation(c);
    c.emit('__unsafe_action__', { data: { action: { type: 'NAVIGATE' } } });
    expect(mockNative.intern).toHaveBeenLastCalledWith('navigation.navigate');
    expect(mockNative.beginSpan).toHaveBeenCalledWith(2, 0, true);
    expect(mockNative.endSpan).not.toHaveBeenCalled();

    c.go('Details');
    c.emit('state');
    expect(mockNative.endSpan).toHaveBeenCalledTimes(1);
    expect(mockNative.endSpan).toHaveBeenCalledWith(2, 102, true);
  });

  it('ignores no-op actions', () => {
    const c = container('Home');
    rnti.trackNavigation(c);
    c.emit('__unsafe_action__', { data: { noop: true } });
    expect(mockNative.beginSpan).not.toHaveBeenCalled();
  });

  it('ends a pending span and removes listeners on cleanup', () => {
    const c = container();
    const untrack = rnti.trackNavigation(c);
    c.emit('__unsafe_action__', { data: { action: { type: 'GO_BACK' } } });
    untrack();
    expect(mockNative.endSpan).toHaveBeenCalledTimes(1);
    expect(c.off).toHaveBeenCalledTimes(2);
    expect(mockNative.markScreen).not.toHaveBeenCalled();
  });
});
