'use strict'
const test = require('node:test')
const assert = require('node:assert/strict')
const fs = require('node:fs')
const os = require('node:os')
const path = require('node:path')
const { createFakeCtx, createFakeSession, appendSessionEvent, startTestBridge } = require('./helpers')
const { startDshMonitor } = require('../../../pc/src-tauri/bridge/agents/dsh-status-monitor')

function append(ctx, session, type, data) { appendSessionEvent(ctx, session, { type, data }) }
const message = (turn, text) => ({ turn, message: { content: [{ type: 'text', text }] } })

test('answer cache binds live Session identity and current turn, rejecting delayed old answers', async t => {
  const session = createFakeSession('synthetic-same-id')
  const ctx = createFakeCtx({ sessions: [session] })
  const { bridge } = await startTestBridge(t, ctx)
  append(ctx, session, 'turn/start', { turn: 1 })
  append(ctx, session, 'assistant/message', message(1, 'first answer'))
  append(ctx, session, 'turn/end', { turn: 1, reason: { kind: 'completed' } })
  assert.equal(bridge.summarizeSession(session).displayContent, 'first answer')
  append(ctx, session, 'turn/start', { turn: 2 })
  assert.equal(bridge.summarizeSession(session).displayContent, '')
  append(ctx, session, 'assistant/message', message(1, 'delayed first answer'))
  assert.equal(bridge.summarizeSession(session).displayContent, '')
  append(ctx, session, 'turn/end', { turn: 2, reason: { kind: 'completed' } })
  assert.equal(bridge.summarizeSession(session).displayContent, '', 'no-text completion must not recycle preceding answer')
  append(ctx, session, 'turn/start', { turn: 3 })
  append(ctx, session, 'assistant/message', message(3, 'third answer'))
  assert.equal(bridge.summarizeSession(session).displayContent, 'third answer')
  const replacement = createFakeSession(session.id)
  assert.equal(bridge.summarizeSession(replacement).displayContent, '', 'recreated same-id session has no previous body')
  ctx.dispatchSync('session/disposed', session)
  assert.equal(bridge.summarizeSession(session).displayContent, '')
})

test('real Worker polls authenticated synthetic plugin, keeps hooks responsive and fences disable', { timeout: 45000 }, async t => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'dsh-follow-worker-'))
  const capability = path.join(tmp, 'private', 'capability.json')
  const session = createFakeSession('synthetic-worker')
  Object.defineProperty(session, 'events', { get() { throw Error('history must never be read') } })
  const ctx = createFakeCtx({ sessions: [session] })
  const { bridge, url } = await startTestBridge(t, ctx, { writeBootstrap: true, bootstrapFile: capability })
  const received = [], warnings = []
  const monitor = startDshMonitor({ env: { ...process.env, HACHIMODOCK_DSH_CAPABILITY_FILE: capability },
    onState(p) { received.push(p) }, onUnavailable() { warnings.push(true) } })
  t.after(async () => { await monitor.stop(); fs.rmSync(tmp, { recursive: true, force: true }) })
  async function until(predicate) {
    const deadline = Date.now() + 15000
    while (!predicate()) {
      if (Date.now() > deadline) assert.fail('synthetic Worker transition timeout')
      await new Promise(resolve => setTimeout(resolve, 25))
    }
  }
  append(ctx, session, 'turn/start', { turn: 1 })
  await until(() => received.length > 0)
  assert.equal(received[0].state, 'working')
  assert.equal((await fetch(`${url}/sessions`)).status, 401)
  append(ctx, session, 'assistant/message', message(1, 'Chinese 中文🙂 <think>private-thought</think> password=private-secret'))
  append(ctx, session, 'turn/end', { turn: 1, reason: { kind: 'completed' } })
  await until(() => received.some(p => p.state === 'done'))
  assert.ok(received.at(-1).display.content.includes('中文🙂'))
  assert.ok(!JSON.stringify(received).includes('private-secret'))
  assert.ok(!JSON.stringify(received).includes('private-thought'))
  assert.equal(bridge.server.hasEventSubscriber(), false, 'read-only polling never claims approval/SSE receiver')
  assert.deepEqual(warnings, [])
  await monitor.stop()
  const count = received.length
  append(ctx, session, 'turn/start', { turn: 2 })
  await new Promise(resolve => setTimeout(resolve, 100))
  assert.equal(received.length, count)
})
