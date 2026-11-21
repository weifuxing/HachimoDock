'use strict'

// Only synthetic homes/profiles. No running Harness, credentials or transcripts.
const test = require('node:test')
const assert = require('node:assert/strict')
const fs = require('node:fs')
const os = require('node:os')
const path = require('node:path')
const { spawnSync } = require('node:child_process')
const { windowsAcl } = require('../windows-acl')
const root = path.join(__dirname, '..')

function fixture(t, { override = false, bom = false } = {}) {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'dsh-install-fixture-'))
  t.after(() => fs.rmSync(tmp, { recursive: true, force: true }))
  const mkdir = name => {
    if (process.platform === 'win32') windowsAcl('prepare-directory', name)
    else fs.mkdirSync(name, { mode: 0o700 })
  }
  const write = (name, content) => {
    if (process.platform === 'win32') windowsAcl('create-file', name)
    fs.writeFileSync(name, content, { mode: 0o600 })
  }
  const home = path.join(tmp, '用户 Home'); mkdir(home)
  const dshHome = path.join(home, override ? 'Harness 数据' : '.dsh'); mkdir(dshHome)
  mkdir(path.join(dshHome, 'profiles'))
  const profile = path.join(dshHome, 'profiles', 'desktop'); mkdir(profile)
  write(path.join(profile, 'package.json'), (bom ? '\uFEFF' : '') + JSON.stringify({ dsh: { profile: { bundles: ['@deepseek-ai/dsh-web-app'] } } }))
  const original = Buffer.from((bom ? '\uFEFF' : '') + '# 用户注释：保留\r\n[] # empty profile\r\n')
  const patch = path.join(profile, 'cordis.patch.yml'); write(patch, original)
  // No real user files exist. These canaries must never be opened or changed.
  write(path.join(dshHome, 'credentials.json'), 'synthetic-do-not-read')
  write(path.join(profile, 'sessions.json'), 'synthetic-do-not-read')
  const bundle = path.join(tmp, 'bundle')
  fs.cpSync(root, bundle, { recursive: true, filter: p => !p.startsWith(path.join(root, 'test')) })
  const audit = path.join(tmp, 'assert-no-private-reads.cjs')
  fs.writeFileSync(audit, `const fs = require('node:fs');
    const blocked = new Set(${JSON.stringify([path.join(dshHome, 'credentials.json'), path.join(profile, 'sessions.json'), path.join(profile, 'lock')])});
    for (const key of ['openSync', 'readFileSync']) {
      const original = fs[key];
      fs[key] = function(p, ...args) { if (blocked.has(String(p))) throw Error('private-read-forbidden'); return original.call(this, p, ...args); };
    }`)
  const env = { HOME: home, USERPROFILE: home, PATH: process.env.PATH,
    ...(process.env.SystemRoot ? { SystemRoot: process.env.SystemRoot } : {}),
    ...(process.env.WINDIR ? { WINDIR: process.env.WINDIR } : {}),
    ...(override ? { DSH_HOME: dshHome } : {}) }
  const run = (op = '--install') => {
    const r = spawnSync(process.execPath, ['--require', audit, path.join(bundle, 'managed-install.js'), op], {
      env, cwd: home, encoding: 'utf8', timeout: 120000,
    })
    assert.equal(r.error, undefined)
    assert.equal(r.stderr, '')
    assert.ok(!r.stdout.includes(home) && !r.stdout.includes('synthetic-do-not-read'), 'receipt contains no private path/content')
    return JSON.parse(r.stdout)
  }
  return { home, profile, patch, original, bundle, run, write, mkdir, dshHome }
}

test('running desktop lock does not block metadata HMR; upgrade is owned, repeatable and reversible', t => {
  const f = fixture(t)
  const lock = path.join(f.profile, 'lock'); f.write(lock, 'synthetic host owns this lock')
  const beforeLock = fs.readFileSync(lock)
  const clientPath = path.join(f.bundle, 'client.js'), currentClient = fs.readFileSync(clientPath)
  // Migrate a receipt-owned legacy client to the status-only client.
  fs.writeFileSync(clientPath, "const base = /* HACHIMODOCK_MANAGED_BASE */ 'http://127.0.0.1:17892'\nmodule.exports = { base }\n")
  const first = f.run(); assert.equal(first.ok, true); assert.equal(first.changed, true)
  const firstPatch = fs.readFileSync(f.patch)
  assert.equal(f.run().changed, false, 'unchanged bundle must not churn port, patch or backups')
  fs.writeFileSync(clientPath, currentClient)
  const update = f.run(); assert.equal(update.ok, true); assert.equal(update.changed, true)
  assert.notDeepEqual(fs.readFileSync(f.patch), firstPatch)
  assert.equal(f.run().changed, false)
  assert.equal(f.run('--rollback').ok, true)
  assert.deepEqual(fs.readFileSync(f.patch), firstPatch)
  assert.deepEqual(fs.readFileSync(lock), beforeLock, 'never acquire, read or remove the host lock')
  assert.equal(fs.readFileSync(path.join(f.dshHome, 'credentials.json'), 'utf8'), 'synthetic-do-not-read')
  assert.equal(fs.readFileSync(path.join(f.profile, 'sessions.json'), 'utf8'), 'synthetic-do-not-read')
})

test('UTF8 BOM metadata and CRLF survive fresh install, repeat and exact rollback', t => {
  const f = fixture(t, { bom: true })
  assert.equal(f.run().ok, true)
  assert.ok(fs.readFileSync(f.patch, 'utf8').startsWith('\uFEFF# 用户注释：保留\r\n'))
  assert.equal(f.run().changed, false)
  assert.equal(f.run('--rollback').ok, true)
  assert.deepEqual(fs.readFileSync(f.patch), f.original)
})

test('official DSH_HOME selects only the explicit synthetic profile', t => {
  const f = fixture(t, { override: true })
  assert.equal(f.run().ok, true)
  assert.ok(fs.readFileSync(f.patch, 'utf8').includes('HACHIMODOCK MANAGED BRIDGE'))
  assert.equal(fs.existsSync(path.join(f.home, '.dsh')), false, 'never scan or create an alternate profile')
})

test('foreign bridge entries and an installer-owned busy lock remain untouched', t => {
  const f = fixture(t)
  const foreign = Buffer.from('- insert:\n    - id: hachimodock-bridge-foreign\n      name: foreign\n')
  fs.writeFileSync(f.patch, foreign)
  assert.equal(f.run().code, 'foreign-bridge-entry')
  assert.deepEqual(fs.readFileSync(f.patch), foreign)
  fs.writeFileSync(f.patch, f.original)
  const managed = path.join(f.profile, '.hachimodock-managed')
  f.write(path.join(managed, 'install.lock'), 'another installer')
  assert.equal(f.run().ok, false)
  assert.equal(fs.readFileSync(path.join(managed, 'install.lock'), 'utf8'), 'another installer')
  assert.deepEqual(fs.readFileSync(f.patch), f.original)
})

test('invalid UTF8 is rejected rather than rewritten as replacement characters', t => {
  const f = fixture(t)
  const malformed = Buffer.from([0x23, 0x20, 0xff, 0x0a, 0x5b, 0x5d, 0x0a])
  fs.writeFileSync(f.patch, malformed)
  assert.equal(f.run().code, 'unsupported-profile-encoding')
  assert.deepEqual(fs.readFileSync(f.patch), malformed)
})

test('BOM directly before an empty sequence is preserved through installation and rollback', t => {
  const f = fixture(t)
  const original = Buffer.from('\uFEFF[]\r\n')
  fs.writeFileSync(f.patch, original)
  assert.equal(f.run().ok, true)
  assert.equal(f.run('--rollback').ok, true)
  assert.deepEqual(fs.readFileSync(f.patch), original)
})

test('tampered managed plugin is refused without replacing user metadata', t => {
  const f = fixture(t)
  assert.equal(f.run().ok, true)
  const before = fs.readFileSync(f.patch)
  const managed = path.join(f.profile, '.hachimodock-managed')
  const digest = fs.readdirSync(managed).find(n => /^[a-f0-9]{64}$/.test(n))
  fs.appendFileSync(path.join(managed, digest, 'client.js'), '\n// foreign modification\n')
  assert.equal(f.run().code, 'managed-package-changed')
  assert.deepEqual(fs.readFileSync(f.patch), before)
})

test('symbolic profile metadata is refused without following its target', { skip: process.platform === 'win32' }, t => {
  const f = fixture(t)
  const target = path.join(f.home, 'unrelated.yml'); f.write(target, f.original)
  fs.unlinkSync(f.patch); fs.symlinkSync(target, f.patch)
  assert.equal(f.run().code, 'unsafe-profile-entry')
  assert.deepEqual(fs.readFileSync(target), f.original)
})
