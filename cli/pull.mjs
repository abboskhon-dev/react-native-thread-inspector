import { execFileSync } from 'node:child_process';
import {
  cpSync,
  existsSync,
  mkdirSync,
  readdirSync,
  readFileSync,
} from 'node:fs';
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

// UDID only: devicectl also takes names, and a phone may share a simulator's name.
export function isSimulator(device, run) {
  const { devices } = JSON.parse(
    run('xcrun', ['simctl', 'list', 'devices', '-j'])
  );
  return Object.values(devices)
    .flat()
    .some((d) => d.udid === device);
}

// devicectl reads the app data container only from development builds (get-task-allow).
function pullIosDevice({ app, device, out }, run) {
  let found = false;
  let lastError;
  for (const dir of Object.values(DIRS)) {
    try {
      run('xcrun', [
        'devicectl',
        'device',
        'copy',
        'from',
        '--device',
        device,
        '--domain-type',
        'appDataContainer',
        '--domain-identifier',
        app,
        '--source',
        `Library/Application Support/${dir}`,
        '--destination',
        join(out, dir),
        '--quiet',
      ]);
    } catch (e) {
      lastError = e.stderr?.trim() || e.message;
      continue;
    }
    found = true;
  }
  if (!found)
    throw new Error(
      `nothing copied from ${device}; check that it is unlocked, that ${app} is a development build, and that the app has started the inspector (devicectl: ${lastError})`
    );
}

function startedAt(sessionDir) {
  try {
    const file = join(sessionDir, 'session.json');
    return JSON.parse(readFileSync(file, 'utf8')).start.wall_us ?? 0;
  } catch {
    return 0;
  }
}

function listed(out, kind) {
  const dir = join(out, DIRS[kind]);
  if (!existsSync(dir)) return [];
  const paths = readdirSync(dir)
    .filter((n) => n.startsWith(PREFIX[kind]))
    .sort()
    .map((n) => join(dir, n));
  if (kind !== 'sessions') return paths;
  // Folder names carry uptime, which restarts on reboot and does not sort as text.
  const at = new Map(paths.map((p) => [p, startedAt(p)]));
  return paths.sort((a, b) => at.get(a) - at.get(b));
}

// Read-only on the device: copies saved incidents and sampling sessions to `out`.
export function pull({ platform, app, device, out = 'rnti-data' }, run = exec) {
  // The id is spliced into a device shell path.
  if (!/^[A-Za-z0-9._-]+$/.test(app ?? ''))
    throw new Error(`invalid app id: ${app}`);
  mkdirSync(out, { recursive: true });
  if (platform === 'android') pullAndroid({ app, device, out }, run);
  else if (platform === 'ios' && device && !isSimulator(device, run))
    pullIosDevice({ app, device, out }, run);
  else if (platform === 'ios') pullIosSimulator({ app, device, out }, run);
  else throw new Error(`unknown platform: ${platform} (android or ios)`);
  return {
    out,
    incidents: listed(out, 'incidents'),
    sessions: listed(out, 'sessions'),
  };
}
