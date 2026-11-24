import { test } from 'node:test'
import assert from 'node:assert/strict'
import { createApi, toApiError, needsCsrf, setUnauthorizedHandler, ApiError } from '../src/lib/http.js'
import { parseHash } from '../src/lib/router.js'
import { pruneDevices } from '../src/lib/format.js'

const response = (status, body) => ({ ok: status >= 200 && status < 300, status, text: async () => typeof body === 'string' ? body : JSON.stringify(body) })

test('CSRF header on every request except GET/HEAD', async () => {
  const seen = []
  const api = createApi(async (path, init) => { seen.push([init.method || 'GET', init.headers['X-Linkra']]); return response(200, { ok: true }) })
  await api('/api/system')
  await api('/api/x', { method: 'HEAD' })
  await api('/api/x', { json: { a: 1 } })
  await api('/api/x', { method: 'DELETE' })
  await api('/api/x', { method: 'POST', body: 'raw' })
  assert.deepEqual(seen, [['GET', undefined], ['HEAD', undefined], ['POST', '1'], ['DELETE', '1'], ['POST', '1']])
  assert.equal(needsCsrf('put'), true)
  assert.equal(needsCsrf(undefined), false)
})

test('error mapping: contract format, legacy format, plain status', () => {
  let e = toApiError(429, { error: { code: 'locked', message: 'Zu viele Versuche', retry_after_s: 30 } })
  assert.equal(e.code, 'locked'); assert.equal(e.message, 'Zu viele Versuche'); assert.equal(e.retryAfter, 30); assert.equal(e.status, 429)
  e = toApiError(400, { error: 'SSID required' })
  assert.equal(e.message, 'SSID required'); assert.equal(e.code, null)
  e = toApiError(500, null)
  assert.equal(e.message, 'Fehler im Gerät')
  e = toApiError(401, '<html>')
  assert.equal(e.code, 'unauthorized')
  assert.ok(e instanceof ApiError)
})

test('401 unauthorized calls the handler once; login errors do not', async () => {
  let calls = 0
  setUnauthorizedHandler(() => { calls++ })
  const api = createApi(async (path) => path === '/api/auth/login'
    ? response(401, { error: { code: 'invalid_credentials', message: 'falsch' } })
    : path === '/api/auth/password'
      ? response(401, { error: { code: 'invalid_credentials', message: 'falsch' } })
      : response(401, { error: { code: 'unauthorized', message: 'Anmeldung erforderlich' } }))
  await assert.rejects(api('/api/system'), (e) => e.code === 'unauthorized')
  assert.equal(calls, 1)
  await assert.rejects(api('/api/auth/login', { json: {} }), (e) => e.code === 'invalid_credentials')
  await assert.rejects(api('/api/auth/password', { json: {} }), (e) => e.code === 'invalid_credentials')
  assert.equal(calls, 1)
  setUnauthorizedHandler(null)
})

test('network failure and timeout become readable errors', async () => {
  const api = createApi(async () => { throw new TypeError('fetch failed') })
  await assert.rejects(api('/api/x'), (e) => e.code === 'network' && /nicht erreichbar/.test(e.message))
  const slow = createApi((path, init) => new Promise((_, rej) => init.signal.addEventListener('abort', () => rej(Object.assign(new Error('x'), { name: 'AbortError' })))))
  await assert.rejects(slow('/api/x', { timeoutMs: 20 }), (e) => /Zeitüberschreitung/.test(e.message))
})

test('hash routing never throws', () => {
  assert.deepEqual(parseHash('#devices/%E0'), { page: 'overview', arg: null })
  assert.deepEqual(parseHash('#devices/Nexus-TH_211'), { page: 'devices', arg: 'Nexus-TH_211' })
  assert.deepEqual(parseHash('#/settings/account'), { page: 'settings', arg: 'account' })
  assert.deepEqual(parseHash(''), { page: 'overview', arg: null })
  assert.deepEqual(parseHash('#<script>'), { page: 'overview', arg: null })
  assert.deepEqual(parseHash('#devices/a%2Fb'), { page: 'devices', arg: 'a/b' })
})

test('device map stays bounded; saved devices are never dropped', () => {
  const now = 100 * 3600e3
  const m = new Map()
  m.set('saved', { key: 'saved', persisted: true, lastSeen: 0 })
  m.set('old', { key: 'old', lastSeen: now - 25 * 3600e3 })
  for (let i = 0; i < 10; i++) m.set(`d${i}`, { key: `d${i}`, lastSeen: now - i * 1000 })
  const same = pruneDevices(m, now, { max: 100 })
  assert.equal(same.has('old'), false); assert.equal(same.has('saved'), true); assert.equal(same.size, 11)
  const small = pruneDevices(m, now, { max: 5 })
  assert.equal(small.size, 5)
  assert.equal(small.has('saved'), true)
  assert.deepEqual([...small.keys()].filter(k => k !== 'saved').sort(), ['d0', 'd1', 'd2', 'd3'])
  const untouched = new Map([['a', { key: 'a', lastSeen: now }]])
  assert.equal(pruneDevices(untouched, now), untouched)
})
