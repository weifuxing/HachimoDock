/** Public release inputs are tested in disposable trees; never read user credentials. */
import {test} from 'node:test';
import assert from 'node:assert/strict';
import {copyFileSync, mkdirSync, mkdtempSync, rmSync, symlinkSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {dirname, join} from 'node:path';
import {spawnSync} from 'node:child_process';

const sentinel = 'fixture-only-do-not-print-value';
function fixture(t) {
  const root = mkdtempSync(join(tmpdir(), 'public-build-inputs-'));
  t.after(() => rmSync(root, {recursive: true, force: true}));
  const put = (name, data) => {
    const path = join(root, name);
    mkdirSync(dirname(path), {recursive: true});
    writeFileSync(path, data);
    return path;
  };
  put('scripts/check-public-build-env.mjs', '');
  copyFileSync(new URL('../scripts/check-public-build-env.mjs', import.meta.url),
    join(root, 'scripts/check-public-build-env.mjs'));
  put('src-tauri/tauri.conf.json', JSON.stringify({bundle: {resources: {
    'runtime': 'runtime', '../skills': 'skills',
  }}}));
  return {root, put, run: (env = {}) => spawnSync(process.execPath,
    [join(root, 'scripts/check-public-build-env.mjs')], {
      cwd: tmpdir(), env: {PATH: process.env.PATH, ...env}, encoding: 'utf8',
    })};
}

function rejected(result) {
  assert.equal(result.status, 1);
  assert.doesNotMatch(result.stdout + result.stderr, new RegExp(sentinel));
}

test('clean checkout allows absent generated resources, public config and signing inputs', t => {
  const f = fixture(t);
  f.put('.env.production', 'VITE_COMMUNITY_ORIGIN=https://example.invalid\n');
  f.put('.env.example', `VITE_PROVIDER_API_KEY=${sentinel}\n`);
  f.put('src-tauri/runtime/package.json', '{"name":"public-runtime"}');
  assert.equal(f.run({APPLE_API_KEY: sentinel, HACHIMI_MAC_SIGNING_IDENTITY: sentinel}).status, 0);
});

test('existing credential environment rejection remains redacted', t => {
  rejected(fixture(t).run({PET_MANAGER_INTERNAL_LLM_API_KEY: sentinel}));
});

test('Vite-prefixed credential environment input is rejected', t => {
  rejected(fixture(t).run({VITE_PROVIDER_API_KEY: sentinel}));
});

for (const name of ['.env', '.env.local', '.env.production', '.env.production.local', '.env.staging']) {
  test(`rejects credential assignments from ${name} before Vite loads them`, t => {
    const f = fixture(t);
    f.put(name, `# public fixture\nexport VITE_PROVIDER_API_KEY = "${sentinel}"\n`);
    rejected(f.run());
  });
}

test('dotenv cannot hide a reserved credential input behind an empty shell override', t => {
  const f = fixture(t);
  f.put('.env.production', `PET_MANAGER_INTERNAL_ASR_API_KEY=${sentinel}\n`);
  rejected(f.run({PET_MANAGER_INTERNAL_ASR_API_KEY: ''}));
});

for (const path of ['src-tauri/runtime/.env', 'src-tauri/runtime/state/library.json',
  'src-tauri/runtime/test/fixtures/personal.json', 'public/realtime-chat.jsonl',
  'dist/user/secure.json', 'skills/cache/user.sqlite']) {
  test(`rejects private or test input in packaged tree: ${path}`, t => {
    const f = fixture(t);
    f.put(path, sentinel);
    rejected(f.run());
  });
}

test('platform resource overlays are checked too', t => {
  const f = fixture(t);
  f.put('src-tauri/tauri.macos.conf.json', JSON.stringify({bundle: {resources: {
    'platform-runtime': 'platform-runtime',
  }}}));
  f.put('src-tauri/platform-runtime/.npmrc', sentinel);
  rejected(f.run());
});

test('resource symlink cannot import a private tree through recursive copying', t => {
  const f = fixture(t);
  const external = f.put('outside-runtime/innocent-name.txt', sentinel);
  f.put('src-tauri/runtime/package.json', '{}');
  symlinkSync(dirname(external), join(f.root, 'src-tauri/runtime/copied-user-state'));
  rejected(f.run());
});

test('bootstrap root symlinks are inspected rather than skipped or categorically rejected', t => {
  const f = fixture(t);
  const clean = f.put('bootstrap/package.json', '{}');
  symlinkSync(dirname(clean), join(f.root, 'src-tauri/runtime'));
  assert.equal(f.run().status, 0);
  f.put('bootstrap/state/library.json', sentinel);
  rejected(f.run());
});

test('unmapped source tests remain outside the runtime gate', t => {
  const f = fixture(t);
  f.put('src/test/fixtures/library.json', sentinel);
  assert.equal(f.run().status, 0);
});
