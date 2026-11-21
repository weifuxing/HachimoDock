'use strict'

// Synthetic live events -> authenticated DSH bridge -> public local Agent Bus
// -> production display queue. No transcript, model, real app or device access.
const test = require('node:test')
const assert = require('node:assert/strict')
const fs = require('node:fs')
const os = require('node:os')
const path = require('node:path')
const { pathToFileURL } = require('node:url')
const { boundString } = require('../server')
const { createFakeCtx, createFakeSession, createFakeAgent, appendSessionEvent, startTestBridge } = require('./helpers')
const { DshAdapter } = require('../../../pc/src-tauri/bridge/packages/agent-session-bus/src/adapters/dsh')
const { createAgentSessionBus } = require('../../../pc/src-tauri/bridge/packages/agent-session-bus/src')

test('native discovery wires the same DSH_HOME and Windows USERPROFILE contract as the installer', () => {
  const rust = fs.readFileSync(path.resolve(__dirname, '../../../pc/src-tauri/src/lib.rs'), 'utf8')
  const detection = rust.slice(rust.indexOf('fn find_dsh_profile_dir('), rust.indexOf('fn detect_hermes('))
  assert.match(detection, /dsh_paths::profile_dir\(home, env::var_os\("DSH_HOME"\)/)
  assert.match(detection, /dsh_paths::user_home\(cfg!\(windows\), env::var_os\("HOME"\).*env::var_os\("USERPROFILE"\)/)
  assert.doesNotMatch(detection, /get_home_dir\(\)/)
})

test('UTF8 summaries stay valid at emoji truncation boundaries', () => {
  for (const limit of [160, 200, 383]) {
    const text = '中'.repeat(limit - 2) + '🙂' + '后续文字'
    const clipped = boundString(text, limit)
    assert.equal(Buffer.from(clipped, 'utf8').toString('utf8'), clipped, 'no unpaired surrogate may reach native JSON decoding')
    assert.ok(clipped.length <= limit)
    assert.ok(clipped.endsWith('…'))
  }
})

test('live Chinese DSH session reaches shared bus and visible queue with authentication, without history reads', async t => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'dsh-ingress-'))
  t.after(() => fs.rmSync(tmp, { recursive: true, force: true }))
  const capability = path.join(tmp, 'private', 'capability.json')
  const session = createFakeSession('synthetic-session', { cwd: 'C:\\用户 测试\\项目' })
  Object.defineProperty(session, 'events', { get() { throw Error('must not read history') } })
  const ctx = createFakeCtx({ sessions: [session] })
  const agent = createFakeAgent(session); agent.status = 'running'; ctx.addAgent(agent)
  const { url } = await startTestBridge(t, ctx, { bootstrapFile: capability, writeBootstrap: true })
  const adapter = new DshAdapter({ env: { HACHIMODOCK_DSH_CAPABILITY_FILE: capability } })
  const bus = createAgentSessionBus({ port: 0, adapters: [adapter], log() {} })
  const port = await bus.start(); t.after(() => bus.stop())
  const endpoint = `http://127.0.0.1:${port}/agent/sessions?agentId=deepseek-harness`
  const read = async () => {
    const response = await fetch(endpoint, { headers: bus.capability.headers('read') })
    assert.equal(response.status, 200)
    return response.json()
  }
  assert.equal((await fetch(`${url}/sessions`)).status, 401)
  assert.equal((await fetch(endpoint)).status, 401)
  const display = await import(pathToFileURL(path.resolve(__dirname, '../../../pc/src/lib/p4-session-service.js')).href)
  let result = await read()
  assert.equal(result.ready, true)
  assert.equal(result.sessions[0].state, 'unknown')
  assert.equal(result.sessions[0].activitySource, 'live-agent-status')
  assert.equal(display.buildP4ConversationQueue(result.sessions, [], Date.now()).length, 1, 'mid-turn attachment with verified running agent is visible')

  appendSessionEvent(ctx, session, { type: 'turn/start', data: { turn: 1 } })
  appendSessionEvent(ctx, session, { type: 'session/title', data: { title: '中文任务🙂' } })
  appendSessionEvent(ctx, session, { type: 'assistant/message', data: { turn: 1, message: {
    content: [{ type: 'reasoning', text: 'synthetic-private-reasoning' }, { type: 'text', text: '正在验证中文气泡🙂' }],
  } } })
  result = await read()
  assert.equal(result.sessions[0].state, 'working')
  assert.equal(result.sessions[0].displayContent, '正在验证中文气泡🙂')
  assert.equal(result.sessions[0].cwd, 'C:\\用户 测试\\项目')
  assert.ok(!JSON.stringify(result).includes('synthetic-private-reasoning'))
  const queue = display.buildP4ConversationQueue(result.sessions, [], Date.now())
  assert.equal(queue.length, 1); assert.equal(queue[0].name, '中文任务🙂')

  appendSessionEvent(ctx, session, { type: 'turn/end', data: { turn: 1, reason: { kind: 'aborted' } } })
  result = await read()
  assert.equal(result.sessions[0].state, 'stopped', 'cancel is not task completion')
  assert.equal(display.buildP4ConversationQueue(result.sessions, queue, Date.now()).length, 1)
})
