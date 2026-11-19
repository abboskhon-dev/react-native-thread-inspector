import assert from 'node:assert/strict';
import { mkdirSync, mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
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

test('android gives up when the app does not answer in time', async () => {
  const run = (cmd, args, opts) => {
    assert.equal(opts.timeout, 300);
    throw Object.assign(new Error('spawnSync adb ETIMEDOUT'), {
      code: 'ETIMEDOUT',
    });
  };
  await assert.rejects(
    record(
      { cmd: 'stop', platform: 'android', app: 'com.app', timeoutMs: 300 },
      run
    ),
    /did not answer in 300 ms/
  );
});

test('ios returns only the reply to its own request, not a late one', async () => {
  const container = mkdtempSync(join(tmpdir(), 'rnti-record-'));
  const dir = join(container, 'Library', 'Application Support');
  const reply = join(dir, 'rnti-record-reply.json');
  mkdirSync(dir, { recursive: true });
  writeFileSync(reply, '{"request":1,"reply":{"status":"stale"}}');
  const calls = [];
  const run = (cmd, args) => {
    calls.push([cmd, ...args].join(' '));
    if (args[1] === 'get_app_container') return `${container}\n`;
    const request = readFileSync(join(dir, 'rnti-record-stop.request'), 'utf8');
    setTimeout(
      () =>
        writeFileSync(reply, '{"request":2,"reply":{"status":"late start"}}'),
      100
    );
    setTimeout(
      () =>
        writeFileSync(
          reply,
          `{"request":${request},"reply":{"status":"available","path":"/s/session-2"}}`
        ),
      300
    );
    return '';
  };
  const r = await record({ cmd: 'stop', platform: 'ios', app: 'org.app' }, run);
  assert.equal(
    calls[1],
    'xcrun simctl spawn booted notifyutil -p org.app.rnti.sampling.stop'
  );
  assert.deepEqual(r, { status: 'available', path: '/s/session-2' });
});

test('ios fails fast when the app does not answer', async () => {
  const container = mkdtempSync(join(tmpdir(), 'rnti-record-'));
  const run = (cmd, args) => (args[1] === 'get_app_container' ? container : '');
  await assert.rejects(
    record(
      { cmd: 'start', platform: 'ios', app: 'org.app', timeoutMs: 300 },
      run
    ),
    /did not answer in 300 ms/
  );
});

test('ios refuses a physical device before touching it', async () => {
  const calls = [];
  const run = (cmd, args) => {
    calls.push(args.slice(0, 2).join(' '));
    return JSON.stringify({ devices: { 'iOS-26': [{ udid: 'SIM-1' }] } });
  };
  await assert.rejects(
    record(
      { cmd: 'start', platform: 'ios', app: 'org.app', device: 'PHONE-1' },
      run
    ),
    /PHONE-1 is not a simulator/
  );
  assert.deepEqual(calls, ['simctl list']);
});

test('rejects an app id that could escape the shell or notification name', async () => {
  await assert.rejects(
    record({ cmd: 'start', platform: 'android', app: 'a;rm -rf' }, () => ''),
    /invalid app id/
  );
  await assert.rejects(
    record({ cmd: 'start', platform: 'android', app: '-f' }, () => ''),
    /invalid app id/
  );
  await assert.rejects(
    record({ cmd: 'pause', platform: 'android', app: 'a' }, () => ''),
    /unknown record command/
  );
});
