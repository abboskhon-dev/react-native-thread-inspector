import { execFileSync } from 'node:child_process';
import { cpSync, existsSync, mkdirSync, readdirSync } from 'node:fs';
import { join } from 'node:path';

const DIRS = { incidents: 'rnti-incidents', sessions: 'rnti-sessions' };
const PREFIX = { incidents: 'incident-', sessions: 'session-' };

function exec(cmd, args) {
  return execFileSync(cmd, args, {
    encoding: 'utf8',
    stdio: ['ignore', 'pipe', 'pipe'],
  });
}

function pullAndroid({ app, device, out }, run) {
  const adb = device ? ['-s', device] : [];
  const base = `/sdcard/Android/data/${app}/files`;
  const found = [];
  for (const dir of Object.values(DIRS)) {
    try {
      run('adb', [...adb, 'shell', 'ls', `${base}/${dir}`]);
    } catch {
      continue;
    }
    run('adb', [...adb, 'pull', `${base}/${dir}`, out]);
    found.push(dir);
  }
  if (!found.length)
    throw new Error(
      `nothing under ${base} on the device; check the app id, that the app has started the inspector, and \`adb devices\``
    );
}

function pullIosSimulator({ app, device, out }, run) {
  const container = run('xcrun', [
    'simctl',
    'get_app_container',
    device ?? 'booted',
    app,
    'data',
  ]).trim();
  const base = join(container, 'Library', 'Application Support');
  let found = false;
  for (const dir of Object.values(DIRS)) {
    if (!existsSync(join(base, dir))) continue;
    cpSync(join(base, dir), join(out, dir), { recursive: true });
    found = true;
  }
  if (!found)
    throw new Error(
      `nothing under ${base}; has the app started the inspector?`
    );
}

function listed(out, kind) {
  const dir = join(out, DIRS[kind]);
  if (!existsSync(dir)) return [];
  return readdirSync(dir)
    .filter((n) => n.startsWith(PREFIX[kind]))
    .sort()
    .map((n) => join(dir, n));
}

// Read-only on the device: copies saved incidents and sampling sessions to `out`.
export function pull({ platform, app, device, out = 'rnti-data' }, run = exec) {
  // The id is spliced into a device shell path.
  if (!/^[A-Za-z0-9._-]+$/.test(app ?? ''))
    throw new Error(`invalid app id: ${app}`);
  mkdirSync(out, { recursive: true });
  if (platform === 'android') pullAndroid({ app, device, out }, run);
  else if (platform === 'ios') pullIosSimulator({ app, device, out }, run);
  else throw new Error(`unknown platform: ${platform} (android or ios)`);
  return {
    out,
    incidents: listed(out, 'incidents'),
    sessions: listed(out, 'sessions'),
  };
}
