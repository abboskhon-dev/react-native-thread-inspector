import { execFileSync } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { isSimulator } from './pull.mjs';

const ACTION = 'com.threadinspector.SAMPLING';

function exec(cmd, args, { timeout } = {}) {
  return execFileSync(cmd, args, {
    encoding: 'utf8',
    stdio: ['ignore', 'pipe', 'pipe'],
    timeout,
  });
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function readReply(file) {
  try {
    return JSON.parse(readFileSync(file, 'utf8'));
  } catch {
    return undefined;
  }
}

// `am broadcast` prints the receiver's reply as: Broadcast completed: result=0, data="{...}"
function recordAndroid({ cmd, app, device, timeoutMs }, run) {
  const adb = device ? ['-s', device] : [];
  let out;
  try {
    out = run(
      'adb',
      [
        ...adb,
        'shell',
        'am',
        'broadcast',
        '-a',
        ACTION,
        '-p',
        app,
        '--es',
        'cmd',
        cmd,
      ],
      { timeout: timeoutMs }
    );
  } catch (e) {
    if (e.code !== 'ETIMEDOUT') throw e;
    throw new Error(
      `${app} did not answer in ${timeoutMs} ms; is its JS thread busy?`
    );
  }
  const m = out.match(/data="(.*)"\s*$/m);
  if (!m)
    throw new Error(
      `${app} did not answer; is it running with start({ remoteSampling: true })?`
    );
  return JSON.parse(m[1]);
}

// A Darwin notification carries no payload, so the request id and the reply travel as files in the app's container.
async function recordIosSimulator({ cmd, app, device, timeoutMs }, run) {
  const target = device ?? 'booted';
  const container = run('xcrun', [
    'simctl',
    'get_app_container',
    target,
    app,
    'data',
  ]).trim();
  const dir = join(container, 'Library', 'Application Support');
  const reply = join(dir, 'rnti-record-reply.json');
  const request = Date.now() * 1000 + Math.floor(Math.random() * 1000);
  mkdirSync(dir, { recursive: true });
  writeFileSync(join(dir, `rnti-record-${cmd}.request`), String(request));
  run('xcrun', [
    'simctl',
    'spawn',
    target,
    'notifyutil',
    '-p',
    `${app}.rnti.sampling.${cmd}`,
  ]);
  for (const deadline = Date.now() + timeoutMs; Date.now() < deadline;) {
    // A late reply to an earlier command carries that command's id.
    const r = readReply(reply);
    if (r?.request === request) return r.reply;
    await sleep(100);
  }
  throw new Error(
    `${app} did not answer in ${timeoutMs} ms; is it running with start({ remoteSampling: true })?`
  );
}

// Starts or stops a Hermes sampling session in a running app that opted in with `remoteSampling`.
export async function record(
  { cmd, platform, app, device, timeoutMs = 5_000 },
  run = exec
) {
  if (cmd !== 'start' && cmd !== 'stop')
    throw new Error(`unknown record command: ${cmd} (start or stop)`);
  // The id is spliced into a broadcast filter and a notification name.
  if (!/^[A-Za-z0-9][A-Za-z0-9._-]*$/.test(app ?? ''))
    throw new Error(`invalid app id: ${app}`);
  if (platform === 'android')
    return recordAndroid({ cmd, app, device, timeoutMs }, run);
  if (platform === 'ios') {
    if (device && !isSimulator(device, run))
      throw new Error(
        `${device} is not a simulator; rnti record reaches iOS simulators only`
      );
    return recordIosSimulator({ cmd, app, device, timeoutMs }, run);
  }
  throw new Error(`unknown platform: ${platform} (android or ios)`);
}
