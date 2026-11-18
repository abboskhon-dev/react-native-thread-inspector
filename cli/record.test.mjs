import assert from 'node:assert/strict';
import { mkdirSync, mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';
import { record } from './record.mjs';

test('android sends a broadcast only the app receives and returns its reply', async () => {
  const calls = [];
  const run = (cmd, args) => {
    calls.push([cmd, ...args].join(' '));
    return 'Broadcasting: Intent { act=com.threadinspector.SAMPLING }\nBroadcast completed: result=0, data="{"status":"available","path":"/x/session-1"}"\n';
  };
  const r = await record(
    { cmd: 'stop', platform: 'android', app: 'com.app', device: 'S1' },
    run
  );
  assert.deepEqual(calls, [
    'adb -s S1 shell am broadcast -a com.threadinspector.SAMPLING -p com.app --es cmd stop',
  ]);
  assert.deepEqual(r, { status: 'available', path: '/x/session-1' });
});

test('android fails when the app did not opt in', async () => {
  const run = () =>
    'Broadcasting: Intent { act=com.threadinspector.SAMPLING }\nBroadcast completed: result=0\n';
  await assert.rejects(
    record({ cmd: 'start', platform: 'android', app: 'com.app' }, run),
    /remoteSampling: true/
  );
});

test('ios posts the notification and returns the reply the app writes', async () => {
  const container = mkdtempSync(join(tmpdir(), 'rnti-record-'));
  const reply = join(container, 'Library', 'Application Support', 'rnti-record-reply.json');
  mkdirSync(join(container, 'Library', 'Application Support'), { recursive: true });
  writeFileSync(reply, '{"status":"stale"}');
  const calls = [];
  const run = (cmd, args) => {
    calls.push([cmd, ...args].join(' '));
    if (args[1] === 'get_app_container') return `${container}\n`;
    setTimeout(() => writeFileSync(reply, '{"status":"available","path":"/s/session-2"}'), 200);
    return '';
  };
  const r = await record({ cmd: 'stop', platform: 'ios', app: 'org.app' }, run);
  assert.equal(calls[1], 'xcrun simctl spawn booted notifyutil -p org.app.rnti.sampling.stop');
  assert.deepEqual(r, { status: 'available', path: '/s/session-2' });
});

test('ios fails fast when the app does not answer', async () => {
  const container = mkdtempSync(join(tmpdir(), 'rnti-record-'));
  const run = (cmd, args) => (args[1] === 'get_app_container' ? container : '');
  await assert.rejects(
    record({ cmd: 'start', platform: 'ios', app: 'org.app', timeoutMs: 300 }, run),
    /did not answer in 300 ms/
  );
});

test('rejects an app id that could escape the shell or notification name', async () => {
  await assert.rejects(
    record({ cmd: 'start', platform: 'android', app: 'a;rm -rf' }, () => ''),
    /invalid app id/
  );
  await assert.rejects(
    record({ cmd: 'pause', platform: 'android', app: 'a' }, () => ''),
    /unknown record command/
  );
});
