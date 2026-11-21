'use strict'

// Private local bootstrap. No credential is accepted from URL/query parameters.
const fs = require('node:fs')
const path = require('node:path')
const os = require('node:os')
const { randomBytes, timingSafeEqual } = require('node:crypto')
const { isMainThread, parentPort, Worker, workerData } = require('node:worker_threads')
const { windowsAcl, windowsPath } = require('./adapters/windows-acl')
const secret = () => randomBytes(32).toString('hex')
const descriptorPath = (role = 'read') => path.join(process.env.HACHIMODOCK_BRIDGE_CAPABILITY_DIR || path.join(os.homedir(), '.claw-pet', 'bridge-runtime'), role === 'hook' ? 'hook.json' : 'client.json')
function equal(a, b) {
  return typeof a === 'string' && typeof b === 'string' && Buffer.byteLength(a) === Buffer.byteLength(b) && timingSafeEqual(Buffer.from(a), Buffer.from(b))
}
// Dependency injection exercises the real writer ordering and reader guards on
// non-Windows hosts; it is not a Windows filesystem/PowerShell execution claim.
function createCapabilityStore(options = {}) {
  const io = options.fs || fs, win = (options.platform || process.platform) === 'win32'
  const paths = win ? path.win32 : path
  const acl = options.acl || windowsAcl, mint = options.secret || secret
  const uid = options.uid || (() => process.getuid?.())
  function privateStat(file, directory = false) {
    const st = io.lstatSync(file)
    if (st.isSymbolicLink() || (directory ? !st.isDirectory() : !st.isFile()) ||
        (!win && (uid() !== undefined && st.uid !== uid() || st.mode & 0o077))) throw new Error('unsafe capability permissions')
    return st
  }
  function existing(file) {
    try { return io.lstatSync(file) } catch (e) { if (e.code === 'ENOENT') return null; throw e }
  }
  function prepareDirectory(file = descriptorPath()) {
    if (win) { file = windowsPath(file); acl('prepare-directory', paths.dirname(file)) }
    else io.mkdirSync(paths.dirname(file), { recursive: true, mode: 0o700 })
    privateStat(paths.dirname(file), true)
    if (existing(file)) {
      privateStat(file)
      // A disclosed existing token is never read/reused. Repair only its ACL
      // after the directory is private, then replace with a newly minted epoch.
      if (win) acl('protect-file', file)
    }
  }
  function readDescriptor(file = descriptorPath()) {
    if (win) { file = windowsPath(file); acl('verify-file', file) }
    privateStat(paths.dirname(file), true)
    const fd = io.openSync(file, io.constants.O_RDONLY | (io.constants.O_NOFOLLOW || 0))
    try {
      const before = privateStat(file), st = io.fstatSync(fd)
      if (!st.isFile() || st.ino !== before.ino || st.dev !== before.dev || st.size > 4096 || !win && (uid() !== undefined && st.uid !== uid() || st.mode & 0o077)) throw new Error('unsafe capability descriptor')
      if (win) acl('verify-file', file)
      const d = JSON.parse(io.readFileSync(fd, 'utf8'))
      if (d.version !== 1 || !/^[a-f0-9]{64}$/.test(d.epoch) ||
          ![d.busPort, d.hookPort].every(p => p === null || Number.isInteger(p) && p > 0 && p <= 65535) ||
          !d.tokens || !Object.keys(d.tokens).length ||
          !Object.entries(d.tokens).every(([k, v]) => ['read', 'control', 'hook'].includes(k) && /^[a-f0-9]{64}$/.test(v))) throw new Error('invalid bridge capability descriptor')
      return d
    } finally { io.closeSync(fd) }
  }
  function writeDescriptor(d, file = descriptorPath()) {
    prepareDirectory(file)
    const dir = paths.dirname(file), tmp = paths.join(dir, `.capability-${mint()}.tmp`)
    let fd, created = false
    try {
      // Windows creates the empty temp with explicit owner + protected DACL,
      // rather than relying on the elevated token's default object owner.
      if (win) { acl('create-file', tmp); created = true }
      fd = io.openSync(tmp, win ? 'r+' : 'wx', 0o600)
      created = true
      if (win) {
        acl('verify-file', tmp)
        const st = privateStat(tmp), opened = io.fstatSync(fd)
        if (!opened.isFile() || st.ino !== opened.ino || st.dev !== opened.dev || opened.size !== 0) throw new Error('unsafe new capability file')
      }
      io.writeFileSync(fd, JSON.stringify(d)); io.fsyncSync(fd)
    } catch (e) {
      if (fd !== undefined) io.closeSync(fd)
      // CreateNew rejection must never delete a preexisting entry.
      if (created && existing(tmp)) io.unlinkSync(tmp)
      throw e
    }
    io.closeSync(fd)
    try {
      io.renameSync(tmp, file)
      if (win) acl('verify-file', file)
      else privateStat(file)
    } finally { if (existing(tmp)) io.unlinkSync(tmp) }
  }
  return { readDescriptor, writeDescriptor, prepareDirectory }
}
const store = createCapabilityStore()
function headersFor(role, port) {
  const d = store.readDescriptor(descriptorPath(role))
  if (!(role === 'hook' ? d.hookPort === port : d.busPort === port || d.hookPort === port) || !d.tokens[role]) throw new Error('bridge endpoint unavailable')
  return { Authorization: `Bearer ${d.tokens[role]}` }
}
function createCapability(bootstrap) {
  const { epoch, tokens } = bootstrap || {
    epoch: secret(), tokens: { read: secret(), control: secret(), hook: secret() },
  }
  return {
    headers(role) {
      if (!Object.hasOwn(tokens, role)) throw new Error('invalid bridge role')
      return { Authorization: `Bearer ${tokens[role]}` }
    },
    authorize(req, roles) {
      const h = req.headers.authorization
      return typeof h === 'string' && roles.some(role => equal(h, `Bearer ${tokens[role]}`))
    },
    publish({ busPort = null, hookPort = null }) {
      const common = { version: 1, epoch, busPort, hookPort }
      store.writeDescriptor({ ...common, tokens: { read: tokens.read, control: tokens.control } }, descriptorPath('read'))
      store.writeDescriptor({ ...common, tokens: { hook: tokens.hook } }, descriptorPath('hook'))
    },
    publishInWorker(ports, onUnavailable) {
      let worker
      try {
        worker = new Worker(__filename, {
          workerData: { publishBridgeCapability: true, epoch, tokens, ports },
        })
      } catch {
        onUnavailable('worker_start')
        return () => {}
      }
      worker.once('message', failure => { if (failure) onUnavailable(failure) })
      worker.once('error', () => onUnavailable('worker_error'))
      worker.unref()
      return () => { void worker.terminate().catch(() => {}) }
    },
    clear() {
      for (const role of ['read', 'hook']) {
        const file = descriptorPath(role)
        try { if (store.readDescriptor(file).epoch === epoch) fs.unlinkSync(file) } catch { /* stale or missing: never remove a successor */ }
      }
    },
  }
}

if (!isMainThread && workerData?.publishBridgeCapability === true) {
  const { epoch, tokens, ports } = workerData
  try {
    createCapability({ epoch, tokens }).publish(ports)
    parentPort.postMessage(null)
  } catch (error) {
    parentPort.postMessage(error?.code === 'WINDOWS_CAPABILITY_ACL_REJECTED'
      ? error.message : 'descriptor_unavailable')
  }
}

async function readManagedBootstrap(input) {
  let data = ''
  for await (const chunk of input) {
    data += chunk
    if (data.length > 1024) throw new Error('invalid managed bridge bootstrap')
  }
  let bootstrap
  try { bootstrap = JSON.parse(data) } catch { throw new Error('invalid managed bridge bootstrap') }
  const { epoch, tokens } = bootstrap || {}
  const values = [epoch, tokens?.read, tokens?.control, tokens?.hook]
  if (!values.every(value => typeof value === 'string' && /^[a-f0-9]{64}$/.test(value)) ||
      new Set(values).size !== values.length) throw new Error('invalid managed bridge bootstrap')
  return { epoch, tokens: { read: tokens.read, control: tokens.control, hook: tokens.hook } }
}
module.exports = { secret, equal, descriptorPath, createCapabilityStore, headersFor, createCapability, readManagedBootstrap, ...store }
