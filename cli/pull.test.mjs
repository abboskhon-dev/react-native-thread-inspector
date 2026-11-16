import assert from 'node:assert/strict';
import { mkdirSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';
import { pull } from './pull.mjs';

const tmp = () => mkdtempSync(join(tmpdir(), 'rnti-pull-'));

test('android pulls both directories from app external files', () => {
  const out = tmp();
  const calls = [];
  const run = (cmd, args) => {
    calls.push([cmd, ...args].join(' '));
    if (args.includes('pull'))
      mkdirSync(join(out, args.at(-2).split('/').pop(), 'incident-x-1'), {
        recursive: true,
      });
    return '';
  };
  const r = pull(
    { platform: 'android', app: 'com.app', device: 'S1', out },
    run
  );
  assert.deepEqual(calls, [
    'adb -s S1 shell ls /sdcard/Android/data/com.app/files/rnti-incidents',
    `adb -s S1 pull /sdcard/Android/data/com.app/files/rnti-incidents ${out}`,
    'adb -s S1 shell ls /sdcard/Android/data/com.app/files/rnti-sessions',
    `adb -s S1 pull /sdcard/Android/data/com.app/files/rnti-sessions ${out}`,
  ]);
  assert.equal(r.incidents.length, 1);
  assert.equal(r.sessions.length, 0);
});

test('android skips a missing directory and fails when both are missing', () => {
  const out = tmp();
  const pulled = [];
  const run = (cmd, args) => {
    if (args.includes('ls') && args.at(-1).endsWith('rnti-sessions'))
      throw new Error('No such file or directory');
    if (args.includes('pull')) pulled.push(args.at(-2));
    return '';
  };
  pull({ platform: 'android', app: 'com.app', out }, run);
  assert.deepEqual(pulled, [
    '/sdcard/Android/data/com.app/files/rnti-incidents',
  ]);
  assert.throws(
    () =>
      pull({ platform: 'android', app: 'com.app', out }, () => {
        throw new Error('no device');
      }),
    /nothing under/
  );
});

test('ios copies from the simulator data container', () => {
  const container = tmp();
  const out = tmp();
  mkdirSync(
    join(container, 'Library/Application Support/rnti-sessions/session-9'),
    { recursive: true }
  );
  const r = pull({ platform: 'ios', app: 'com.app', out }, (cmd, args) => {
    assert.deepEqual(
      [cmd, ...args],
      ['xcrun', 'simctl', 'get_app_container', 'booted', 'com.app', 'data']
    );
    return `${container}\n`;
  });
  assert.deepEqual(r.sessions, [join(out, 'rnti-sessions', 'session-9')]);
});

test('rejects app ids that could reach the device shell', () => {
  assert.throws(
    () => pull({ platform: 'android', app: 'a; rm -rf /', out: tmp() }),
    /invalid app id/
  );
  assert.throws(
    () => pull({ platform: 'web', app: 'a', out: tmp() }),
    /unknown platform/
  );
});
