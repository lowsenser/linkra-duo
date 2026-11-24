// Central poll scheduler (review 07 H1, M2, M6).
// - every task has its own interval (may depend on route/mode) and a start offset,
//   so requests never fire in the same tick
// - a task never overlaps itself (in-flight guard, also for kick())
// - failures back off exponentially with jitter up to maxBackoffMs
// - paused while the tab is hidden; kick() runs a task at once (e.g. when the tab returns)
// Timers, clock and visibility are injected so the logic is unit-testable.

/** Exponential backoff: base, 2×base, 4×base … capped at max, ±jitter */
export function backoffDelay(failures, baseMs, maxMs, rnd = Math.random, jitter = 0.2) {
  if (failures <= 0) return baseMs
  const raw = Math.min(maxMs, baseMs * 2 ** (failures - 1))
  const j = 1 + (rnd() * 2 - 1) * jitter
  return Math.round(Math.min(maxMs, raw * j))
}

export function createScheduler({
  setTimer = (fn, ms) => setTimeout(fn, ms),
  clearTimer = (id) => clearTimeout(id),
  isHidden = () => typeof document !== 'undefined' && document.hidden,
  rnd = Math.random,
  backoffBaseMs = 3000,
  maxBackoffMs = 30000,
} = {}) {
  const tasks = new Map()
  let running = false

  const intervalOf = (t) => (typeof t.every === 'function' ? t.every() : t.every)

  function nextDelay(t) {
    const base = intervalOf(t)
    if (!t.failures) return base
    return Math.max(base, backoffDelay(t.failures, Math.max(base, backoffBaseMs), maxBackoffMs, rnd))
  }

  function plan(t, ms) {
    if (t.timer != null) clearTimer(t.timer)
    t.timer = running ? setTimer(() => tick(t), ms) : null
  }

  async function run(t) {
    if (t.busy) return false
    t.busy = true
    try {
      await t.fn()
      t.failures = 0
      t.onResult?.(true)
    } catch (e) {
      t.failures++
      t.onResult?.(false, e)
    } finally {
      t.busy = false
    }
    return true
  }

  async function tick(t) {
    t.timer = null
    if (!running) return
    if (isHidden()) { t.missed = true; return }       // resumed by kickAll() on visibility
    await run(t)
    if (running && t.timer == null) plan(t, nextDelay(t))
  }

  return {
    /** fn must throw on failure. every: ms or () => ms. offset: first run delay. */
    add(name, fn, { every, offset = 0, onResult } = {}) {
      const t = { name, fn, every, offset, onResult, failures: 0, busy: false, timer: null, missed: false }
      tasks.set(name, t)
      if (running) plan(t, offset)
      return t
    },
    start() {
      if (running) return
      running = true
      for (const t of tasks.values()) plan(t, t.offset)
    },
    stop() {
      running = false
      for (const t of tasks.values()) { if (t.timer != null) clearTimer(t.timer); t.timer = null; t.failures = 0 }
    },
    /** Run now (if not in flight) and restart its interval */
    async kick(name) {
      const t = tasks.get(name)
      if (!t || !running) return false
      if (t.busy) return false
      if (t.timer != null) { clearTimer(t.timer); t.timer = null }
      const ran = await run(t)
      if (running && t.timer == null) plan(t, nextDelay(t))
      return ran
    },
    /** Re-plan a task whose interval just changed (e.g. route switch) */
    reschedule(name) {
      const t = tasks.get(name)
      if (t && running && !t.busy) plan(t, Math.min(nextDelay(t), 500))
    },
    /** After the tab returns: run every task that missed a tick, staggered by its offset */
    resumeHidden() {
      for (const t of tasks.values()) {
        if (t.missed || t.timer == null) { t.missed = false; if (!t.busy) plan(t, Math.min(t.offset, 1500)) }
      }
    },
    failures: (name) => tasks.get(name)?.failures ?? 0,
    isRunning: () => running,
    _tasks: tasks,
  }
}
