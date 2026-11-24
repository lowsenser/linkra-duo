#!/usr/bin/env node
// Linkra Duo – development mock of the gateway API.
// Implements the sign-in contract (docs/auth-api.md) including TOTP with a
// fixed test secret, plus enough of the data API (system, wifi, rf, mqtt,
// devices, SSE) to click through the whole UI without hardware.
//
//   node mock/server.js [--port 8787] [--account] [--totp] [--unsynced] [--ttl 120]
//
//   --account   start with an existing account (admin / linkra-test-pw) instead of the setup wizard
//   --totp      … with 2FA already active (needs --account)
//   --unsynced  device clock "not synced": TOTP logins answer 503 time_not_synced
//   --ttl N     sessions expire after N seconds (tests the "session expired" path)
//
// Helpers (development only, not on the device):
//   GET  /api/__mock/totp      current code for the test secret
//   POST /api/__mock/expire    end all sessions now
//   POST /api/__mock/reset     back to "no account" (like the BOOT-button recovery)
//   GET  /api/__mock/offline?s=20   answer nothing for N seconds (tests the offline banner)
//
// Non-/api paths serve ../data (the production build) so `npm run build`
// output can be tested against the mock at http://localhost:8787.

import http from 'node:http'
import { randomBytes, createHash, timingSafeEqual } from 'node:crypto'
import { readFile, stat } from 'node:fs/promises'
import { fileURLToPath } from 'node:url'
import path from 'node:path'
import { verify, totp } from './totp.js'

const args = process.argv.slice(2)
const flag = (n) => args.includes(`--${n}`)
const opt = (n, d) => { const i = args.indexOf(`--${n}`); return i >= 0 && args[i + 1] ? args[i + 1] : d }

const PORT = Number(opt('port', process.env.MOCK_PORT || 8787))
const SESSION_TTL_S = Number(opt('ttl', 7 * 24 * 3600))
export const TEST_SECRET = 'JBSWY3DPEHPK3PXPJBSWY3DPEHPK3PXP'      // 20 bytes, base32
export const TEST_USER = 'admin', TEST_PASSWORD = 'linkra-test-pw'
const TEST_RECOVERY = ['k7mp-3xq9', 'h4tn-8wrc', 'p2zd-6bkm', 'x9fq-4nhs', 'c3vr-7jtw', 'm8kx-2pdq', 'w6hb-9zcn', 'r4qt-5mxv', 'f7nc-3kwp', 't2xj-8vhd']
const DATA_DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../data')

// ---------- auth state (as the firmware keeps it in NVS + RAM) ----------
const sha = (s) => createHash('sha256').update(s).digest('hex')
const auth = {
  user: null, pwHash: null,
  totpSecret: null, totpActive: false, pendingSecret: null, lastStep: -1,
  recovery: [],                                    // hashes
  timeSynced: !flag('unsynced'),
}
const sessions = new Map()                         // token -> { user, expires }
const fails = new Map()                            // ip -> { n, lockedUntil, locks }
const globalFails = { n: 0, lockedUntil: 0, locks: 0 }
let offlineUntil = 0

function resetAuth() {
  Object.assign(auth, { user: null, pwHash: null, totpSecret: null, totpActive: false, pendingSecret: null, lastStep: -1, recovery: [] })
  sessions.clear(); fails.clear(); Object.assign(globalFails, { n: 0, lockedUntil: 0, locks: 0 })
}
if (flag('account')) {
  auth.user = TEST_USER; auth.pwHash = sha(TEST_PASSWORD)
  if (flag('totp')) { auth.totpSecret = TEST_SECRET; auth.totpActive = true; auth.recovery = TEST_RECOVERY.map(sha) }
}

const eq = (a, b) => { const x = Buffer.from(String(a)), y = Buffer.from(String(b)); return x.length === y.length && timingSafeEqual(x, y) }

// ---------- helpers ----------
function send(res, status, body, headers = {}) {
  const text = typeof body === 'string' ? body : JSON.stringify(body)
  res.writeHead(status, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store', ...headers })
  res.end(text)
}
const fail = (res, status, code, message, extra = {}) => send(res, status, { error: { code, message, ...extra } })

function cookies(req) {
  const out = {}
  for (const p of (req.headers.cookie || '').split(';')) {
    const i = p.indexOf('='); if (i > 0) out[p.slice(0, i).trim()] = p.slice(i + 1).trim()
  }
  return out
}
function session(req) {
  const tok = cookies(req).linkra_session
  const s = tok && sessions.get(tok)
  if (!s) return null
  if (Date.now() > s.expires) { sessions.delete(tok); return null }
  s.expires = Date.now() + SESSION_TTL_S * 1000                   // sliding validity
  return { token: tok, ...s }
}
function newSession(user) {
  const tok = randomBytes(32).toString('hex')
  sessions.set(tok, { user, expires: Date.now() + SESSION_TTL_S * 1000, created: Date.now() })
  while (sessions.size > 8) sessions.delete(sessions.keys().next().value)   // oldest goes
  return `linkra_session=${tok}; HttpOnly; SameSite=Strict; Path=/; Max-Age=604800`
}
const clearCookie = 'linkra_session=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0'

function statusJson(s) {
  return {
    setup_required: !auth.user, authenticated: !!s, ...(s ? { user: s.user } : {}),
    totp_enabled: auth.totpActive, time_synced: auth.timeSynced,
  }
}

async function body(req) {
  let size = 0, chunks = []
  for await (const c of req) { size += c.length; if (size > 16384) throw new Error('too large'); chunks.push(c) }
  const t = Buffer.concat(chunks).toString('utf8')
  if (!t) return {}
  return JSON.parse(t)
}

// Lockout: 5 failures (per IP and global) → 30 s, doubling per further lock, max 15 min
function lockState(ip) {
  const now = Date.now()
  const f = fails.get(ip)
  const until = Math.max(f?.lockedUntil || 0, globalFails.lockedUntil)
  return until > now ? Math.ceil((until - now) / 1000) : 0
}
// the 5th failure itself still answers 401, the following requests 429 (firmware behaviour)
function registerFail(ip) {
  const f = fails.get(ip) || { n: 0, lockedUntil: 0, locks: 0 }
  for (const x of [f, globalFails]) {
    x.n++
    if (x.n >= 5) { x.locks++; x.n = 0; x.lockedUntil = Date.now() + Math.min(900, 30 * 2 ** (x.locks - 1)) * 1000 }
  }
  fails.set(ip, f)
}
const clearFails = (ip) => { fails.delete(ip); globalFails.n = 0 }
const lockedReply = (res, s) => send(res, 429, { error: { code: 'locked', message: `Zu viele Fehlversuche. Bitte ${s} s warten.`, retry_after_s: s } }, { 'Retry-After': String(s) })
const closeAllStreams = () => { for (const c of clients) c.end() }

/** Checks TOTP or recovery code; returns null on success or [status, code, message] */
function checkSecondFactor(b) {
  if (b.recovery_code) {
    const h = sha(String(b.recovery_code).toLowerCase())
    const i = auth.recovery.indexOf(h)
    if (i < 0) return [401, 'invalid_credentials', 'Wiederherstellungscode ungültig']
    auth.recovery.splice(i, 1)
    return null
  }
  if (!b.totp && !b.code) return [401, 'totp_required', 'Code aus der Authenticator-App erforderlich']
  if (!auth.timeSynced) return [503, 'time_not_synced', 'Uhrzeit des Geräts noch nicht synchronisiert']
  const st = verify(auth.totpSecret, b.totp || b.code)
  if (st < 0 || st <= auth.lastStep) return [401, 'totp_invalid', 'Code ungültig']
  auth.lastStep = st
  return null
}

// ---------- simulated radio ----------
const PROFILES = [
  { id: 'ook433-std', band: '433', modulation: 'OOK', freqMHz: 433.92, bitrateKbps: 4.8, bwKHz: 270, dwellMs: 20000, home: true, note: 'Wetterstationen, Thermometer (Nexus, Bresser, TFA …)' },
  { id: 'ook433-low', band: '433', modulation: 'OOK', freqMHz: 433.92, bitrateKbps: 2.4, bwKHz: 203, dwellMs: 8000, home: false, note: 'Langsame OOK-Sender' },
  { id: 'fsk433-ec', band: '433', modulation: 'FSK', freqMHz: 433.82, bitrateKbps: 9.6, bwKHz: 135, dwellMs: 8000, home: false, note: 'EcoWitt / Fine Offset FSK' },
  { id: 'fsk868-lacrosse', band: '868', modulation: 'FSK', freqMHz: 868.3, bitrateKbps: 17.24, bwKHz: 203, dwellMs: 15000, home: true, note: 'LaCrosse IT+, TX29' },
  { id: 'fsk868-8k', band: '868', modulation: 'FSK', freqMHz: 868.3, bitrateKbps: 8.0, bwKHz: 135, dwellMs: 8000, home: false, note: 'Bresser 868, Hideki' },
  { id: 'wmbus-t1', band: '868', modulation: 'wM-Bus', freqMHz: 868.95, bitrateKbps: 100, bwKHz: 325, dwellMs: 10000, home: false, note: 'Wasser-, Wärme- und Heizkostenzähler (wM-Bus T1/C1)' },
]
const stats = Object.fromEntries(PROFILES.map(p => [p.id, { visits: 0, activeMs: 0, decodes: 0, unparsed: 0, lastDecodeMs: 0 }]))
const radio = { '433': { idx: 0, start: Date.now(), decoded: 0, raw: 0, sens: 1 }, '868': { idx: 3, start: Date.now(), decoded: 0, raw: 0, sens: 1 } }
let rotation = true, pinned = null, pinnedUntil = 0
const BOOT = Date.now()
const bandProfiles = (b) => PROFILES.filter(p => p.band === b)
const active = (b) => bandProfiles(b)[radio[b].idx % bandProfiles(b).length]

function rotate() {
  const now = Date.now()
  if (pinned && now > pinnedUntil) pinned = null
  for (const b of ['433', '868']) {
    const r = radio[b], p = active(b)
    if (pinned) { const pi = bandProfiles(b).findIndex(x => x.id === pinned); if (pi >= 0 && pi !== r.idx) { stats[p.id].activeMs += now - r.start; r.idx = pi; r.start = now; stats[pinned].visits++ } continue }
    if (rotation && now - r.start > p.dwellMs) {
      stats[p.id].activeMs += now - r.start
      r.idx = (r.idx + 1) % bandProfiles(b).length; r.start = now
      stats[active(b).id].visits++
    }
  }
}
setInterval(rotate, 250)

const SENSORS = [
  { model: 'Nexus-TH', id: 211, channel: 1, band: '433', p: 'ook433-std', t: 21.4, h: 46 },
  { model: 'Bresser-3CH', id: 92, channel: 2, band: '433', p: 'ook433-std', t: 4.8, h: 81 },
  { model: 'Fineoffset-WH51', id: 'a1b2c3', band: '433', p: 'fsk433-ec', moisture: 37 },
  { model: 'LaCrosse-TX29IT', id: 14, band: '868', p: 'fsk868-lacrosse', t: 19.2 },
  { model: 'LaCrosse-TX29IT', id: 51, band: '868', p: 'fsk868-lacrosse', t: -2.7 },
  { model: 'wMBus-Techem-HKV', id: 41570312, band: '868', p: 'wmbus-t1', hca: 412 },
]
const db = new Map()   // device_key -> row
for (const s of SENSORS.slice(0, 3)) {
  const key = `${s.model}_${s.id}`
  db.set(key, { device_key: key, model: s.model, device_id: typeof s.id === 'number' ? s.id : -1, channel: s.channel ?? -1, temperature: s.t ?? null, humidity: s.h ?? null, battery_ok: 1, rssi: -71, frequency: s.band === '868' ? 868.3 : 433.92, signal_count: 40, mqtt_enabled: key.startsWith('Nexus') ? 1 : 0, first_seen: 1000, last_seen: 2000 })
}

const mqttCfg = { enabled: true, host: '192.168.1.10', port: 1883, tls: false, user: 'linkra', has_password: true, base_topic: 'linkra', discovery_prefix: 'homeassistant', expire_after: 3600 }
const mqttStat = { published: 1284, dropped: 0, failed: 2, reconnects: 1, connectedAt: Date.now() - 3600e3 }

function signalFrom(s) {
  const noise = (a) => +(a + (Math.random() - 0.5) * 0.4).toFixed(1)
  const prof = PROFILES.find(p => p.id === s.p)
  const msg = { model: s.model, id: s.id, decoded: true, band: s.band, frequency: prof.freqMHz, rx_profile: s.p, modulation: prof.modulation, rssi: Math.round(-60 - Math.random() * 35) }
  if (s.channel != null) msg.channel = s.channel
  if (s.t != null) { s.t = noise(s.t); msg.temperature_C = s.t }
  if (s.h != null) msg.humidity = Math.round(s.h + (Math.random() - 0.5) * 2)
  if (s.moisture != null) msg.moisture = s.moisture
  if (s.hca != null) msg.hca_current = s.hca
  if (s.model !== 'wMBus-Techem-HKV') msg.battery_ok = 1
  if (db.has(`${s.model}_${s.id}`)) msg.persisted = true
  return msg
}

// ---------- SSE ----------
const clients = new Set()
function sse(res, event, data) { res.write(`event: ${event}\ndata: ${JSON.stringify(data)}\n\n`) }
setInterval(() => {
  if (!clients.size || Date.now() < offlineUntil) return
  const f = {}
  for (const b of ['433', '868']) {
    const floor = b === '433' ? -104 : -100
    const burst = Math.random() < 0.08
    f['p' + b] = Math.round(floor + Math.random() * 6 + (burst ? 25 + Math.random() * 20 : 0))
    f['m' + b] = f['p' + b] - 3
    f['avg' + b] = floor; f['th' + b] = floor + 6
  }
  f.rx = (f.p433 > -80 ? 1 : 0) | (f.p868 > -76 ? 2 : 0)
  f.wm = active('868').modulation === 'wM-Bus' ? 1 : 0
  for (const c of clients) sse(c, 'rf_rssi', f)
}, 200)
setInterval(() => {
  if (!clients.size || Date.now() < offlineUntil) return
  const candidates = SENSORS.filter(s => active(s.band).id === s.p)
  if (!candidates.length || Math.random() < 0.3) {
    if (Math.random() < 0.3) {
      const b = Math.random() < 0.5 ? '433' : '868'
      radio[b].raw++; stats[active(b).id].unparsed++
      const raw = { model: `${b} MHz Gerät`, status: 'classified', band: b, frequency: active(b).freqMHz, rx_profile: active(b).id, modulation: active(b).modulation, rssi: -95, fingerprint: 'pwm-500/1000', duration: 41000 }
      for (const c of clients) sse(c, 'rf_signal', raw)
    }
    return
  }
  const s = candidates[Math.floor(Math.random() * candidates.length)]
  const msg = signalFrom(s)
  radio[s.band].decoded++; radio[s.band].raw++
  stats[s.p].decodes++; stats[s.p].lastDecodeMs = Date.now()
  const row = db.get(`${s.model}_${s.id}`)
  if (row) { row.signal_count++; row.last_seen = Date.now() - BOOT; row.temperature = msg.temperature_C ?? row.temperature }
  for (const c of clients) sse(c, 'rf_signal', msg)
}, 1800)

// ---------- data API ----------
function rfOverview() {
  const now = Date.now()
  const modules = ['433', '868'].map(b => {
    const p = active(b), r = radio[b]
    const floor = b === '433' ? -104 : -100
    return {
      band: b, ok: true, freqMHz: p.freqMHz, sensitivity: r.sens, rssi: Math.round(floor + Math.random() * 5), noiseFloor: floor, threshold: floor + 6,
      receiving: Math.random() < 0.1, rawSignals: r.raw, ignored: Math.floor(r.raw / 4), decoded: r.decoded, queueDrops: 0, regErrors: 0,
      profile: p.id, modulation: p.modulation, bitrateKbps: p.bitrateKbps, bwKHz: p.bwKHz, devKHz: p.modulation === 'FSK' ? 47 : 0,
      profileElapsedMs: now - r.start, profileDwellMs: p.dwellMs,
      ...(b === '868' ? { wmbus: { active: p.modulation === 'wM-Bus', syncs: 120, ok: 37, bad: 4, overflow: 0, lastError: '' } } : { forcedEnds: 0 }),
    }
  })
  return {
    rotation, uptimeMs: now - BOOT, modules, pinned, pinnedRemainingMs: pinned ? pinnedUntil - now : 0,
    profiles: PROFILES.map(p => {
      const st = stats[p.id], a = active(p.band).id === p.id
      return { ...p, visits: st.visits, activeMs: st.activeMs + (a ? now - radio[p.band].start : 0), decodes: st.decodes, unparsed: st.unparsed, lastDecodeAgoS: st.lastDecodeMs ? Math.round((now - st.lastDecodeMs) / 1000) : -1 }
    }),
  }
}

const system = () => ({
  version: '0.6.0-beta (Mock)', hardware: 'A0.1', build: 'Nov 24 2026 12:00:00', idf: 'v5.1.4', uptime: Math.round((Date.now() - BOOT) / 1000), uptime_ms: Date.now() - BOOT,
  epoch_ms: auth.timeSynced ? Date.now() : 0, free_heap: 81234, min_heap: 60211, max_alloc: 45000, heap_total: 327680, psram_total: 8388608, psram_free: 7900000,
  chip: 'ESP32-S3', chip_rev: 2, cores: 2, cpu_mhz: 240, temp_c: 41.5, flash_size: 16777216, sketch_size: 1900000, sketch_free: 4000000,
  fs_total: 1441792, fs_used: 380000, partition: 'factory', partition_size: 6291456, ota_possible: false, reset_reason: 'poweron', clients: 0, has_config: true,
})
const wifiStatus = () => ({ mode: 'connected', connected: true, ip: '192.168.1.50', ap_ip: '192.168.4.1', ssid: 'Heimnetz', bssid: 'AA:BB:CC:DD:EE:FF', rssi: -58, channel: 6, gateway: '192.168.1.1', dns: '192.168.1.1', subnet: '255.255.255.0', mac: '24:0A:C4:00:11:22', hostname: 'linkra-duo' })

async function dataRoute(req, res, url, b) {
  const p = url.pathname, m = req.method
  if (m === 'GET' && p === '/api/system') return send(res, 200, system())
  if (m === 'GET' && p === '/api/wifi/status') return send(res, 200, wifiStatus())
  if (m === 'GET' && p === '/api/rf/overview') return send(res, 200, rfOverview())
  if (m === 'GET' && p === '/api/rf/devices') return send(res, 200, [...db.values()])
  if (m === 'DELETE' && p === '/api/rf/devices') { db.clear(); return send(res, 200, { ok: true }) }
  if (m === 'POST' && p === '/api/rf/devices/persist') { db.set(b.device_key, { ...b, signal_count: 1, mqtt_enabled: 0, first_seen: Date.now() - BOOT, last_seen: Date.now() - BOOT }); return send(res, 200, { ok: true }) }
  if (m === 'POST' && p === '/api/rf/devices/delete') { db.delete(b.device_key); return send(res, 200, { ok: true }) }
  if (m === 'POST' && p === '/api/rf/profile') {
    if (b.rotation !== undefined) rotation = !!b.rotation
    if (b.profile) { pinned = b.profile; pinnedUntil = Date.now() + (b.durationMs || 60000) }
    return send(res, 200, { ok: true })
  }
  if (m === 'POST' && p === '/api/rf/sensitivity') { radio[b.module].sens = b.level; return send(res, 200, { ok: true }) }
  if (m === 'GET' && p === '/api/mqtt') return send(res, 200, { ...mqttCfg, node_id: 'linkra_001122', connected: mqttCfg.enabled, published: mqttStat.published++, dropped: 0, failed: 2, reconnects: 1, connected_for_s: Math.round((Date.now() - mqttStat.connectedAt) / 1000), last_error: '', queue: 0 })
  if (m === 'POST' && p === '/api/mqtt') { const { pass, ...rest } = b; Object.assign(mqttCfg, rest); if (pass) mqttCfg.has_password = true; return dataRoute({ method: 'GET' }, res, new URL('http://x/api/mqtt'), {}) }
  if (m === 'POST' && p === '/api/mqtt/device') { const r = db.get(b.device_key); if (!r) return fail(res, 404, 'not_found', 'Gerät nicht gespeichert'); r.mqtt_enabled = b.enabled ? 1 : 0; return send(res, 200, { ok: true }) }
  if (m === 'GET' && p === '/api/fs/list') return send(res, 200, { files: [{ path: '/index.html', size: 1350 }, { path: '/assets/index.js', size: 86000 }, { path: '/devices.db', size: 24576 }] })
  if (m === 'GET' && p === '/api/wifi/scan') { await new Promise(r => setTimeout(r, 1500)); return send(res, 200, { networks: [{ ssid: 'Heimnetz', rssi: -55, channel: 6, secure: true }, { ssid: 'Nachbar', rssi: -81, channel: 11, secure: true }] }) }
  if (m === 'POST' && (p === '/api/fs/upload' || p === '/api/fs/delete')) return send(res, 200, { ok: true })
  if (m === 'POST' && p === '/api/ota') return fail(res, 500, 'update_failed', 'OTA im Mock nicht möglich')
  if (m === 'POST' && (p === '/api/reboot' || p === '/api/config/reset' || p === '/api/wifi/settings/connect')) return send(res, 200, { ok: true })
  return fail(res, 404, 'not_found', 'Unbekannte Route (Mock)')
}

// ---------- server ----------
const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.css': 'text/css', '.webp': 'image/webp', '.svg': 'image/svg+xml', '.png': 'image/png' }
async function serveStatic(req, res, url) {
  let rel = decodeURIComponent(url.pathname)
  if (rel === '/' || !rel.startsWith('/assets/')) rel = '/index.html'
  const file = path.join(DATA_DIR, path.normalize(rel))
  if (!file.startsWith(DATA_DIR)) return fail(res, 403, 'forbidden', 'Verboten')
  try {
    await stat(file)
    res.writeHead(200, { 'Content-Type': MIME[path.extname(file)] || 'application/octet-stream', 'Cache-Control': rel === '/index.html' ? 'no-cache' : 'public, max-age=31536000, immutable' })
    res.end(await readFile(file))
  } catch { send(res, 404, 'data/ fehlt – erst `npm run build` ausführen oder `npm run dev:mock` nutzen', { 'Content-Type': 'text/plain; charset=utf-8' }) }
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://localhost')
  const p = url.pathname, m = req.method
  const ip = req.socket.remoteAddress || '?'
  if (/\.\.|\/\/|\\/.test(req.url)) return send(res, 400, 'Bad Request', { 'Content-Type': 'text/plain' })
  // origin check for every method (firmware): a foreign Origin is rejected
  if (req.headers.origin) {
    let oh = ''; try { oh = new URL(req.headers.origin).host } catch { }
    if (oh !== req.headers.host) return fail(res, 403, 'csrf', 'Ungültige Anfrage')
  }
  if (!p.startsWith('/api/')) {
    if (!['GET', 'HEAD'].includes(m) || !(p === '/' || p === '/index.html' || p.startsWith('/assets/'))) return send(res, 404, 'Not found', { 'Content-Type': 'text/plain' })
    return serveStatic(req, res, url)
  }

  // development helpers
  if (p === '/api/__mock/totp') return send(res, 200, { code: totp(auth.pendingSecret || auth.totpSecret || TEST_SECRET), secret: TEST_SECRET })
  if (p === '/api/__mock/expire') { sessions.clear(); for (const c of clients) c.end(); return send(res, 200, { ok: true }) }
  if (p === '/api/__mock/reset') { resetAuth(); for (const c of clients) c.end(); return send(res, 200, { ok: true }) }
  if (p === '/api/__mock/offline') { offlineUntil = Date.now() + Number(url.searchParams.get('s') || 20) * 1000; return send(res, 200, { ok: true }) }
  if (Date.now() < offlineUntil) { req.socket.destroy(); return }

  // CSRF: header on every non-GET/HEAD
  if (!['GET', 'HEAD'].includes(m) && req.headers['x-linkra'] !== '1') return fail(res, 403, 'csrf', 'Ungültige Anfrage')
  // JSON endpoints need Content-Type: application/json (ESPAsyncWebServer answers 404 otherwise)
  const JSON_ROUTES = ['/api/auth/setup', '/api/auth/login', '/api/auth/password', '/api/auth/totp/enable', '/api/auth/totp/disable']
  if (m === 'POST' && JSON_ROUTES.includes(p) && !String(req.headers['content-type'] || '').startsWith('application/json')) return send(res, 404, 'Not found', { 'Content-Type': 'text/plain' })

  let b = {}
  if ((m === 'POST' || m === 'PUT') && String(req.headers['content-type'] || '').startsWith('application/json')) { try { b = await body(req) } catch { return fail(res, 400, 'invalid', 'Ungültiger JSON-Body') } }
  else if (m === 'POST') { for await (const _ of req) { /* drain multipart uploads */ } }
  await new Promise(r => setTimeout(r, p.startsWith('/api/auth/') && m === 'POST' ? 220 : 15))  // PBKDF2 / network feel
  const s = session(req)

  // ----- public -----
  if (m === 'GET' && p === '/api/auth/status') return send(res, 200, statusJson(s))
  if (m === 'POST' && p === '/api/auth/setup') {
    if (auth.user) return fail(res, 409, 'exists', 'Es existiert bereits ein Konto')
    if (!/^[A-Za-z0-9._-]{3,32}$/.test(b.username || '')) return fail(res, 400, 'invalid', 'Benutzername: 3–32 Zeichen, nur A–Z, a–z, 0–9, . _ -')
    if (typeof b.password !== 'string' || b.password.length < 10 || b.password.length > 128) return fail(res, 400, 'invalid', 'Passwort: 10 bis 128 Zeichen')
    auth.user = b.username; auth.pwHash = sha(b.password)
    const c = newSession(auth.user)
    return send(res, 200, statusJson({ user: auth.user }), { 'Set-Cookie': c })
  }
  if (m === 'POST' && p === '/api/auth/login') {
    if (!auth.user) return fail(res, 409, 'setup_required', 'Noch kein Konto eingerichtet')
    const locked = lockState(ip)
    if (locked) return lockedReply(res, locked)
    if (!eq(b.username || '', auth.user) || !eq(sha(b.password || ''), auth.pwHash)) {
      registerFail(ip)
      return fail(res, 401, 'invalid_credentials', 'Benutzername oder Passwort falsch')
    }
    if (auth.totpActive) {
      const e = checkSecondFactor(b)
      if (e) {
        if (e[1] !== 'totp_required' && e[1] !== 'time_not_synced') registerFail(ip)
        return fail(res, e[0], e[1], e[2])
      }
    }
    clearFails(ip)
    return send(res, 200, statusJson({ user: auth.user }), { 'Set-Cookie': newSession(auth.user) })
  }

  // ----- everything else needs a session -----
  if (!s) return fail(res, 401, 'unauthorized', 'Anmeldung erforderlich')

  if (m === 'GET' && p === '/api/events') {
    res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache', Connection: 'keep-alive' })
    res.write('retry: 3000\n\n')
    clients.add(res)
    const tok = s.token
    const check = setInterval(() => { if (!sessions.has(tok)) res.end() }, 2000)
    req.on('close', () => { clients.delete(res); clearInterval(check) })
    return
  }
  // logout, logout-all and password change close every SSE stream; valid clients reconnect
  if (m === 'POST' && p === '/api/auth/logout') { sessions.delete(s.token); closeAllStreams(); return send(res, 200, { ok: true }, { 'Set-Cookie': clearCookie }) }
  if (m === 'POST' && p === '/api/auth/logout-all') { sessions.clear(); closeAllStreams(); return send(res, 200, { ok: true }, { 'Set-Cookie': clearCookie }) }
  if (m === 'POST' && p === '/api/auth/password') {
    const locked = lockState(ip)
    if (locked) return lockedReply(res, locked)
    if (!eq(sha(b.current || ''), auth.pwHash)) { registerFail(ip); return fail(res, 401, 'invalid_credentials', 'Aktuelles Passwort falsch') }
    if (typeof b.new !== 'string' || b.new.length < 10 || b.new.length > 128) return fail(res, 400, 'invalid', 'Neues Passwort: 10 bis 128 Zeichen')
    auth.pwHash = sha(b.new)
    for (const k of [...sessions.keys()]) if (k !== s.token) sessions.delete(k)
    closeAllStreams()
    return send(res, 200, { ok: true })
  }
  if (m === 'POST' && p === '/api/auth/totp/setup') {
    auth.pendingSecret = TEST_SECRET
    const label = encodeURIComponent(`Linkra Duo:${auth.user}`)
    return send(res, 200, { secret: TEST_SECRET, otpauth_uri: `otpauth://totp/${label}?secret=${TEST_SECRET}&issuer=Linkra%20Duo&algorithm=SHA1&digits=6&period=30` })
  }
  if (m === 'POST' && p === '/api/auth/totp/enable') {
    if (!auth.pendingSecret) return fail(res, 400, 'totp_not_pending', 'Keine Einrichtung ausstehend')
    if (!auth.timeSynced) return fail(res, 503, 'time_not_synced', 'Uhrzeit des Geräts noch nicht synchronisiert')
    const st = verify(auth.pendingSecret, b.code)
    if (st < 0) return fail(res, 400, 'totp_invalid', 'Code ungültig')
    auth.totpSecret = auth.pendingSecret; auth.pendingSecret = null; auth.totpActive = true; auth.lastStep = st
    auth.recovery = TEST_RECOVERY.map(sha)
    return send(res, 200, { recovery_codes: TEST_RECOVERY })
  }
  if (m === 'POST' && p === '/api/auth/totp/disable') {
    const locked = lockState(ip)
    if (locked) return lockedReply(res, locked)
    if (!eq(sha(b.password || ''), auth.pwHash)) { registerFail(ip); return fail(res, 401, 'invalid_credentials', 'Passwort falsch') }
    const e = checkSecondFactor({ totp: b.code, recovery_code: b.recovery_code })
    if (e) {
      if (e[1] !== 'totp_required' && e[1] !== 'time_not_synced') registerFail(ip)
      return fail(res, e[0] === 401 && e[1] === 'totp_invalid' ? 400 : e[0], e[1], e[2])
    }
    Object.assign(auth, { totpSecret: null, totpActive: false, recovery: [], lastStep: -1 })
    return send(res, 200, { ok: true })
  }
  return dataRoute(req, res, url, b)
})

server.listen(PORT, () => {
  console.log(`Linkra-Mock läuft auf http://localhost:${PORT}`)
  console.log(auth.user ? `Konto: ${TEST_USER} / ${TEST_PASSWORD}${auth.totpActive ? ' (2FA aktiv)' : ''}` : 'Kein Konto – die UI startet mit dem Einrichtungsassistenten')
  console.log(`TOTP-Testgeheimnis: ${TEST_SECRET}  ·  aktueller Code: GET /api/__mock/totp`)
  if (!auth.timeSynced) console.log('Uhrzeit "nicht synchron": TOTP-Anmeldung antwortet 503 time_not_synced')
})
