import { test } from 'node:test'
import assert from 'node:assert/strict'
import { backoffDelay, createScheduler } from '../src/lib/scheduler.js'

// Fake clock: timers run when advance() passes their due time
function fakeTimers() {
  let now = 0, seq = 0
  const timers = new Map()
  return {
    setTimer: (fn, ms) => { const id = ++seq; timers.set(id, { at: now + ms, fn }); return id },
    clearTimer: (id) => { timers.delete(id) },
    async advance(ms) {
      const end = now + ms
      for (;;) {
        const next = [...timers.entries()].sort((a, b) => a[1].at - b[1].at)[0]
        if (!next || next[1].at > end) break
        now = next[1].at
        timers.delete(next[0])
        await next[1].fn()
        await new Promise(r => setImmediate(r))
      }
      now = end
    },
    get now() { return now },
    pending: () => timers.size,
  }
}

test('backoff doubles from base and is capped, jitter ±20 %', () => {
  const mid = () => 0.5                       // jitter factor 1.0
  assert.equal(backoffDelay(0, 3000, 30000, mid), 3000)
  assert.equal(backoffDelay(1, 3000, 30000, mid), 3000)
  assert.equal(backoffDelay(2, 3000, 30000, mid), 6000)
  assert.equal(backoffDelay(3, 3000, 30000, mid), 12000)
  assert.equal(backoffDelay(5, 3000, 30000, mid), 30000)
  assert.equal(backoffDelay(20, 3000, 30000, mid), 30000)
  assert.equal(backoffDelay(2, 3000, 30000, () => 0), 4800)     // −20 %
  assert.equal(backoffDelay(2, 3000, 30000, () => 1), 7200)     // +20 %
  assert.equal(backoffDelay(9, 3000, 30000, () => 1), 30000)    // never above max
})

test('tasks start at their offset and repeat at their interval', async () => {
  const t = fakeTimers()
  const runs = []
  const s = createScheduler({ ...t, isHidden: () => false })
  s.add('a', () => { runs.push(['a', t.now]) }, { every: 1000, offset: 0 })
  s.add('b', () => { runs.push(['b', t.now]) }, { every: 5000, offset: 1500 })
  s.start()
  await t.advance(6600)
  assert.deepEqual(runs.filter(r => r[0] === 'b').map(r => r[1]), [1500, 6500])
  assert.deepEqual(runs.filter(r => r[0] === 'a').map(r => r[1]), [0, 1000, 2000, 3000, 4000, 5000, 6000])
})

test('interval may be a function (route dependent)', async () => {
  const t = fakeTimers()
  let every = 1000, n = 0
  const s = createScheduler({ ...t, isHidden: () => false })
  s.add('rf', () => { n++ }, { every: () => every })
  s.start()
  await t.advance(2500)          // 0, 1000, 2000
  assert.equal(n, 3)
  every = 5000
  await t.advance(10000)         // 3000 (planned before change), then 8000
  assert.equal(n, 5)
})

test('failures back off exponentially and reset on success', async () => {
  const t = fakeTimers()
  let fail = true
  const at = []
  const s = createScheduler({ ...t, isHidden: () => false, rnd: () => 0.5, backoffBaseMs: 3000, maxBackoffMs: 30000 })
  s.add('rf', () => { at.push(t.now); if (fail) throw new Error('offline') }, { every: 1000 })
  s.start()
  await t.advance(60000)
  // 0, +3 s, +6 s, +12 s, +24 s, +30 s …
  assert.deepEqual(at.slice(0, 6), [0, 3000, 9000, 21000, 45000, 75000].filter(x => x <= 60000).slice(0, 6))
  assert.equal(s.failures('rf'), at.length)
  fail = false
  await s.kick('rf')
  assert.equal(s.failures('rf'), 0)
  const before = at.length
  await t.advance(3000)
  assert.equal(at.length, before + 3)   // back to 1 s
})

test('a task never overlaps itself, kick() is ignored while in flight', async () => {
  const t = fakeTimers()
  let running = 0, maxParallel = 0, release
  const s = createScheduler({ ...t, isHidden: () => false })
  s.add('slow', async () => {
    running++; maxParallel = Math.max(maxParallel, running)
    await new Promise(r => { release = r })
    running--
  }, { every: 100 })
  s.start()
  const first = t.advance(0)                 // starts the first run, which blocks
  await new Promise(r => setImmediate(r))
  assert.equal(await s.kick('slow'), false)
  release(); await first
  assert.equal(maxParallel, 1)
})

test('hidden tab: ticks are skipped, resumeHidden() restarts them', async () => {
  const t = fakeTimers()
  let hidden = false, n = 0
  const s = createScheduler({ ...t, isHidden: () => hidden })
  s.add('rf', () => { n++ }, { every: 1000 })
  s.start()
  await t.advance(10)
  assert.equal(n, 1)
  hidden = true
  await t.advance(10000)
  assert.equal(n, 1)
  assert.equal(t.pending(), 0)               // nothing keeps firing in the background
  hidden = false
  s.resumeHidden()
  await t.advance(10)
  assert.equal(n, 2)
})

test('stop() cancels everything, start() after stop works', async () => {
  const t = fakeTimers()
  let n = 0
  const s = createScheduler({ ...t, isHidden: () => false })
  s.add('x', () => { n++ }, { every: 1000 })
  s.start(); await t.advance(10)
  s.stop()
  assert.equal(t.pending(), 0)
  await t.advance(5000)
  assert.equal(n, 1)
  assert.equal(await s.kick('x'), false)     // stopped: no requests
  s.start(); await t.advance(10)
  assert.equal(n, 2)
})
