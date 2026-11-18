import { execFileSync } from 'node:child_process';
import { existsSync, readFileSync, rmSync } from 'node:fs';
import { join } from 'node:path';

const ACTION = 'com.threadinspector.SAMPLING';

function exec(cmd, args) {
  return execFileSync(cmd, args, {
    encoding: 'utf8',
    stdio: ['ignore', 'pipe', 'pipe'],
  });
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// `am broadcast` prints the receiver's reply as: Broadcast completed: result=0, data="{...}"
function recordAndroid({ cmd, app, device }, run) {
  const adb = device ? ['-s', device] : [];
  const out = run('adb', [
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
  ]);
  const m = out.match(/data="(.*)"\s*$/m);
  if (!m)
    throw new Error(
      `${app} did not answer; is it running with start({ remoteSampling: true })?`
    );
  return JSON.parse(m[1]);
}

// A Darwin notification has no reply, so the app writes one into its container, which the Mac can read.
async function recordIosSimulator({ cmd, app, device, timeoutMs }, run) {
  const target = device ?? 'booted';
  const container = run('xcrun', [
    'simctl',
    'get_app_container',
    target,
    app,
    'data',
  ]).trim();
  const reply = join(container, 'Library', 'Application Support', 'rnti-record-reply.json');
  rmSync(reply, { force: true });
  run('xcrun', [
    'simctl',
    'spawn',
    target,
    'notifyutil',
    '-p',
    `${app}.rnti.sampling.${cmd}`,
  ]);
  for (const deadline = Date.now() + timeoutMs; Date.now() < deadline; ) {
    if (existsSync(reply)) return JSON.parse(readFileSync(reply, 'utf8'));
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
  if (!/^[A-Za-z0-9._-]+$/.test(app ?? ''))
    throw new Error(`invalid app id: ${app}`);
  if (platform === 'android')
    return recordAndroid({ cmd, app, device }, run);
  if (platform === 'ios')
    return recordIosSimulator({ cmd, app, device, timeoutMs }, run);
  throw new Error(`unknown platform: ${platform} (android or ios)`);
}
